#ifndef GSTREAMER_VIDEO_WRITER_H
#define GSTREAMER_VIDEO_WRITER_H

#include <gst/gst.h>

// A reusable "encoded pad -> MP4 file" sink branch, shared by the trip and event recorders.
typedef struct {
    GstElement *rec_bin;          // queue ! h264parse ! mp4mux ! filesink
    guint       watchdog_id;
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
// - Call on_done(path). 
// A 3s watchdog forces teardown if EOS never reaches the filesink.
// The caller must have already stopped feeding the writer (trip:
// tearing down; event: backlog re-blocked)
void writer_close(RctGstVideoWriter *writer);

// Force-drop the branch synchronously (no EOS, no on_done)
void writer_reset(RctGstVideoWriter *writer);

#endif // GSTREAMER_VIDEO_WRITER_H
