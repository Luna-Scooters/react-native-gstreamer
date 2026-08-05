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
//  Consumers request_src_pad() to tap it (which builds the branch on the first
//  request) and attach their own sink branch, then release_src_pad() when done.
//  The branch is ref-counted by its outstanding src pads and torn down when the
//  last one is released.
//
//  PTS repair (NONE -> running time, for broken-MJPEG frames) happens here at
//  the encoder input; consumers still rebase their own output to start at 0.
//

#ifndef gstreamer_encoder_h
#define gstreamer_encoder_h

#include <gst/gst.h>

// Request a src pad to tap the encoded H.264 stream. Builds the encoder on the
// first request. The pad is a ghost exposed at the encoder BIN boundary; 
// the caller owns the returned pad. NULL if the pipeline isn't ready.
GstPad *rct_gst_encoder_request_src_pad(void);

// Release a src pad from request_src_pad. Also drops the encoder's refcount; the
// branch is torn down when the last pad is released
void rct_gst_encoder_release_src_pad(GstPad *pad);

// Ask the encoder for a keyframe
void rct_gst_encoder_force_keyframe(void);

// Force teardown regardless of refcount (pipeline re-init / terminate).
void rct_gst_encoder_reset(void);

#endif /* gstreamer_encoder_h */
