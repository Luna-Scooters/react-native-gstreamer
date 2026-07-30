//
//  gstreamer_event_recorder.c
//
//  See gstreamer_event_recorder.h.
//
//  Data flow:
//    video-tee ! queue ! videorate(10fps) ! videoconvert ! <h264 enc> ! h264parse ! appsink
//  appsink's new-sample callback (streaming thread) appends each encoded access
//  unit to a GOP ring. A 1s timer sends upstream force-key-unit events so the
//  ring is always cuttable near any point. On an event we remember the trigger
//  PTS; once the ring has buffered up to event+POST we extract the contiguous
//  slice [keyframe<=event-PRE .. event+POST] and mux it to MP4 in a throwaway
//  appsrc ! h264parse ! mp4mux ! filesink pipeline (no re-encode, no interaction
//  with the live pipeline).
//
//  NOTE (concurrent HW encoders): this branch encodes continuously while armed.
//  If the continuous ride recorder is also active, two H.264 encode sessions run
//  at once — fine on capable SoCs, but some mobile encoders allow only one. If
//  that bites on device, unify both onto a single shared encoder feeding two tees.
//

#include "gstreamer_event_recorder.h"
#include "gstreamer_backend.h"
#include "gstreamer_encoder.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

#define EVR_PRE_NS   (5 * GST_SECOND)
#define EVR_POST_NS  (7 * GST_SECOND)
#define EVR_FKU_MS   1000            // force a keyframe every second

typedef struct {
    GstElement  *bin;          // encode + appsink branch, NULL when disarmed
    GstElement  *enc_tee;      // shared encoder's tee we requested a pad from (borrowed)
    GstPad      *tee_pad;      // requested enc-tee src pad feeding the bin
    GstElement  *appsink;      // borrowed (owned by bin)
    guint        fku_timer_id; // periodic force-key-unit

    GMutex       lock;         // guards ring + the pending-save fields below
    GQueue      *ring;         // GstSample* (H.264 AUs), oldest first

    gboolean     pending;      // a save is waiting for POST seconds to buffer
    GstClockTime event_pts;    // trigger time
    GstClockTime save_until;   // event_pts + POST
    gchar       *pending_path; // owned
} RctGstEventRecorder;

static RctGstEventRecorder eventRecorder;
static gboolean er_inited = FALSE;

static void evr_ensure_init(void)
{
    if (!er_inited) {
        g_mutex_init(&eventRecorder.lock);
        eventRecorder.ring = g_queue_new();
        er_inited = TRUE;
    }
}

static gboolean sample_is_keyframe(GstSample *s)
{
    GstBuffer *b = gst_sample_get_buffer(s);
    return b && !GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
}

static GstClockTime sample_pts(GstSample *s)
{
    GstBuffer *b = gst_sample_get_buffer(s);
    return b ? GST_BUFFER_PTS(b) : GST_CLOCK_TIME_NONE;
}

// Drop whole leading GOPs while the ring spans more than we could ever need
// (PRE + POST + one GOP of slack). Caller holds the lock. Skipped while a save
// is pending so the slice can't be trimmed out from under us.
static void evr_trim_locked(void)
{
    const GstClockTime max_span = EVR_PRE_NS + EVR_POST_NS + GST_SECOND;
    for (;;) {
        GstSample *oldest = g_queue_peek_head(eventRecorder.ring);
        GstSample *newest = g_queue_peek_tail(eventRecorder.ring);
        if (!oldest || !newest || oldest == newest)
            return;
        GstClockTime o = sample_pts(oldest), n = sample_pts(newest);
        if (!GST_CLOCK_TIME_IS_VALID(o) || !GST_CLOCK_TIME_IS_VALID(n) || n < o)
            return;
        if (n - o <= max_span)
            return;
        // Only drop a keyframe (GOP boundary) so the ring always starts decodable.
        // Peek the second element: if it's a keyframe, we can drop the head.
        GstSample *head = g_queue_pop_head(eventRecorder.ring);
        GstSample *next = g_queue_peek_head(eventRecorder.ring);
        if (next && !sample_is_keyframe(next)) {
            // Not at a GOP boundary yet — push head back and stop (rare).
            g_queue_push_head(eventRecorder.ring, head);
            return;
        }
        gst_sample_unref(head);
    }
}

// Collect refs of [latest keyframe <= event_pts-PRE .. save_until] into a GList.
// Caller holds the lock.
static GList *evr_extract_slice_locked(void)
{
    GstClockTime want_from = (eventRecorder.event_pts > EVR_PRE_NS) ? eventRecorder.event_pts - EVR_PRE_NS : 0;

    // Find the start: last keyframe with pts <= want_from (fall back to first keyframe).
    GList *start = NULL;
    for (GList *l = eventRecorder.ring->head; l != NULL; l = l->next) {
        GstSample *s = (GstSample *)l->data;
        GstClockTime pts = sample_pts(s);
        if (!GST_CLOCK_TIME_IS_VALID(pts))
            continue;
        if (sample_is_keyframe(s) && pts <= want_from)
            start = l;                 // keep advancing to the latest such keyframe
        if (pts > want_from && start)
            break;
    }
    if (!start) {
        // No keyframe before the window; start at the first keyframe we have.
        for (GList *l = eventRecorder.ring->head; l != NULL; l = l->next) {
            if (sample_is_keyframe((GstSample *)l->data)) { start = l; break; }
        }
    }
    if (!start)
        return NULL;

    GList *slice = NULL;
    for (GList *l = start; l != NULL; l = l->next) {
        GstSample *s = (GstSample *)l->data;
        GstClockTime pts = sample_pts(s);
        if (GST_CLOCK_TIME_IS_VALID(pts) && pts > eventRecorder.save_until)
            break;
        slice = g_list_prepend(slice, gst_sample_ref(s));
    }
    return g_list_reverse(slice);
}

/* ---- Muxing the extracted slice (runs on the main loop) ---- */

typedef struct {
    GList  *slice;   // GstSample* refs, in order (first is a keyframe)
    gchar  *path;    // owned
    GstElement *mux_pipeline;
} EvrMuxJob;

static void evr_mux_job_free(EvrMuxJob *job)
{
    if (job->mux_pipeline) {
        gst_element_set_state(job->mux_pipeline, GST_STATE_NULL);
        gst_object_unref(job->mux_pipeline);
    }
    g_list_free_full(job->slice, (GDestroyNotify)gst_sample_unref);
    g_free(job->path);
    g_free(job);
}

static gboolean evr_mux_bus_cb(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    EvrMuxJob *job = (EvrMuxJob *)user_data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS: {
            g_print("Event clip saved -> %s\n", job->path);
            RctGstConfiguration *cfg = rct_gst_get_configuration();
            if (cfg->onEventSaved)
                cfg->onEventSaved(job->path);
            evr_mux_job_free(job);
            return FALSE;  // remove the watch
        }
        case GST_MESSAGE_ERROR: {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            g_printerr("Event clip mux error: %s (%s)\n",
                       err ? err->message : "?", dbg ? dbg : "");
            g_clear_error(&err); g_free(dbg);
            evr_mux_job_free(job);
            return FALSE;
        }
        default:
            return TRUE;
    }
}

// Builds the throwaway mux pipeline, pushes the slice + EOS. Main loop.
static gboolean evr_run_mux_job(gpointer data)
{
    EvrMuxJob *job = (EvrMuxJob *)data;
    if (!job->slice) { evr_mux_job_free(job); return G_SOURCE_REMOVE; }

    GError *error = NULL;
    gchar *desc = g_strdup_printf(
        "appsrc name=esrc is-live=false format=time do-timestamp=false ! "
        "h264parse config-interval=-1 ! mp4mux fragment-duration=1000 ! "
        "filesink name=esink async=false");
    job->mux_pipeline = gst_parse_launch(desc, &error);
    g_free(desc);
    if (!job->mux_pipeline) {
        g_printerr("Event clip: mux pipeline build failed: %s\n",
                   error ? error->message : "?");
        if (error) g_error_free(error);
        evr_mux_job_free(job);
        return G_SOURCE_REMOVE;
    }

    GstElement *esink = gst_bin_get_by_name(GST_BIN(job->mux_pipeline), "esink");
    g_object_set(esink, "location", job->path, NULL);
    gst_object_unref(esink);

    GstElement *esrc = gst_bin_get_by_name(GST_BIN(job->mux_pipeline), "esrc");
    // Match the caps the encoder produced so h264parse/mp4mux negotiate.
    GstSample *first = (GstSample *)job->slice->data;
    GstCaps *caps = gst_sample_get_caps(first);
    if (caps)
        g_object_set(esrc, "caps", caps, NULL);

    GstBus *bus = gst_element_get_bus(job->mux_pipeline);
    gst_bus_add_watch(bus, evr_mux_bus_cb, job);
    gst_object_unref(bus);

    gst_element_set_state(job->mux_pipeline, GST_STATE_PLAYING);

    // Rebase timestamps to 0 and push every buffer, then EOS.
    GstClockTime base = sample_pts(first);
    if (!GST_CLOCK_TIME_IS_VALID(base)) base = 0;
    for (GList *l = job->slice; l != NULL; l = l->next) {
        GstBuffer *src = gst_sample_get_buffer((GstSample *)l->data);
        GstBuffer *buf = gst_buffer_copy(src);
        GstClockTime pts = GST_BUFFER_PTS(buf);
        GST_BUFFER_PTS(buf) = (GST_CLOCK_TIME_IS_VALID(pts) && pts > base) ? pts - base : 0;
        GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;
        GstFlowReturn ret = GST_FLOW_OK;
        g_signal_emit_by_name(esrc, "push-buffer", buf, &ret);
        gst_buffer_unref(buf);
        if (ret != GST_FLOW_OK) {
            g_printerr("Event clip: push-buffer failed (%d)\n", ret);
            break;
        }
    }
    GstFlowReturn ret = GST_FLOW_OK;
    g_signal_emit_by_name(esrc, "end-of-stream", &ret);
    gst_object_unref(esrc);
    return G_SOURCE_REMOVE;  // teardown happens in the bus watch (EOS/ERROR)
}

/* ---- Capture branch ---- */

static GstFlowReturn evr_on_new_sample(GstElement *appsink, gpointer user_data)
{
    (void)user_data;
    GstSample *sample = NULL;
    g_signal_emit_by_name(appsink, "pull-sample", &sample);
    if (!sample)
        return GST_FLOW_OK;

    GList *slice = NULL;
    gchar *path = NULL;

    g_mutex_lock(&eventRecorder.lock);
    g_queue_push_tail(eventRecorder.ring, sample);  // ownership transferred from pull-sample

    if (eventRecorder.pending) {
        GstClockTime pts = sample_pts(sample);
        if (GST_CLOCK_TIME_IS_VALID(pts) && pts >= eventRecorder.save_until) {
            slice = evr_extract_slice_locked();
            path = eventRecorder.pending_path;
            eventRecorder.pending_path = NULL;
            eventRecorder.pending = FALSE;
        }
        // While pending we accumulate (no trim) so the slice stays intact.
    } else {
        evr_trim_locked();
    }
    g_mutex_unlock(&eventRecorder.lock);

    if (slice) {
        EvrMuxJob *job = g_new0(EvrMuxJob, 1);
        job->slice = slice;
        job->path = path;
        g_idle_add(evr_run_mux_job, job);
    }
    return GST_FLOW_OK;
}

// Periodically ask upstream (the encoder) for a keyframe — encoder-agnostic,
// avoids per-encoder key-interval property names.
static gboolean evr_force_keyframe(gpointer user_data)
{
    (void)user_data;
    if (!eventRecorder.appsink)
        return G_SOURCE_REMOVE;
    GstPad *pad = gst_element_get_static_pad(eventRecorder.appsink, "sink");
    if (pad) {
        gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(
            GST_CLOCK_TIME_NONE, TRUE, 0));
        gst_object_unref(pad);
    }
    return G_SOURCE_CONTINUE;
}

void rct_gst_event_recorder_set_buffering(gboolean enable)
{
    evr_ensure_init();

    if (!enable) {
        rct_gst_event_recorder_reset();
        return;
    }
    if (eventRecorder.bin)
        return;  // already buffering
    if (!pipeline) {
        g_printerr("event_recorder: pipeline not ready\n");
        return;
    }

    GstElement *enc_tee = rct_gst_encoder_acquire();
    if (!enc_tee) {
        g_printerr("event_recorder: shared encoder unavailable (pipeline not ready)\n");
        return;
    }
    eventRecorder.enc_tee = enc_tee;

    gchar *desc = g_strdup_printf(
        "queue max-size-buffers=0 max-size-bytes=0 max-size-time=%" G_GUINT64_FORMAT " leaky=downstream ! "
        "appsink name=evrsink emit-signals=true sync=false max-buffers=2 drop=false",
        (guint64)(EVR_PRE_NS + EVR_POST_NS + GST_SECOND));

    GError *error = NULL;
    eventRecorder.bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_free(desc);
    if (!eventRecorder.bin) {
        g_printerr("event_recorder: branch build failed: %s\n", error ? error->message : "?");
        if (error) g_error_free(error);
        return;
    }

    eventRecorder.appsink = gst_bin_get_by_name(GST_BIN(eventRecorder.bin), "evrsink");
    if (eventRecorder.appsink) {
        g_signal_connect(eventRecorder.appsink, "new-sample", G_CALLBACK(evr_on_new_sample), NULL);
        gst_object_unref(eventRecorder.appsink);  // borrowed; the bin owns it
    }

    gst_bin_add(GST_BIN(pipeline), eventRecorder.bin);
    eventRecorder.tee_pad = gst_element_request_pad_simple(enc_tee, "src_%u");
    GstPad *bin_sink = gst_element_get_static_pad(eventRecorder.bin, "sink");
    if (gst_pad_link(eventRecorder.tee_pad, bin_sink) != GST_PAD_LINK_OK) {
        g_printerr("event_recorder: failed to link tee -> branch\n");
        gst_object_unref(bin_sink);
        rct_gst_event_recorder_reset();
        return;
    }
    gst_object_unref(bin_sink);
    gst_element_sync_state_with_parent(eventRecorder.bin);

    eventRecorder.fku_timer_id = g_timeout_add(EVR_FKU_MS, evr_force_keyframe, NULL);
    g_print("Event recorder buffering started\n");
}

void rct_gst_event_recorder_save(const gchar *file_path)
{
    if (file_path == NULL || *file_path == '\0') {
        g_printerr("event_recorder save: no output path\n");
        return;
    }
    if (!eventRecorder.bin) {
        g_printerr("event_recorder save: not buffering\n");
        return;
    }

    g_mutex_lock(&eventRecorder.lock);
    if (eventRecorder.pending) {
        g_mutex_unlock(&eventRecorder.lock);
        g_printerr("event_recorder save: a clip is already pending\n");
        return;
    }
    GstSample *newest = g_queue_peek_tail(eventRecorder.ring);
    GstClockTime now = newest ? sample_pts(newest) : GST_CLOCK_TIME_NONE;
    if (!GST_CLOCK_TIME_IS_VALID(now)) {
        g_mutex_unlock(&eventRecorder.lock);
        g_printerr("event_recorder save: no buffered frames yet\n");
        return;
    }
    eventRecorder.event_pts = now;
    eventRecorder.save_until = now + EVR_POST_NS;
    eventRecorder.pending_path = g_strdup(file_path);
    eventRecorder.pending = TRUE;
    g_mutex_unlock(&eventRecorder.lock);
    g_print("Event recorder: clip pending -> %s\n", file_path);
}

gboolean rct_gst_event_recorder_is_buffering(void)
{
    return eventRecorder.bin != NULL;
}

void rct_gst_event_recorder_reset(void)
{
    if (!er_inited)
        return;

    if (eventRecorder.fku_timer_id) {
        g_source_remove(eventRecorder.fku_timer_id);
        eventRecorder.fku_timer_id = 0;
    }
    if (eventRecorder.bin) {
        gst_element_set_state(eventRecorder.bin, GST_STATE_NULL);
        GstObject *parent = gst_element_get_parent(eventRecorder.bin);
        if (parent) {
            gst_bin_remove(GST_BIN(parent), eventRecorder.bin);
            gst_object_unref(parent);
        } else {
            gst_object_unref(eventRecorder.bin);
        }
        eventRecorder.bin = NULL;
    }
    if (eventRecorder.enc_tee) {
        if (eventRecorder.tee_pad)
            gst_element_release_request_pad(eventRecorder.enc_tee, eventRecorder.tee_pad);
        eventRecorder.tee_pad = NULL;

        eventRecorder.enc_tee = NULL;
        gst_object_unref(eventRecorder.enc_tee);
        rct_gst_encoder_release();
        
    }
    eventRecorder.appsink = NULL;

    g_mutex_lock(&eventRecorder.lock);
    g_queue_foreach(eventRecorder.ring, (GFunc)gst_sample_unref, NULL);
    g_queue_clear(eventRecorder.ring);
    eventRecorder.pending = FALSE;
    g_free(eventRecorder.pending_path);
    eventRecorder.pending_path = NULL;
    g_mutex_unlock(&eventRecorder.lock);
}
