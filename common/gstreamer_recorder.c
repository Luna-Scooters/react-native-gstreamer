//
//  gstreamer_recorder.c
//

#include "gstreamer_recorder.h"
#include "gstreamer_backend.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;
extern GstElement *video_tee;

typedef struct {
    GstElement  *bin;            // queue ! ... ! filesink, NULL when idle
    GstPad      *tee_pad;        // requested tee src pad feeding the bin
    gchar       *file_path;      // owned copy of the output path
    GstClockTime pts_base;       // first PTS seen; rebases the file timeline to 0
    GstState     deferred_state; // pipeline state postponed until finalize completes
    guint        watchdog_id;    // forces teardown if EOS never reaches the filesink
} RctGstRecorder;

static RctGstRecorder recorder = {
    .file_path = NULL,
    .pts_base = GST_CLOCK_TIME_NONE,
    .deferred_state = GST_STATE_VOID_PENDING,
    .watchdog_id = 0,
};

// Pick the highest-ranked H.264 encoder, preferring a hardware one
static gchar *find_best_h264_encoder(void)
{
    GList *encoders = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_VIDEO_ENCODER, GST_RANK_MARGINAL);
    GstCaps *h264 = gst_caps_from_string("video/x-h264");
    GList *filtered = gst_element_factory_list_filter(encoders, h264, GST_PAD_SRC, FALSE);

    gchar *best_name = NULL;
    guint best_rank = 0;
    gboolean best_hw = FALSE;
    for (GList *l = filtered; l != NULL; l = l->next) {
        GstElementFactory *f = GST_ELEMENT_FACTORY(l->data);
        const gchar *klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
        gboolean hw = (klass && g_strstr_len(klass, -1, "Hardware")) ? TRUE : FALSE;
        guint rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(f));
        gboolean better = (best_name == NULL) || (hw && !best_hw) ||
                          (hw == best_hw && rank > best_rank);
        if (better) {
            g_free(best_name);
            best_name = g_strdup(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)));
            best_rank = rank;
            best_hw = hw;
        }
    }

    gst_caps_unref(h264);
    gst_plugin_feature_list_free(filtered);
    gst_plugin_feature_list_free(encoders);
    return best_name ? best_name : g_strdup("x264enc");
}

void rct_gst_recorder_reset(void)
{
    recorder.deferred_state = GST_STATE_VOID_PENDING;
    if (recorder.watchdog_id) {
        g_source_remove(recorder.watchdog_id);
        recorder.watchdog_id = 0;
    }
    if (recorder.bin) {
        gst_element_set_state(recorder.bin, GST_STATE_NULL);
        if (pipeline)
            gst_bin_remove(GST_BIN(pipeline), recorder.bin);
        recorder.bin = NULL;
    }
    if (recorder.tee_pad) {
        if (video_tee)
            gst_element_release_request_pad(video_tee, recorder.tee_pad);
        gst_object_unref(recorder.tee_pad);
        recorder.tee_pad = NULL;
    }
    g_free(recorder.file_path);
    recorder.file_path = NULL;
    recorder.pts_base = GST_CLOCK_TIME_NONE;
}

// Runs on the main loop once the recording branch has finished
static gboolean recorder_teardown(gpointer forced_by_watchdog)
{
    if (forced_by_watchdog)
        g_printerr("Recording finalize timed out, forcing teardown (file may be truncated)\n");

    gchar *finished_path = g_steal_pointer(&recorder.file_path);
    GstState deferred = recorder.deferred_state;

    rct_gst_recorder_reset();
    g_print("Recording stopped and file finalized\n");

    if (finished_path) {
        RctGstConfiguration *cfg = rct_gst_get_configuration();
        if (cfg->onRecordingFinished)
            cfg->onRecordingFinished(finished_path);
        g_free(finished_path);
    }
    // Recording is inactive now, so this cannot re-enter the defer path.
    if (deferred != GST_STATE_VOID_PENDING)
        rct_gst_set_pipeline_state(deferred);
    return G_SOURCE_REMOVE;
}

// Fires when EOS reaches the filesink: schedule teardown on the main loop.
static GstPadProbeReturn record_eos_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_EOS)
        return GST_PAD_PROBE_OK;

    g_idle_add(recorder_teardown, NULL);
    return GST_PAD_PROBE_REMOVE;
}

// Repair and rebase timestamps on buffers entering the record branch
// - The first PTS becomes the file's t=0
static GstPadProbeReturn record_stamp_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == NULL)
        return GST_PAD_PROBE_OK;

    GstClockTime pts = GST_BUFFER_PTS(buffer);

    if (pts == GST_CLOCK_TIME_NONE) {
        if (!pipeline)
            return GST_PAD_PROBE_OK;
        GstClock *clock = gst_element_get_clock(pipeline);
        if (!clock)
            return GST_PAD_PROBE_OK;
        GstClockTime now = gst_clock_get_time(clock);
        GstClockTime base = gst_element_get_base_time(pipeline);
        gst_object_unref(clock);
        if (now == GST_CLOCK_TIME_NONE || base == GST_CLOCK_TIME_NONE || now < base)
            return GST_PAD_PROBE_OK;
        pts = now - base;
    }

    if (recorder.pts_base == GST_CLOCK_TIME_NONE)
        recorder.pts_base = pts;
    pts = (pts > recorder.pts_base) ? pts - recorder.pts_base : 0;

    buffer = gst_buffer_make_writable(buffer);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

// Fires when the tee src pad is idle: unlink the branch and push EOS into it so
// mp4mux finalizes the file. Rendering keeps flowing through the other tee pad.
static GstPadProbeReturn record_unlink_probe(GstPad *tee_pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)info; (void)user_data;
    if (!recorder.bin)
        return GST_PAD_PROBE_REMOVE;

    GstPad *bin_sink = gst_element_get_static_pad(recorder.bin, "sink");
    gst_pad_unlink(tee_pad, bin_sink);
    gst_pad_send_event(bin_sink, gst_event_new_eos());
    gst_object_unref(bin_sink);
    return GST_PAD_PROBE_REMOVE;
}

void rct_gst_start_recording(const gchar *file_path, gint width, gint height, gint fps)
{
    if (rct_gst_is_recording()) {
        g_printerr("start_recording: already recording\n");
        return;
    }
    if (!video_tee || !pipeline) {
        g_printerr("start_recording: no video-tee available (pipeline not ready)\n");
        return;
    }

    gchar *enc = find_best_h264_encoder();
    g_print("Recording with encoder: %s\n", enc);

    // Don't pin the raw format: let videoconvert negotiate whatever the chosen
    // encoder accepts (amcvidenc prefers NV12, x264enc prefers I420, etc.).
    GString *caps = g_string_new("video/x-raw");
    if (width > 0)  g_string_append_printf(caps, ",width=%d", width);
    if (height > 0) g_string_append_printf(caps, ",height=%d", height);
    if (fps > 0)    g_string_append_printf(caps, ",framerate=%d/1", fps);

    // Fragmented MP4 (1s fragments): media hits the disk continuously, so a crash,
    // kill or forced teardown mid-ride still leaves a playable file. faststart
    // would buffer everything and only write at EOS — any interruption = 0 bytes.
    gchar *desc = g_strdup_printf(
        "queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 leaky=downstream ! "
        "videorate ! videoscale ! videoconvert ! %s ! "
        "%s ! h264parse config-interval=-1 ! mp4mux fragment-duration=1000 ! "
        "filesink name=recsink async=false location=\"%s\"",
        caps->str, enc, file_path);

    GError *error = NULL;
    recorder.bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_string_free(caps, TRUE);
    g_free(desc);
    g_free(enc);
    if (!recorder.bin) {
        g_printerr("start_recording: failed to build record bin: %s\n",
                   error ? error->message : "unknown");
        if (error) g_error_free(error);
        return;
    }

    GstElement *recsink = gst_bin_get_by_name(GST_BIN(recorder.bin), "recsink");
    if (recsink) {
        GstPad *sinkpad = gst_element_get_static_pad(recsink, "sink");
        gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                          record_eos_probe, NULL, NULL);
        gst_object_unref(sinkpad);
        gst_object_unref(recsink);
    }

    // Attach the branch to the tee, live. The stamp probe (rebase/PTS repair)
    // rides on the requested pad and dies with it.
    gst_bin_add(GST_BIN(pipeline), recorder.bin);
    recorder.tee_pad = gst_element_request_pad_simple(video_tee, "src_%u");
    gst_pad_add_probe(recorder.tee_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      record_stamp_probe, NULL, NULL);
    GstPad *bin_sink = gst_element_get_static_pad(recorder.bin, "sink");
    if (gst_pad_link(recorder.tee_pad, bin_sink) != GST_PAD_LINK_OK) {
        g_printerr("start_recording: failed to link tee -> record bin\n");
        gst_object_unref(bin_sink);
        rct_gst_recorder_reset();
        return;
    }
    gst_object_unref(bin_sink);
    gst_element_sync_state_with_parent(recorder.bin);

    recorder.file_path = g_strdup(file_path);
    g_print("Recording started -> %s\n", file_path);
}

void rct_gst_stop_recording(void)
{
    if (!rct_gst_is_recording()) {
        g_printerr("stop_recording: not recording\n");
        return;
    }
    // A non-zero watchdog means a finalize is already in progress.
    if (recorder.watchdog_id != 0)
        return;
    recorder.watchdog_id = g_timeout_add(3000, recorder_teardown, GINT_TO_POINTER(TRUE));

    gst_pad_add_probe(recorder.tee_pad, GST_PAD_PROBE_TYPE_IDLE,
                      record_unlink_probe, NULL, NULL);
}

gboolean rct_gst_is_recording(void)
{
    return recorder.file_path != NULL;
}

void rct_gst_recorder_defer_state(GstState state)
{
    recorder.deferred_state = state;
    rct_gst_stop_recording();
}
