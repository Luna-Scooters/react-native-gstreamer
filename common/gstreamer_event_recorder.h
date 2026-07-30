//
//  gstreamer_event_recorder.h
//
//  "Instant replay" recorder: continuously buffers the last PRE seconds of
//  hardware-encoded H.264 off the pipeline's video-tee, so that when an event
//  fires it can save a clip covering [event - PRE, event + POST] — the seconds
//  BEFORE the trigger plus the seconds after. The buffer holds encoded access
//  units (whole GOPs) in memory, so no re-encode happens on save.
//

#ifndef gstreamer_event_recorder_h
#define gstreamer_event_recorder_h

#include <gst/gst.h>

// Arm/disarm continuous buffering. While armed, an encode+appsink branch runs
// off video-tee, keeping the rolling GOP ring. Safe to call before the tee is
// ready — it attaches once the pipeline is playing.
void rct_gst_event_recorder_set_buffering(gboolean enable);

// Trigger: save a clip [event - PRE, event + POST] to file_path. Returns
// immediately; the file is written once POST seconds of footage have buffered,
// then onEventSaved(path) fires. No-op if not buffering or a save is pending.
void rct_gst_event_recorder_save(const gchar *file_path);

gboolean rct_gst_event_recorder_is_buffering(void);

// Detach the branch and drop all state. Call before the pipeline is destroyed.
void rct_gst_event_recorder_reset(void);

#endif /* gstreamer_event_recorder_h */
