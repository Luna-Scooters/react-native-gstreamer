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

// The display branch - display-queue ! ... ! vulkansink - goes down on its own,
// ahead of the rest of the pipeline, in two cases:
//
// - The surface goes away while the pipeline runs on - the RTSP session, a
//   recording. Nothing may draw into it once surfaceDestroyed() returns, or
//   vulkansink fails with "Surface lost". The display comes back up on the next
//   surface: vulkansink opens a new window going from READY to PAUSED.
// - The pipeline goes below PAUSED. Going down with frames still on their way
//   through the branch crashed the Mali driver's event thread about once in 20.
//
// Down means: a probe on display-queue's src pad holds back what comes next,
// the frame already past it is waited for, and the sink goes to READY, locked
// out of the pipeline's state changes. The leaky queue drops what piles up in
// the meantime, so the tee goes on feeding the recording.

// For a sink holding a frame for preroll in PAUSED, which it never lets go of.
#define DISPLAY_IDLE_TIMEOUT (500 * G_TIME_SPAN_MILLISECOND)

// The UI thread and the GMainLoop thread (a deferred state) both change state.
static GMutex display_lock;
// display-queue's src pad and the probe holding it back, while the display is down.
static GstPad *display_gate;
static gulong display_gate_probe;

static GstPadProbeReturn cb_display_idle(GstPad *pad, GstPadProbeInfo *info, gpointer idle)
{
    (void)pad; (void)info;
    g_async_queue_push(idle, GINT_TO_POINTER(TRUE));
    return GST_PAD_PROBE_REMOVE;
}

// Call with display_lock.
static void display_down(GstElement *bin, GstElement *sink)
{
    if (display_gate)
        return;
    g_autoptr(GstElement) queue = gst_bin_get_by_name(GST_BIN(bin), "display-queue");
    if (!queue)
        return;   // the debug pipeline
    display_gate = gst_element_get_static_pad(queue, "src");

    // Under the state lock, so that a state change the bin is in the middle of
    // is done with the sink first.
    GST_STATE_LOCK(bin);
    gst_element_set_locked_state(sink, TRUE);
    GST_STATE_UNLOCK(bin);

    display_gate_probe = gst_pad_add_probe(display_gate, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                           NULL, NULL, NULL);

    // An idle probe runs once the push in flight returns, or right away.
    GAsyncQueue *idle = g_async_queue_new();
    gulong idle_probe = gst_pad_add_probe(display_gate, GST_PAD_PROBE_TYPE_IDLE, cb_display_idle,
                                          g_async_queue_ref(idle), (GDestroyNotify)g_async_queue_unref);
    if (idle_probe && !g_async_queue_timeout_pop(idle, DISPLAY_IDLE_TIMEOUT)) {
        g_printerr("Display still busy after 500 ms; taking the video sink down anyway\n");
        gst_pad_remove_probe(display_gate, idle_probe);
    }
    g_async_queue_unref(idle);

    gst_element_set_state(sink, GST_STATE_READY);
}

// Call with display_lock.
static void display_up(GstElement *bin, GstElement *sink)
{
    if (!display_gate)
        return;

    // Not as an async state change: in a PLAYING pipeline, a sink that has to
    // preroll makes it lose its state and come back with a new base time - under
    // a recording in progress.
    gboolean async = TRUE;
    g_object_get(sink, "async", &async, NULL);
    g_object_set(sink, "async", FALSE, NULL);
    GST_STATE_LOCK(bin);
    gst_element_set_locked_state(sink, FALSE);
    if (!gst_element_sync_state_with_parent(sink))
        g_printerr("Could not bring the video sink back up\n");
    GST_STATE_UNLOCK(bin);
    g_object_set(sink, "async", async, NULL);

    // Going down, the sink forgot the caps it was given: have them sent again.
    g_autoptr(GstPad) sinkpad = gst_element_get_static_pad(sink, "sink");
    gst_pad_push_event(sinkpad, gst_event_new_reconfigure());

    gst_pad_remove_probe(display_gate, display_gate_probe);
    display_gate_probe = 0;
    gst_clear_object(&display_gate);
}

void rct_gst_set_drawable_surface(guintptr _drawableSurface)
{
    // Drawable surface set to null: the display branch goes down and stays down until the next surface.
    if (!_drawableSurface) {
        drawable_surface = 0;
        g_autoptr(GstElement) sink = pipeline ? gst_bin_get_by_name(GST_BIN(pipeline), RCT_GST_VIDEO_SINK) : NULL;
        if (sink) {
            g_mutex_lock(&display_lock);
            display_down(pipeline, sink);
            g_mutex_unlock(&display_lock);
        }
        return;
    }

    // vulkansink only takes a new handle as it opens its window, so a running
    // one carries on drawing into the old surface. Say so, otherwise the
    // symptom is a view that silently stops updating.
    if (pipeline && drawable_surface && _drawableSurface != drawable_surface) {
        g_printerr("Drawable surface changed under a running video sink; "
                   "set 0 first to have it follow\n");
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

            // Hand the sink the handle directly: asking for it with
            // prepare-window-handle goes through the bus, which drops messages
            // while the pipeline is in NULL - after an error, say - and the sink
            // would open its next window on the previous surface's handle.
            if (GST_IS_VIDEO_OVERLAY(video_sink)) {
                video_overlay = GST_VIDEO_OVERLAY(video_sink);
                gst_video_overlay_set_window_handle(video_overlay, drawable_surface);
            }

            g_mutex_lock(&display_lock);
            display_up(pipeline, video_sink);
            g_mutex_unlock(&display_lock);
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

GstStateChangeReturn rct_gst_display_set_pipeline_state(GstElement *target, GstState state)
{
    g_autoptr(GstElement) sink = gst_bin_get_by_name(GST_BIN(target), RCT_GST_VIDEO_SINK);
    if (state > GST_STATE_READY || !sink)
        return gst_element_set_state(target, state);

    g_mutex_lock(&display_lock);
    // Already down if there is no surface, and then it stays down.
    gboolean has_surface = display_gate == NULL;
    display_down(target, sink);
    GstStateChangeReturn ret = gst_element_set_state(target, state);
    if (has_surface)
        display_up(target, sink);
    g_mutex_unlock(&display_lock);
    return ret;
}

void rct_gst_display_reset(GstElement *target)
{
    // A display down for want of a surface stayed down: to NULL with the rest.
    g_autoptr(GstElement) display_sink = gst_bin_get_by_name(GST_BIN(target), RCT_GST_VIDEO_SINK);
    if (display_sink) {
        g_mutex_lock(&display_lock);
        display_up(target, display_sink);
        g_mutex_unlock(&display_lock);
    }

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

void apply_mirror_image(GstElement *target)
{
    g_autoptr(GstElement) videoflip = gst_bin_get_by_name(GST_BIN(target), "display-flip");
    if (!videoflip) {
        g_printerr("Mirror image not applied :  videoflip element not found\n");
        return;
    }

    GstVideoOrientationMethod direction = rct_gst_get_configuration()->mirrorImage
        ? GST_VIDEO_ORIENTATION_HORIZ : GST_VIDEO_ORIENTATION_IDENTITY;
    g_object_set(videoflip, "video-direction", direction, NULL);
}
