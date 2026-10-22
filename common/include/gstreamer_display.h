//
//  gstreamer_display.h
//
//  What the player shows: the decoded frames on video-tee, uploaded to Vulkan
//  and rendered by vulkansink into the platform's drawable surface.
//
//      video-tee ! queue ! videoconvert ! vulkanupload ! vulkancolorconvert ! vulkansink
//
//  An H.264 camera's decoder is the display's too: the recorders take the
//  camera's own H.264 from ahead of it and the encoder never starts, so the
//  display is the only one reading its frames, and they may be Vulkan images.
//  A JPEG camera's decoder also feeds the encoder, which needs them in system
//  memory, so that one stays the decode chain's.
//

#ifndef gstreamer_display_h
#define gstreamer_display_h

#include <gst/gst.h>

// Name of the sink the display renders with, in the app and debug pipelines alike.
#define RCT_GST_VIDEO_SINK "video-sink"

// The display branch, appended to video-tee in the launch command.
//
// The queue is bounded and leaky so a stalled sink drops stale frames instead
// of filling, blocking the tee (and with it the decoder and the encoder branch),
// then replaying the backlog at sync=false speed.
#define RCT_GST_DISPLAY_BRANCH                                                  \
    "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream " \
    "! videoconvert ! video/x-raw,format=NV12 "                                 \
    "! vulkanupload ! vulkancolorconvert "                                      \
    "! vulkansink sync=false name=" RCT_GST_VIDEO_SINK " enable-last-sample=true"

// Render into `drawable_surface`. Also picks up the pipeline's video sink, so
// it is called again once a new pipeline is built. Main thread.
void rct_gst_set_drawable_surface(guintptr drawable_surface);

// The frame the sink rendered last, or NULL. Any thread; the caller owns it.
GstSample *rct_gst_pull_last_sample(void);

// The pipeline bus's sync handler: hands the sink the surface when it asks for
// a window. Streaming thread.
GstBusSyncReply rct_gst_display_bus_sync(GstBus *bus, GstMessage *message, gpointer user_data);

// Drop the video sink, once the pipeline is gone. The surface is kept for the
// next pipeline.
void rct_gst_display_reset(void);

#endif /* gstreamer_display_h */
