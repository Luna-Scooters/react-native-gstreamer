//
//  gstreamer_encoder.h
//
//  A single, continuous H.264 encode branch off the pipeline's video-tee, for a
//  camera that doesn't send H.264 itself. It is shared by every consumer (ride
//  recorder, event recorder) through the H.264 feed (gstreamer_h264_feed.h), so
//  that only ONE hardware encode session is ever used — required because ride
//  and event recording run at the same time and mobile encoders often allow one
//  session.
//
//      video-tee ! queue ! videorate ! videoconvert ! <h264 enc>
//                ! h264parse ! tee name=enc-tee
//
//  The feed starts the branch for its first consumer and resets it when the
//  last one is done.
//
//  PTS repair (NONE -> running time, for broken-MJPEG frames) happens here at
//  the encoder input; consumers still rebase their own output to start at 0.
//

#ifndef gstreamer_encoder_h
#define gstreamer_encoder_h

#include <gst/gst.h>

// Build the encode branch if it isn't running and return its enc-tee, borrowed
// (the branch owns it). NULL if the pipeline isn't ready.
GstElement *rct_gst_encoder_start(void);

// Ask the encoder for a keyframe
void rct_gst_encoder_force_keyframe(void);

// Tear the branch down.
void rct_gst_encoder_reset(void);

#endif /* gstreamer_encoder_h */
