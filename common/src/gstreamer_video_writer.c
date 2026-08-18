//
//  gstreamer_video_writer.c
//
//  See gstreamer_video_writer.h. Owns the muxing tail
//  (queue ! h264parse ! mp4mux ! filesink) plus the finalize dance shared by the
//  trip and event recorders:
//
//    src ! [ queue ! h264parse ! mp4mux fragment-duration=1000 ! filesink ]
//
//  On close: push EOS into the muxer (own thread) so it writes the trailer; an
//  EOS probe on the filesink schedules teardown on the main loop and fires
//  on_done(path). A 3s watchdog forces teardown if EOS never arrives.
//

#include "gstreamer_video_writer.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

static const guint WRITER_FINALIZE_TIMEOUT_MS = 3000;
static const GstClockTime WRITER_STATE_TIMEOUT = 100 * GST_MSECOND;

// Main loop: tear the branch down and fire on_done. Reached from the filesink EOS
// probe (normal) or the watchdog (forced). Idempotent via the rec_bin guard.
static gboolean writer_finalize(gpointer user_data)
{
    RctGstVideoWriter *writer = (RctGstVideoWriter *)user_data;
    if (!writer->rec_bin)
        return G_SOURCE_REMOVE;   // already finalized

    if (writer->watchdog_id) {
        g_source_remove(writer->watchdog_id);
        writer->watchdog_id = 0;
    }

    // Set NULL before removing: a late push from the source hits a flushing pad
    // (graceful) instead of a not-linked error.
    gst_element_set_state(writer->rec_bin, GST_STATE_NULL);
    if (pipeline)
        gst_bin_remove(GST_BIN(pipeline), writer->rec_bin);
    writer->rec_bin = NULL;

    gchar *path = writer->file_path;
    writer->file_path = NULL;
    if (writer->on_done)
        writer->on_done(path);
    g_free(path);
    return G_SOURCE_REMOVE;
}

// Fires when EOS reaches the filesink: the file is finalized — schedule teardown.
static GstPadProbeReturn writer_eos_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS)
        return GST_PAD_PROBE_OK;
    g_idle_add(writer_finalize, user_data);
    return GST_PAD_PROBE_REMOVE;
}

// Pushes EOS into the muxer on its own thread. gst_pad_send_event(EOS) travels
// synchronously through mux + filesink (it writes the trailer), so doing it off
// the main loop avoids a stall. Works whether the source is flowing (trip) or
// held by a block probe (event) — unlike an idle probe, which never fires on a
// block-held pad.
static gpointer writer_push_eos(gpointer data)
{
    GstPad *bin_sink = (GstPad *)data;   // reffed by the caller
    gst_pad_send_event(bin_sink, gst_event_new_eos());
    gst_object_unref(bin_sink);
    return NULL;
}

gboolean writer_start(RctGstVideoWriter *writer, GstPad *src, const gchar *path, void (*done)(gchar *))
{
    if (!writer || !src || !path || !*path || !pipeline) {
        g_printerr("writer_start: bad args or pipeline not ready\n");
        return FALSE;
    }

    writer->rec_bin = NULL;
    writer->watchdog_id = 0;
    writer->on_done = done;
    writer->file_path = g_strdup(path);

    // Front queue decouples filesink's disk I/O from the source's streaming thread
    // (a slow write must not stall the shared encoder / the other consumers), and
    // — being the element the source links to — lets the whole branch tear down
    // atomically, so the source is never left flowing into an orphaned pad.
    // h264parse before mp4mux: its video caps steer mp4mux to a VIDEO pad (without
    // it mp4mux picks an audio pad and the link fails with NOFORMAT). Fragmented
    // MP4 keeps the file playable through crashes/forced teardown. We do NOT touch
    // PTS/DTS — mp4mux normalizes the timeline, and the encoder emits reordered
    // (B-frame) DTS that must stay monotonic.
    gchar *desc = g_strdup_printf(
        "queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 leaky=downstream ! "
        "h264parse ! mp4mux fragment-duration=1000 "
        "fragment-mode=first-moov-then-finalise ! "
        "filesink name=recsink async=false location=\"%s\"",
        path);
    GError *error = NULL;
    writer->rec_bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_free(desc);
    if (!writer->rec_bin) {
        g_printerr("writer_start: failed to build record bin: %s\n",
                   error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_free(writer->file_path);
        writer->file_path = NULL;
        return FALSE;
    }

    GstElement *recsink = gst_bin_get_by_name(GST_BIN(writer->rec_bin), "recsink");
    if (!recsink) {
        g_printerr("writer_start: recsink not found in record bin\n");
        gst_object_unref(writer->rec_bin);
        writer->rec_bin = NULL;
        g_free(writer->file_path);
        writer->file_path = NULL;
        return FALSE;
    }
    GstPad *sinkpad = gst_element_get_static_pad(recsink, "sink");
    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      writer_eos_probe, writer, NULL);
    gst_object_unref(sinkpad);
    gst_object_unref(recsink);

    // Bring the branch to PLAYING BEFORE linking, so the source only pushes into a
    // ready muxer (linking first can wedge a live streaming thread with FLUSHING).
    gst_bin_add(GST_BIN(pipeline), writer->rec_bin);
    gst_element_sync_state_with_parent(writer->rec_bin);
    gst_element_get_state(writer->rec_bin, NULL, NULL, WRITER_STATE_TIMEOUT);

    GstPad *bin_sink = gst_element_get_static_pad(writer->rec_bin, "sink");
    GstPadLinkReturn link_ret = gst_pad_link(src, bin_sink);
    gst_object_unref(bin_sink);
    if (link_ret != GST_PAD_LINK_OK) {
        g_printerr("writer_start: failed to link src -> writer (%d)\n", link_ret);
        gst_element_set_state(writer->rec_bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(pipeline), writer->rec_bin);
        writer->rec_bin = NULL;
        g_free(writer->file_path);
        writer->file_path = NULL;
        return FALSE;
    }
    return TRUE;
}

void writer_close(RctGstVideoWriter *writer)
{
    if (!writer || !writer->rec_bin || writer->watchdog_id != 0)
        return;   // nothing to close, or a finalize is already in progress

    writer->watchdog_id = g_timeout_add(WRITER_FINALIZE_TIMEOUT_MS, writer_finalize, writer);

    // Push EOS into the muxer -> the sink EOS probe runs writer_finalize. 
    // The caller should have already stopped feeding the writer (trip:
    // stopping; event: backlog re-blocked), so no data races the EOS.
    GstPad *bin_sink = gst_element_get_static_pad(writer->rec_bin, "sink");
    GThread *eos_thread = g_thread_new("writer-eos", writer_push_eos, bin_sink);
    g_thread_unref(eos_thread);   // detached; it unrefs bin_sink and self-cleans
}

void writer_reset(RctGstVideoWriter *writer)
{
    if (!writer)
        return;
    if (writer->watchdog_id) {
        g_source_remove(writer->watchdog_id);
        writer->watchdog_id = 0;
    }
    if (writer->rec_bin) {
        gst_element_set_state(writer->rec_bin, GST_STATE_NULL);
        if (pipeline)
            gst_bin_remove(GST_BIN(pipeline), writer->rec_bin);
        writer->rec_bin = NULL;
    }
    g_free(writer->file_path);
    writer->file_path = NULL;
    writer->on_done = NULL;
}
