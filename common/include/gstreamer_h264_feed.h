//
//  gstreamer_h264_feed.h
//
//  The H.264 stream every recorder (ride, event) records from.
//
//      camera:  rtph264depay ! h264parse ! tee name=camera-h264-tee ! <decoder>
//      encoder: video-tee ! ... ! <h264 enc> ! h264parse ! tee name=enc-tee
//
//  Consumers request_src_pad() to tap it and attach their own sink branch, then
//  release_src_pad() when done. The feed is ref-counted by its outstanding src
//  pads, and the encoder is torn down when the last one is released.
//
//  The camera only sends a keyframe every few seconds, so the feed keeps the
//  camera's current GOP and replays it to each new pad: a recording that joins
//  mid-GOP opens on the last keyframe.
//

#ifndef gstreamer_h264_feed_h
#define gstreamer_h264_feed_h

#include <gst/gst.h>

// Name of the tee the H.264 decode chain keeps ahead of its decoder.
#define RCT_GST_CAMERA_H264_TEE "camera-h264-tee"

// Request a src pad to tap the H.264 stream. Starts the encoder on the first
// request when the camera doesn't send H.264. The pad is a ghost exposed at the
// tee's BIN boundary; the caller owns the returned pad. NULL if the pipeline
// isn't ready.
GstPad *rct_gst_h264_request_src_pad(void);

// Release a src pad from request_src_pad. Also drops the feed's refcount; the
// encoder is torn down when the last pad is released.
void rct_gst_h264_release_src_pad(GstPad *pad);

// The decode chain is built: for an H.264 camera, start keeping its current GOP,
// so a recording that joins mid-GOP opens on the last keyframe. Streaming thread.
void rct_gst_h264_on_decode_chain(void);

// Ask for a keyframe. Only the encoder can make one; the camera's own stream
// comes as it is.
void rct_gst_h264_force_keyframe(void);

// Force teardown regardless of refcount (pipeline re-init / terminate).
void rct_gst_h264_reset(void);

#endif /* gstreamer_h264_feed_h */
