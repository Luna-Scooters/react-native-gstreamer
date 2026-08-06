//
//  gstreamer_event_recorder.c
//
//  See gstreamer_event_recorder.h.
//
//  Backlog recording, the canonical GStreamer way. Off the shared encoder's
//  enc-tee we keep a leaky queue whose SRC pad is held by a blocking probe:
//
//    enc-tee ! queue name=evq (leaky, holds ~PRE s)   [src pad BLOCKED]
//
//  Blocked, the queue can't push, so it just fills and leaks — always holding the
//  most recent PRE seconds of encoded H.264. On a trigger we:
//    1. build a fresh  mp4mux ! filesink  and link it after the queue,
//    2. drop the one buffer parked in the block probe, then drop delta frames
//       until the first keyframe (so the MP4 opens on an IDR),
//    3. remove the block probe: the backlog flushes into the muxer, then live
//       frames follow for POST seconds,
//    4. re-block the queue and push EOS into the muxer to finalize the file.
//

#include "gstreamer_event_recorder.h"
#include "gstreamer_backend.h"
#include "gstreamer_encoder.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

#define EVR_FKU_MS   1000            // force a keyframe every second
#define EVR_BACKLOG_SLACK_SEC 1                    // keep a bit more than PRE so the
                                                   // keyframe drop still leaves ~PRE s
static const GstClockTime EVR_STATE_TIMEOUT    = 200 * GST_MSECOND;
static const GstClockTime EVR_FINALIZE_TIMEOUT = 3 * GST_SECOND;

typedef struct {
    GstElement  *event_queue;       // persistent leaky backlog queue, NULL when disarmed
    GstPad      *enc_tee_pad;       // src pad tapping the shared encoder, feeding evq
    GstPad      *event_queue_src;   // evq src pad — where we block / gate / push EOS
    gulong       block_id;          // block probe holding the backlog (armed-idle)
    gulong       gate_id;           // keyframe-gate probe active during a clip's start
    guint        fku_timer_id;      // periodic force-key-unit while armed
    guint        video_pre_length;  // seconds kept before the trigger
    guint        video_post_length; // seconds recorded after the trigger

    // Per-clip (one event capture at a time):
    GstElement  *record_bin;    // fresh mp4mux ! filesink, NULL when idle
    guint        buffer_count;  // drives the drop-one + keyframe gate
    guint        stop_id;       // POST timer that ends the clip
    guint        watchdog_id;   // forces teardown if EOS never reaches the filesink
    gchar       *pending_path;  // owned, the current clip's output path
} RctGstEventRecorder;

static RctGstEventRecorder eventRecorder;

/* ---- probes ---- */

// Holds the queue's src pad closed, so the queue fills but nothing flows
static GstPadProbeReturn evr_block_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)info; (void)user_data;
    return GST_PAD_PROBE_OK;
}

// Runs when the backlog is unblocked. Drops delta frames until the first keyframe 
// so the MP4 opens on an IDR. 
static GstPadProbeReturn evr_gate_probe_keyframe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf == NULL)
        return GST_PAD_PROBE_OK;

    if (eventRecorder.buffer_count++ == 0)
        return GST_PAD_PROBE_DROP;   // the stale buffer parked in the block probe

    if (!GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) {
        eventRecorder.gate_id = 0;              // keyframe: let it through and remove the gate
        return GST_PAD_PROBE_REMOVE;
    }
    return GST_PAD_PROBE_DROP;        // delta frame: keep waiting for a keyframe
}

static gboolean evr_finalize(gpointer forced_by_watchdog);

// Fires when EOS reaches the filesink: the file is finalized — schedule teardown.
static GstPadProbeReturn evr_eos_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS)
        return GST_PAD_PROBE_OK;
    g_idle_add(evr_finalize, NULL);
    return GST_PAD_PROBE_REMOVE;
}

/* ---- clip lifecycle ---- */

// Tear the current clip's mux+filesink down (leaves the armed backlog intact).
static void evr_drop_record_bin(void)
{
    if (eventRecorder.event_queue_src) {
        GstPad *peer = gst_pad_get_peer(eventRecorder.event_queue_src);
        if (peer) {
            gst_pad_unlink(eventRecorder.event_queue_src, peer);
            gst_object_unref(peer);
        }
    }
    if (eventRecorder.record_bin) {
        gst_element_set_state(eventRecorder.record_bin, GST_STATE_NULL);
        if (pipeline)
            gst_bin_remove(GST_BIN(pipeline), eventRecorder.record_bin);
        eventRecorder.record_bin = NULL;
    }
}

// Main loop, once the clip's file is finalized (EOS) or the watchdog fires.
static gboolean evr_finalize(gpointer forced_by_watchdog)
{
    if (!eventRecorder.record_bin)
        return G_SOURCE_REMOVE;   // already finalized
    if (forced_by_watchdog)
        g_printerr("Event clip finalize timed out, forcing teardown (file may be truncated)\n");
    if (eventRecorder.watchdog_id) {
        g_source_remove(eventRecorder.watchdog_id);
        eventRecorder.watchdog_id = 0;
    }

    evr_drop_record_bin();

    gchar *path = eventRecorder.pending_path;
    eventRecorder.pending_path = NULL;
    if (path) {
        g_print("Event clip saved -> %s\n", path);
        RctGstConfiguration *cfg = rct_gst_get_configuration();
        if (cfg->onEventSaved)
            cfg->onEventSaved(path);
        g_free(path);
    }
    return G_SOURCE_REMOVE;
}

// Pushes EOS into the muxer to finalize the file. Done on its own thread because
// gst_pad_send_event(EOS) travels synchronously through mux + filesink.
static gpointer evr_push_eos(gpointer data)
{
    GstPad *mux_sink = data;   // reffed by the caller
    gst_pad_send_event(mux_sink, gst_event_new_eos());
    gst_object_unref(mux_sink);
    return NULL;
}

// POST elapsed: stop the clip. Re-block the backlog so no more frames enter the
// muxer, then EOS the muxer to finalize. Main loop.
static gboolean evr_stop(gpointer user_data)
{
    (void)user_data;
    eventRecorder.stop_id = 0;
    if (!eventRecorder.record_bin || !eventRecorder.event_queue_src)
        return G_SOURCE_REMOVE;

    // Back to holding a rolling backlog.
    eventRecorder.block_id = gst_pad_add_probe(eventRecorder.event_queue_src,
        GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER, evr_block_probe, NULL, NULL);
    if (eventRecorder.gate_id) {   // gate never saw a keyframe (shouldn't happen with the FKU)
        gst_pad_remove_probe(eventRecorder.event_queue_src, eventRecorder.gate_id);
        eventRecorder.gate_id = 0;
    }

    GstPad *mux_sink = gst_pad_get_peer(eventRecorder.event_queue_src);   // the record bin's sink
    if (mux_sink)
        g_thread_new("evr-eos", evr_push_eos, mux_sink);

    eventRecorder.watchdog_id = g_timeout_add((guint)(EVR_FINALIZE_TIMEOUT / GST_MSECOND),
                                   evr_finalize, GINT_TO_POINTER(TRUE));
    return G_SOURCE_REMOVE;
}

// Periodically ask the shared encoder for a keyframe so clips can start close to
// event-PRE.
static gboolean evr_force_keyframe(gpointer user_data)
{
    (void)user_data;
    if (!eventRecorder.event_queue)
        return G_SOURCE_REMOVE;
    rct_gst_encoder_force_keyframe();
    return G_SOURCE_CONTINUE;
}

/* ---- public API ---- */

void rct_gst_event_recorder_set_buffering(gboolean enable, gint video_pre_length, gint video_post_length)
{
    if (!enable) {
        rct_gst_event_recorder_reset();
        return;
    }
    if (eventRecorder.event_queue)
        return;   // already armed
    if (!pipeline) {
        g_printerr("event_recorder: pipeline not ready\n");
        return;
    }

    eventRecorder.video_pre_length  = (video_pre_length  > 0) ? (guint)video_pre_length  : 0;
    eventRecorder.video_post_length = (video_post_length > 0) ? (guint)video_post_length : 0;

    // Leaky backlog: always keeps the most recent PRE (+slack) seconds.
    eventRecorder.event_queue = gst_element_factory_make("queue", "evq");
    g_object_set(eventRecorder.event_queue,
        "max-size-buffers", (guint)0,
        "max-size-bytes",   (guint)0,
        "max-size-time",    (guint64)(eventRecorder.video_pre_length + EVR_BACKLOG_SLACK_SEC) * GST_SECOND,
        "leaky",            2,   // 2 = downstream: drop the OLDEST when full
        NULL);
    gst_bin_add(GST_BIN(pipeline), eventRecorder.event_queue);

    // Block the queue's output, then start it: it fills but records nothing.
    eventRecorder.event_queue_src = gst_element_get_static_pad(eventRecorder.event_queue, "src");
    eventRecorder.block_id = gst_pad_add_probe(eventRecorder.event_queue_src,
        GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER, evr_block_probe, NULL, NULL);
    gst_element_sync_state_with_parent(eventRecorder.event_queue);
    gst_element_get_state(eventRecorder.event_queue, NULL, NULL, EVR_STATE_TIMEOUT);

    // Link the shared encoder in only once evq is running (avoids a flushing
    // race on the shared streaming thread).
    eventRecorder.enc_tee_pad = rct_gst_encoder_request_src_pad();
    if (!eventRecorder.enc_tee_pad) {
        g_printerr("event_recorder: encoder gave no src pad\n");
        rct_gst_event_recorder_reset();
        return;
    }
    GstPad *event_queue_sink = gst_element_get_static_pad(eventRecorder.event_queue, "sink");
    if (gst_pad_link(eventRecorder.enc_tee_pad, event_queue_sink) != GST_PAD_LINK_OK) {
        g_printerr("event_recorder: failed to link enc-tee -> backlog queue\n");
        gst_object_unref(event_queue_sink);
        rct_gst_event_recorder_reset();
        return;
    }
    gst_object_unref(event_queue_sink);

    eventRecorder.fku_timer_id = g_timeout_add(EVR_FKU_MS, evr_force_keyframe, NULL);
    g_print("Event recorder armed (backlog %us, post %us)\n",
            eventRecorder.video_pre_length, eventRecorder.video_post_length);
}

void rct_gst_event_recorder_save(const gchar *file_path)
{
    if (file_path == NULL || *file_path == '\0') {
        g_printerr("event save: no output path\n");
        return;
    }
    if (!eventRecorder.event_queue || !eventRecorder.event_queue_src) {
        g_printerr("event save: not buffering\n");
        return;
    }
    if (eventRecorder.record_bin) {
        g_printerr("event save: a clip is already recording\n");
        return;
    }
    if (!pipeline) {
        g_printerr("event save: pipeline not ready\n");
        return;
    }

    gchar *desc = g_strdup_printf(
        "h264parse ! mp4mux name=evmux fragment-duration=1000 ! "
        "filesink name=evsink async=false location=\"%s\"",
        file_path);
    GError *error = NULL;
    eventRecorder.record_bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_free(desc);
    if (!eventRecorder.record_bin) {
        g_printerr("event save: record bin build failed: %s\n", error ? error->message : "?");
        if (error) g_error_free(error);
        return;
    }

    eventRecorder.pending_path = g_strdup(file_path);
    eventRecorder.buffer_count = 0;

    GstElement *evsink = gst_bin_get_by_name(GST_BIN(eventRecorder.record_bin), "evsink");
    if (evsink) {
        GstPad *sp = gst_element_get_static_pad(evsink, "sink");
        gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, evr_eos_probe, NULL, NULL);
        gst_object_unref(sp);
        gst_object_unref(evsink);
    }

    // Attach the muxer after the (still blocked) backlog queue and bring it up
    // BEFORE unblocking, so data only flows once the muxer is ready.
    gst_bin_add(GST_BIN(pipeline), eventRecorder.record_bin);
    GstPad *bin_sink = gst_element_get_static_pad(eventRecorder.record_bin, "sink");
    if (gst_pad_link(eventRecorder.event_queue_src, bin_sink) != GST_PAD_LINK_OK) {
        g_printerr("event save: failed to link backlog queue -> record bin\n");
        gst_object_unref(bin_sink);
        evr_drop_record_bin();
        g_free(eventRecorder.pending_path); eventRecorder.pending_path = NULL;
        return;
    }
    gst_object_unref(bin_sink);
    gst_element_sync_state_with_parent(eventRecorder.record_bin);
    gst_element_get_state(eventRecorder.record_bin, NULL, NULL, EVR_STATE_TIMEOUT);

    // Gate the start on a keyframe, then unblock: the backlog flushes into the mux.
    eventRecorder.gate_id = gst_pad_add_probe(eventRecorder.event_queue_src, GST_PAD_PROBE_TYPE_BUFFER,
                                   evr_gate_probe_keyframe, NULL, NULL);
    if (eventRecorder.block_id) {
        gst_pad_remove_probe(eventRecorder.event_queue_src, eventRecorder.block_id);
        eventRecorder.block_id = 0;
    }

    eventRecorder.stop_id = g_timeout_add(eventRecorder.video_post_length * 1000, evr_stop, NULL);
    g_print("Event clip -> %s (backlog %us + %us live)\n",
            file_path, eventRecorder.video_pre_length, eventRecorder.video_post_length);
}

gboolean rct_gst_event_recorder_is_buffering(void)
{
    return eventRecorder.event_queue != NULL;
}

void rct_gst_event_recorder_reset(void)
{
    if (eventRecorder.fku_timer_id)      { g_source_remove(eventRecorder.fku_timer_id);      eventRecorder.fku_timer_id = 0; }
    if (eventRecorder.stop_id)     { g_source_remove(eventRecorder.stop_id);     eventRecorder.stop_id = 0; }
    if (eventRecorder.watchdog_id) { g_source_remove(eventRecorder.watchdog_id); eventRecorder.watchdog_id = 0; }

    // Abandon any in-progress clip (no onEventSaved; fMP4 leaves it playable).
    evr_drop_record_bin();
    g_free(eventRecorder.pending_path);
    eventRecorder.pending_path = NULL;

    if (eventRecorder.event_queue_src) {
        eventRecorder.gate_id = 0;
        eventRecorder.block_id = 0;
        gst_object_unref(eventRecorder.event_queue_src);
        eventRecorder.event_queue_src = NULL;
    }
    if (eventRecorder.enc_tee_pad) {
        rct_gst_encoder_release_src_pad(eventRecorder.enc_tee_pad);
        eventRecorder.enc_tee_pad = NULL;
    }
    if (eventRecorder.event_queue) {
        gst_element_set_state(eventRecorder.event_queue, GST_STATE_NULL);
        if (pipeline)
            gst_bin_remove(GST_BIN(pipeline), eventRecorder.event_queue);
        eventRecorder.event_queue = NULL;
    }
}
