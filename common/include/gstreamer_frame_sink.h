//
//  gstreamer_frame_sink.h
//
//  Camera frames for on-device inference: an appsink on video-tee that keeps
//  the newest decoded frame and hands it out as packed RGBA at the model's
//  input size.
//
//      video-tee ! appsink (newest frame)
//
//  The branch only holds a reference to the decoder's frame. Converting and
//  scaling it happen in rct_gst_frame_sink_next(), on the caller's thread, so
//  a frame nobody asks for costs nothing. The frame is stretched to the size
//  asked for, not letterboxed: the model sees the whole camera image, and its
//  normalized coordinates map straight back onto it.
//
//  It maps one to one onto luna::Mobile::IFrameSource: Configure() is
//  rct_gst_frame_sink_configure() and NextFrame() is rct_gst_frame_sink_next().
//  Only plain C types, so an inference module can include this header without
//  GStreamer's.
//
//  The size asked for outlives the player: the branch is built again on every
//  pipeline the player makes (each camera screen visit) until
//  rct_gst_frame_sink_stop(). It may be asked for before the player exists.
//
//  On iOS the functions link statically into the app. On Android they are
//  exported from librctgstplayer.so; another native library reaches them with
//  dlopen("librctgstplayer.so", RTLD_NOW) and dlsym().
//

#ifndef gstreamer_frame_sink_h
#define gstreamer_frame_sink_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define RCT_GST_FRAME_SINK_API __attribute__((visibility("default")))
#else
#define RCT_GST_FRAME_SINK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Ask for frames of `width` x `height`, and start the branch if the player is
// running; otherwise it starts with the player. The size is fixed until
// rct_gst_frame_sink_stop(): asking again for the same size only rebuilds a
// missing branch. 0 on success; -1 for a size of 0 or over 4096, for another
// size before stop, or if the branch can't be built. Any thread.
RCT_GST_FRAME_SINK_API int rct_gst_frame_sink_configure(uint32_t width, uint32_t height);

// Copy the newest frame that hasn't been handed out yet into `rgba`, as
// width * height * 4 bytes of packed RGBA (opaque alpha), rows top to bottom.
// 0 on success; -1, leaving `rgba` untouched, when there is no new frame -
// which is not an error - or when `size` is too small. Never blocks on the
// stream. Any thread.
RCT_GST_FRAME_SINK_API int rct_gst_frame_sink_next(uint8_t *rgba, size_t size);

// Stop the branch and forget the size. Any thread.
RCT_GST_FRAME_SINK_API void rct_gst_frame_sink_stop(void);

// Backend: the pipeline was built (video-tee exists). Builds the branch on it
// if frames are wanted. Thread that builds the pipeline.
void rct_gst_frame_sink_attach(void);

// Backend: the pipeline is going away. Takes the branch down and lets go of
// the pipeline; the size is kept for the next one. Before the pipeline goes
// to NULL.
void rct_gst_frame_sink_detach(void);

#ifdef __cplusplus
}
#endif

#endif /* gstreamer_frame_sink_h */
