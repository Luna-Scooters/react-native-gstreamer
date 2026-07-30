//
//  gstreamer_encoder.h
//
//  A single, ref-counted, continuous H.264 encode branch off the pipeline's
//  video-tee, shared by every consumer (ride recorder, event recorder) so that
//  only ONE hardware encode session is ever used — required because ride and
//  event recording run at the same time and mobile encoders often allow one
//  session.
//
//      video-tee ! queue ! videorate ! videoconvert ! <h264 enc>
//                ! h264parse ! tee name=enc-tee
//
//  Consumers acquire() (which builds the branch on the first call), request a
//  src pad from the returned enc-tee to attach their own sink branch, then
//  release() when done (the branch is torn down on the last release).
//
//  PTS repair (NONE -> running time, for broken-MJPEG frames) happens here at
//  the encoder input; consumers still rebase their own output to start at 0.
//

#ifndef gstreamer_encoder_h
#define gstreamer_encoder_h

#include <gst/gst.h>

// Build (if needed) and ref the shared encode branch; returns the "enc-tee"
// element (borrowed — do not unref) to request src pads from, or NULL if the
// pipeline isn't ready
GstElement *rct_gst_encoder_acquire(void);

// Drop one ref; tears the branch down when the last consumer releases.
void rct_gst_encoder_release(void);

// Force teardown regardless of refcount (pipeline re-init / terminate).
void rct_gst_encoder_reset(void);

#endif /* gstreamer_encoder_h */
