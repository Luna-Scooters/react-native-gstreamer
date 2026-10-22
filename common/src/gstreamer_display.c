//
//  gstreamer_display.c
//
//  See gstreamer_display.h.
//

#include "gstreamer_display.h"
#include "gstreamer_backend.h"
#include "gstreamer_codec.h"
#include <gst/video/video.h>

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

// Serializes video_sink between the main thread (which swaps it on surface
// changes and drops it on re-init / terminate) and the image-capture thread
// (rct_gst_pull_last_sample). All backend state lives in file globals and is
// shared by every player instance, so a new instance's rct_gst_init can race
// an old instance's still-running capture loop.
static GMutex video_sink_lock;

static GstElement *video_sink;
static GstVideoOverlay *video_overlay;
static guintptr drawable_surface;

void rct_gst_set_drawable_surface(guintptr _drawableSurface)
{
    // Unlike the GL window, vulkansink's Android window refuses to be repointed
    // at a different native window once it holds one ("View changes are not
    // implemented"), so a surface that is destroyed and recreated - the app
    // going to background, a rotation - only renders again once the pipeline
    // itself is re-created. Log it, otherwise the symptom is a silent black view.
    if (pipeline && drawable_surface && _drawableSurface != drawable_surface) {
        g_printerr("Drawable surface changed under the vulkan sink; "
                   "re-create the pipeline to render into the new one\n");
    }

    drawable_surface = _drawableSurface;

    if(pipeline)
    {
        // gst_bin_get_by_name returns a NEW ref each call (this runs on every
        // surface change), so drop the one we already hold before replacing it.
        g_mutex_lock(&video_sink_lock);
        GstElement *previous_sink = video_sink;
        video_sink = gst_bin_get_by_name(GST_BIN(pipeline), RCT_GST_VIDEO_SINK);
        g_mutex_unlock(&video_sink_lock);
        if (previous_sink)
            gst_object_unref(previous_sink);

        // Configure the video sink if we have one
        if (video_sink) {
            // Configure for lower latency if it's not in debug mode
            if (!rct_gst_get_configuration()->isDebugging) {
                g_object_set(G_OBJECT(video_sink),
                            "qos", TRUE,
                            "max-lateness", 20 * GST_MSECOND,
                            NULL);
            }

            // Set up video overlay if supported
            if (GST_IS_VIDEO_OVERLAY(video_sink)) {
                video_overlay = GST_VIDEO_OVERLAY(video_sink);
                gst_video_overlay_prepare_window_handle(video_overlay);
            }
        }
    }
}

GstSample *rct_gst_pull_last_sample(void)
{
    g_mutex_lock(&video_sink_lock);
    GstSample *sample = NULL;
    if (video_sink)
        g_object_get(video_sink, "last-sample", &sample, NULL);
    g_mutex_unlock(&video_sink_lock);
    return sample;
}

GstBusSyncReply rct_gst_display_bus_sync(GstBus *bus, GstMessage *message, gpointer user_data)
{
    (void)bus; (void)user_data;
    if(!gst_is_video_overlay_prepare_window_handle_message(message))
        return GST_BUS_PASS;

    if (video_overlay && drawable_surface) {
        gst_video_overlay_set_window_handle(video_overlay, drawable_surface);
    }

    gst_message_unref(message);
    return GST_BUS_DROP;
}

void rct_gst_display_reset(void)
{
    // video_overlay is a cast of video_sink, NOT a second ref - clearing the
    // pointer is the whole job. Unreffing it too would over-unref the sink.
    g_mutex_lock(&video_sink_lock);
    GstElement *sink = video_sink;
    video_sink = NULL;
    g_mutex_unlock(&video_sink_lock);
    if (sink)
        gst_object_unref(sink);

    video_overlay = NULL;
}
