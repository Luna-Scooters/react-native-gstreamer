//
//  gstreamer_recorder.c
//

#include "gstreamer_recorder.h"
#include "gstreamer_backend.h"
#include "gstreamer_encoder.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

static const GstClockTime RECORD_BIN_STATE_TIMEOUT = 100 * GST_MSECOND;


typedef struct {
    GstElement  *bin;            // queue ! mp4mux ! filesink, NULL when idle
    GstElement  *enc_tee;        // shared encoder's tee we requested a pad from (borrowed)
    GstPad      *tee_pad;        // requested enc-tee src pad feeding the bin
    GstClockTime pts_base;       // first PTS seen; rebases the file timeline to 0
    GstState     deferred_state; // pipeline state postponed until finalize completes
    guint        watchdog_id;    // forces teardown if EOS never reaches the filesink
} RctGstRecorder;

static RctGstRecorder recorder = {
    .pts_base = GST_CLOCK_TIME_NONE,
    .deferred_state = GST_STATE_VOID_PENDING,
    .watchdog_id = 0,
};

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
    if (recorder.enc_tee) {
        if (recorder.tee_pad) {
            gst_element_release_request_pad(recorder.enc_tee, recorder.tee_pad);
            recorder.tee_pad = NULL;
        }

        gst_object_unref(recorder.enc_tee);
        recorder.enc_tee = NULL;
        rct_gst_encoder_release();
    }
    recorder.pts_base = GST_CLOCK_TIME_NONE;
}

// Runs on the main loop once the recording branch has finished
static gboolean recorder_teardown(gpointer forced_by_watchdog)
{
    if (forced_by_watchdog)
        g_printerr("Recording finalize timed out, forcing teardown (file may be truncated)\n");

    GstState deferred = recorder.deferred_state;

    rct_gst_recorder_reset();
    g_print("Recording stopped and file finalized\n");

    RctGstConfiguration *cfg = rct_gst_get_configuration();
    if (cfg->onRecordingFinished)
        cfg->onRecordingFinished();

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

// Rebase the file timeline so it starts at 0. Buffers are already encoded H.264
// with valid PTS/DTS (PTS repair happened at the shared encoder's input), so we
// only shift by the first PTS — shifting DTS by the same base to stay monotonic.
static GstPadProbeReturn record_stamp_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == NULL)
        return GST_PAD_PROBE_OK;

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return GST_PAD_PROBE_OK;

    if (recorder.pts_base == GST_CLOCK_TIME_NONE)
        recorder.pts_base = pts;
    GstClockTime base = recorder.pts_base;

    buffer = gst_buffer_make_writable(buffer);
    GST_BUFFER_PTS(buffer) = (pts > base) ? pts - base : 0;
    if (GST_BUFFER_DTS_IS_VALID(buffer)) {
        GstClockTime dts = GST_BUFFER_DTS(buffer);
        GST_BUFFER_DTS(buffer) = (dts > base) ? dts - base : 0;
    }
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

// Fires when the tee src pad is idle: unlink the branch and push EOS into it so
// mp4mux finalizes the file. The encoder keeps running for other consumers.
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

void rct_gst_start_recording(const gchar *file_path)
{
    if (file_path == NULL || *file_path == '\0') {
        g_printerr("start_recording: no output path provided\n");
        return;
    }
    if (rct_gst_is_recording()) {
        g_printerr("start_recording: already recording\n");
        return;
    }
    if (!pipeline) {
        g_printerr("start_recording: pipeline not ready\n");
        return;
    }

    // Bring up (or join) the shared encoder and attach our mux branch to its tee.
    GstElement *enc_tee = rct_gst_encoder_acquire();
    if (!enc_tee) {
        g_printerr("start_recording: shared encoder unavailable (pipeline not ready)\n");
        return;
    }
    recorder.enc_tee = enc_tee;

    // Fragmented MP4 (1s fragments): media hits the disk continuously, so a crash,
    // kill or forced teardown mid-ride still leaves a playable file.
    gchar *desc = g_strdup_printf(
        "queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 leaky=downstream ! "
        "mp4mux fragment-duration=1000 ! "
        "filesink name=recsink async=false location=\"%s\"",
        file_path);

    GError *error = NULL;
    recorder.bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_free(desc);
    if (!recorder.bin) {
        g_printerr("start_recording: failed to build record bin: %s\n",
                   error ? error->message : "unknown");
        if (error) g_error_free(error);
        rct_gst_recorder_reset();
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

    // Attach the mux branch to the shared encoder's tee, live. The rebase probe
    // rides on the requested pad and dies with it.
    gst_bin_add(GST_BIN(pipeline), recorder.bin);
    gst_element_sync_state_with_parent(recorder.bin);
    GstStateChangeReturn state_ret =
        gst_element_get_state(recorder.bin, NULL, NULL, RECORD_BIN_STATE_TIMEOUT);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("start_recording: record bin failed to start, aborting\n");
        rct_gst_recorder_reset();
        return;
    }
    else if (state_ret == GST_STATE_CHANGE_ASYNC) {
        g_printerr("start_recording: record bin still changing state after %dms, "
                   "linking anyway\n", (gint)(RECORD_BIN_STATE_TIMEOUT / GST_MSECOND));
    }
    recorder.tee_pad = gst_element_request_pad_simple(enc_tee, "src_%u");
    gst_pad_add_probe(recorder.tee_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      record_stamp_probe, NULL, NULL);
    GstPad *bin_sink = gst_element_get_static_pad(recorder.bin, "sink");
    if (gst_pad_link(recorder.tee_pad, bin_sink) != GST_PAD_LINK_OK) {
        g_printerr("start_recording: failed to link enc-tee -> record bin\n");
        gst_object_unref(bin_sink);
        rct_gst_recorder_reset();
        return;
    }
    gst_object_unref(bin_sink);

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
    return recorder.pts_base != GST_CLOCK_TIME_NONE;
}

void rct_gst_recorder_defer_state(GstState state)
{
    recorder.deferred_state = state;
    rct_gst_stop_recording();
}
