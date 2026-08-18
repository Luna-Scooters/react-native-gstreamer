//
//  gstreamer_event_recorder.h
//
//  "Instant replay" recorder built on a HELD BACKLOG QUEUE — the canonical
//  GStreamer backlog-recording pattern (see Centricular's
//  test-backlog-recording-h264.c).
//
//  While armed, a leaky queue tapped off the shared encoder's enc-tee keeps the
//  last PRE seconds of encoded H.264, held back by a BLOCKING pad probe so it
//  accumulates but records nothing. On an event, it is unblocked: the backlog
//  flushes into a RctGstVideoWriter.
//

#ifndef gstreamer_event_recorder_h
#define gstreamer_event_recorder_h

#include <gst/gst.h>

// Arm/disarm buffering. While armed, a leaky backlog queue off the shared
// encoder's enc-tee holds the last video_pre_length seconds of H.264 (held shut
// by a block probe). video_post_length is the tail recorded after a trigger.
// Both in seconds.
void rct_gst_event_recorder_set_buffering(gboolean enable, gint video_pre_length, gint video_post_length);

// Trigger: save a clip [event - PRE, event + POST] to file_path. Returns
// immediately; the backlog flushes now and POST seconds of live footage follow,
// then onEventSaved(path) fires. No-op if not buffering or a clip is recording.
void rct_gst_event_recorder_save(const gchar *file_path);

gboolean rct_gst_event_recorder_is_buffering(void);

// Detach the branch and drop all state. Call before the pipeline is destroyed.
void rct_gst_event_recorder_reset(void);

#endif /* gstreamer_event_recorder_h */
