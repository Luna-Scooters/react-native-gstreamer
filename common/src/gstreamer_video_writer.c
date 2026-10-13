//
//  gstreamer_video_writer.c
//
//  See gstreamer_video_writer.h. Owns the muxing tail
//  (queue ! h264parse ! mp4mux ! filesink) plus the finalize dance shared by the
//  trip and event recorders:
//
//    src ! [ queue ! h264parse ! mp4mux fragment-duration=1000 ! filesink ]
//
//  On close: push EOS into the muxer (own thread) so it writes the trailer. The
//  bin forwards the filesink's EOS message (message-forward), which the bus watch
//  routes back here via writer_handle_message() to tear down and fire
//  on_done(path). A 3s watchdog forces teardown if EOS never arrives.
//

#include "gstreamer_video_writer.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

static const guint WRITER_FINALIZE_TIMEOUT_MS = 3000;
static const GstClockTime WRITER_STATE_TIMEOUT = 100 * GST_MSECOND;

// Tag every element of a record bin with its writer and that writer's generation,
// so a bus message can be traced back to its writer — even once the bin has been
// removed and the element no longer has a parent to walk up to.
static GQuark writer_quark(void)
{
    static GQuark quark = 0;
    if (!quark)
        quark = g_quark_from_static_string("rct-gst-video-writer");
    return quark;
}

static GQuark writer_generation_quark(void)
{
    static GQuark quark = 0;
    if (!quark)
        quark = g_quark_from_static_string("rct-gst-video-writer-generation");
    return quark;
}

static void writer_tag(GstObject *object, RctGstVideoWriter *writer)
{
    g_object_set_qdata(G_OBJECT(object), writer_quark(), writer);
    g_object_set_qdata(G_OBJECT(object), writer_generation_quark(),
                       GUINT_TO_POINTER(writer->generation));
}

static void writer_tag_bin(RctGstVideoWriter *writer)
{
    writer_tag(GST_OBJECT(writer->rec_bin), writer);

    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(writer->rec_bin));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
            case GST_ITERATOR_OK:
                writer_tag(GST_OBJECT(g_value_get_object(&item)), writer);
                g_value_reset(&item);
                break;
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(it);
                break;
            default:
                done = TRUE;
                break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
}

// Main loop: tear the branch down and fire on_done. Idempotent via the rec_bin guard.
static void writer_teardown(RctGstVideoWriter *writer)
{
    if (!writer->rec_bin)
        return;   // already finalized

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
}

// Watchdog: EOS never reached the filesink. Fragmented MP4 keeps everything up
// to the last fragment playable, so the file is still handed over.
static gboolean writer_watchdog(gpointer user_data)
{
    RctGstVideoWriter *writer = (RctGstVideoWriter *)user_data;
    writer->watchdog_id = 0;   // this source is finishing; don't remove it again
    g_printerr("writer: EOS never reached the filesink, forcing teardown\n");
    writer_teardown(writer);
    return G_SOURCE_REMOVE;
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
    writer->generation++;
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

    // Finalize on the filesink's EOS *message*, not an EOS probe on its pad: a
    // probe fires before filesink has handled the event, and tearing down then
    // flushes the sink mid-write, losing the file's tail ("Error while writing to
    // file"). The message is posted only once filesink has flushed. Bins swallow
    // child EOS messages, so ask this one to forward them.
    g_object_set(writer->rec_bin, "message-forward", TRUE, NULL);
    writer_tag_bin(writer);

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

    writer->watchdog_id = g_timeout_add(WRITER_FINALIZE_TIMEOUT_MS, writer_watchdog, writer);

    // Push EOS into the muxer -> the filesink's EOS message runs writer_teardown.
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

gboolean writer_handle_message(GstMessage *message)
{
    GstObject *src = GST_MESSAGE_SRC(message);
    if (!src)
        return FALSE;
    RctGstVideoWriter *writer = g_object_get_qdata(G_OBJECT(src), writer_quark());
    if (!writer)
        return FALSE;

    // A message from a bin this writer has already torn down (or replaced with
    // the next file's) still belongs to it, but must not touch the current one.
    guint generation = GPOINTER_TO_UINT(
        g_object_get_qdata(G_OBJECT(src), writer_generation_quark()));
    gboolean current = writer->rec_bin && generation == writer->generation;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(message);
            if (!gst_structure_has_name(s, "GstBinForwarded"))
                return FALSE;
            GstMessage *forwarded = NULL;
            gst_structure_get(s, "message", GST_TYPE_MESSAGE, &forwarded, NULL);
            if (forwarded && GST_MESSAGE_TYPE(forwarded) == GST_MESSAGE_EOS && current)
                writer_teardown(writer);
            if (forwarded)
                gst_message_unref(forwarded);
            return TRUE;
        }
        default:
            return FALSE;
    }
}
