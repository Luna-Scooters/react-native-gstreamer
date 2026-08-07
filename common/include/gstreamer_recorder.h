//
//  gstreamer_recorder.h
//
//  Records the ride to an MP4 file: taps the shared H.264 encoder
//  (gstreamer_encoder.c) and hands the muxing tail to a RctGstVideoWriter
//
//  Depends on gstreamer_backend.c for the pipeline global and the
//  onRecordingFinished callback.
//

#ifndef gstreamer_recorder_h
#define gstreamer_recorder_h

#include <gst/gst.h>

// Start recording to file_path (hardware H.264 encoder when available)
void rct_gst_start_recording(const gchar *file_path);

// Finalize asynchronously: drains the branch, writes the MP4 trailer, then
// fires onRecordingFinished. Bounded by a 3s watchdog.
void rct_gst_stop_recording(void);

gboolean rct_gst_is_recording(void);

// Postpone a pipeline state change until the in-progress recording finalizes
// (applied via rct_gst_set_pipeline_state from the finalize handler).
void rct_gst_recorder_defer_state(GstState state);

// Drop all recording state, detaching the branch if attached. Must be called
// before the owning pipeline is destroyed (re-init / terminate).
void rct_gst_recorder_reset(void);

#endif /* gstreamer_recorder_h */
