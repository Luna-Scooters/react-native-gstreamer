#ifndef GSTREAMER_VIDEO_WRITER_H
#define GSTREAMER_VIDEO_WRITER_H

#include <gst/gst.h>

// A reusable "encoded pad -> MP4 file" sink branch, shared by the trip and event recorders.
typedef struct {
    GstElement *rec_bin;          // queue ! h264parse ! mp4mux ! filesink
    guint       watchdog_id;
    guint       generation;       // bumped per file, to tell a stale bin's messages apart
    gchar      *file_path;
    void      (*on_done)(gchar *path);  // onRecordingFinished / onEventSaved
} RctGstVideoWriter;

// Build the muxing tail for `path`, bring it up in the pipeline and link `src`
// into it. Caller owns `src` and `done`. 
// Returns FALSE on failure.
gboolean writer_start(RctGstVideoWriter *writer, GstPad *src, const gchar *path, void (*done)(gchar *));

// Finalize video writing with EOS:
// - Push EOS so mp4mux writes the trailer
// - Tear the branch down and
// - Call on_done(path) once the filesink has written it all.
// A 3s watchdog forces teardown if EOS never reaches the filesink.
// The caller must have already stopped feeding the writer (trip:
// tearing down; event: backlog re-blocked)
void writer_close(RctGstVideoWriter *writer);

// Force-drop the branch synchronously (no EOS, no on_done)
void writer_reset(RctGstVideoWriter *writer);

// Bus watch (main loop): handle a message posted from inside a writer's branch.
// The forwarded filesink EOS finalizes the file. Returns TRUE if the message was
// the writer's own, in which case the bus watch has nothing more to do with it.
gboolean writer_handle_message(GstMessage *message);

#endif // GSTREAMER_VIDEO_WRITER_H
