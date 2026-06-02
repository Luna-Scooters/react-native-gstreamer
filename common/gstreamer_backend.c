//
//  gstreamer_backend.c
//
//  Created by Alann on 13/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#include "gstreamer_backend.h"

// Log info
GST_DEBUG_CATEGORY_STATIC(rct_gst_player);

// Globals items
RctGstAudioLevel* audio_level;
RctGstConfiguration* configuration;

GstElement *pipeline;
GMainLoop *main_loop;
guint bus_watch_id;
GstBus *bus;

// Video
guintptr drawable_surface;
GstVideoOverlay* video_overlay;

// Audio
GstElement* audio_level_element;

// Sinks
GstElement *video_sink;
GstElement *audio_sink;

GstElement *framerate_filter;

// Getters
RctGstConfiguration *rct_gst_get_configuration()
{
    if (!configuration) {
        configuration = g_malloc(sizeof(RctGstConfiguration));
        configuration->audioLevelRefreshRate = g_malloc(sizeof(gint));
        *(configuration->audioLevelRefreshRate) = 100;
        configuration->uri = NULL;
        configuration->isDebugging = FALSE;
        
        configuration->onElementError = NULL;
        configuration->onStateChanged = NULL;
        configuration->onVolumeChanged = NULL;
        configuration->onUriChanged = NULL;
        
        configuration->onInit = NULL;
        configuration->onEOS = NULL;
        configuration->initialDrawableSurface = 0;
    }
    return configuration;
}

RctGstAudioLevel *rct_gst_get_audio_level()
{
    if (!audio_level) {
        audio_level = g_malloc0(sizeof(RctGstAudioLevel));
    }
    return audio_level;
}

// Setters
void rct_gst_set_uri(gchar* _uri)
{
    rct_gst_get_configuration()->uri = _uri;
    if (pipeline)
        apply_uri();
}

void rct_gst_set_audio_level_refresh_rate(gint audio_level_refresh_rate)
{
    if (rct_gst_get_configuration()->audioLevelRefreshRate == NULL) {
        rct_gst_get_configuration()->audioLevelRefreshRate = g_malloc(sizeof(gint));
    }
    *(rct_gst_get_configuration()->audioLevelRefreshRate) = audio_level_refresh_rate;

    if (audio_level_element) {
        g_object_set(audio_level_element, "interval", audio_level_refresh_rate * 1000000, NULL);
    }
}

void rct_gst_set_debugging(gboolean is_debugging)
{
    rct_gst_get_configuration()->isDebugging = is_debugging;
    // TODO : Recreate pipeline...
}

/**********************
 VIDEO HANDLING METHODS
 *********************/
void rct_gst_set_drawable_surface(guintptr _drawableSurface)
{
    drawable_surface = _drawableSurface;
    
    if(pipeline)
    {
        // Always try to get the video-sink by name first (works for both debug and rtspsrc pipelines)
        video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "video-sink");
        
        if (!video_sink) {
            // For playbin3 with a video-sink bin, find glimagesink by interface
            video_sink = gst_bin_get_by_interface(GST_BIN(pipeline), GST_TYPE_VIDEO_OVERLAY);
            if (!video_sink)
                g_print(rct_gst_player, "Could not find GstVideoOverlay element in pipeline");
            else
                g_print(rct_gst_player, "Found video overlay element: %s", GST_ELEMENT_NAME(video_sink));
        } else {
            g_print(rct_gst_player, "Found named video-sink: %s", GST_ELEMENT_NAME(video_sink));
        }
        
        // Configure the video sink if we have one
        if (video_sink) {
            // Set up video overlay if supported
            if (GST_IS_VIDEO_OVERLAY(video_sink)) {
                video_overlay = GST_VIDEO_OVERLAY(video_sink);
                gst_video_overlay_prepare_window_handle(video_overlay);
            }
        }
    }
}

/**********************
 AUDIO HANDLING METHODS
 *********************/
GstElement* create_audio_sink()
{
    // Prepare audio level structure
    rct_gst_get_audio_level();
    
    // New audio bin
    GstElement *leveledsink = gst_bin_new("leveledsink");
    
    // Create an audio level analyzing filter with 100ms refresh rate
    audio_level_element = gst_element_factory_make("level", NULL);
    
    // Creating audio sink
    GstElement *audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");
    gst_bin_add_many(GST_BIN(leveledsink), audio_level_element, audio_sink, NULL);
    
    // Linking them
    if(!gst_element_link(audio_level_element, audio_sink))
        g_printerr("Failed to link audio_level and audio_sink");
    
    // Creating pad and ghost pad
    GstPad *levelPad = gst_element_get_static_pad(audio_level_element, "sink");
    gst_element_add_pad(leveledsink, gst_ghost_pad_new("sink", levelPad));
    gst_object_unref(GST_OBJECT(levelPad));
    
    return leveledsink;
}

GstBusSyncReply cb_create_window(GstBus *bus, GstMessage *message, gpointer user_data)
{
    if(!gst_is_video_overlay_prepare_window_handle_message(message))
        return GST_BUS_PASS;
    
    if (video_overlay && drawable_surface) {
        gst_video_overlay_set_window_handle(video_overlay, drawable_surface);
    }
    
    gst_message_unref(message);
    return GST_BUS_DROP;
}

/*********************
 APPLICATION CALLBACKS
 ********************/
static void cb_error(GstBus *bus, GstMessage *msg, gpointer *user_data)
{
    GError *err;
    gchar *debug_info;
    
    gst_message_parse_error(msg, &err, &debug_info);
    g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
    if (rct_gst_get_configuration()->onElementError) {
        rct_gst_get_configuration()->onElementError(GST_OBJECT_NAME(msg->src), err->message, debug_info);
    }
    g_clear_error(&err);
    g_free(debug_info);
    rct_gst_set_pipeline_state(GST_STATE_NULL);
}

static void cb_eos(GstBus *bus, GstMessage *msg, gpointer *user_data)
{
    if (rct_gst_get_configuration()->onEOS) {
        rct_gst_get_configuration()->onEOS();
    }
}

static void cb_state_changed(GstBus *bus, GstMessage *msg, gpointer *user_data)
{
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

    // Only pay attention to messages coming from the pipeline, not its children
    if(GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline))
    {
        if (rct_gst_get_configuration()->onStateChanged) {
            rct_gst_get_configuration()->onStateChanged(old_state, new_state);
        }
    }
    
}

static gboolean cb_message_element(GstBus *bus, GstMessage *msg, gpointer *user_data)
{
    if(msg->type == GST_MESSAGE_ELEMENT)
    {
        const GstStructure *s = gst_message_get_structure(msg);
        const gchar *name = gst_structure_get_name(s);
        
        GValueArray *rms_arr, *peak_arr, *decay_arr;
        gdouble rms_dB, peak_dB, decay_dB;
        const GValue *value;
        
        if(g_strcmp0(name, "level") == 0)
        {
            /* the values are packed into GValueArrays with the value per channel */
            const GValue *array_val = gst_structure_get_value(s, "peak");
            
            array_val = gst_structure_get_value(s, "rms");
            rms_arr = (GValueArray *)g_value_get_boxed(array_val);
            
            array_val = gst_structure_get_value(s, "peak");
            peak_arr = (GValueArray *)g_value_get_boxed(array_val);
            
            array_val = gst_structure_get_value(s, "decay");
            decay_arr = (GValueArray *)g_value_get_boxed(array_val);
            
            // No multichannel needs to be handled - Otherwise : gint channels = rms_arr->n_values;
            
            // RMS
            value = g_value_array_get_nth(rms_arr, 0);
            rms_dB = g_value_get_double(value);
            rct_gst_get_audio_level()->rms = pow(10, rms_dB / 20); // converting from dB to normal gives us a value between 0.0 and 1.0
            
            // PEAK
            value = g_value_array_get_nth(peak_arr, 0);
            peak_dB = g_value_get_double(value);
            rct_gst_get_audio_level()->peak = pow(10, peak_dB / 20); // converting from dB to normal gives us a value between 0.0 and 1.0
            
            // DECAY
            value = g_value_array_get_nth(decay_arr, 0);
            decay_dB = g_value_get_double(value);
            rct_gst_get_audio_level()->decay = pow(10, decay_dB / 20); // converting from dB to normal gives us a value between 0.0 and 1.0
            
            if (rct_gst_get_configuration()->onVolumeChanged){
                rct_gst_get_configuration()->onVolumeChanged(rct_gst_get_audio_level());
            }
        }
    }
    return TRUE;
}

static gboolean cb_async_done(GstBus *bus, GstMessage *message, gpointer user_data)
{
    return TRUE;
}

static gboolean restart_stream(gpointer data) {
     g_print("Restarting RTSP stream...\n");

     // Stop the stream
     gst_element_set_state(pipeline, GST_STATE_NULL);

     // Restart it
     GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
     if (ret == GST_STATE_CHANGE_FAILURE) {
         g_printerr("Failed to restart pipeline\n");
     } else {
         g_print("Stream restarted successfully.\n");
     }

     return TRUE;
}

static gboolean cb_bus_watch(GstBus *bus, GstMessage *message, gpointer user_data)
{
    switch (GST_MESSAGE_TYPE(message))
    {
        case GST_MESSAGE_ERROR:
            cb_error(bus, message, user_data);
            break;
            
        case GST_MESSAGE_EOS:
            cb_eos(bus, message, user_data);
            break;
            
        case GST_MESSAGE_STATE_CHANGED:
            cb_state_changed(bus, message, user_data);
            break;
            
        case GST_MESSAGE_ELEMENT:
            cb_message_element(bus, message, user_data);
            break;
            
        case GST_MESSAGE_ASYNC_DONE:
            cb_async_done(bus, message, user_data);
            break;

        case GST_MESSAGE_TAG: {
            // Detect MJPEG streams and cap their framerate to 10/1
            GstTagList *tags = NULL;
            gst_message_parse_tag(message, &tags);
            if (tags) {
                gchar *codec = NULL;
                if (gst_tag_list_get_string(tags, GST_TAG_VIDEO_CODEC, &codec)) {
                    g_print(rct_gst_player, "Video codec tag: %s", codec);
                    if (framerate_filter &&
                        (g_ascii_strcasecmp(codec, "JPEG") || g_ascii_strcasecmp(codec, "MJPEG"))) {
                        g_print(rct_gst_player, "MJPEG detected — capping framerate to 10/1");
                        GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                                            "framerate", GST_TYPE_FRACTION, 10, 1,
                                                            NULL);
                        g_object_set(framerate_filter, "caps", caps, NULL);
                        gst_caps_unref(caps);
                    }
                    g_free(codec);
                }
                gst_tag_list_unref(tags);
            }
            break;
        }

        default:
            break;
    }
    
    return TRUE;
}

/*************
 OTHER METHODS
 ************/
GstStateChangeReturn rct_gst_set_pipeline_state(GstState state)
{
    g_print("Pipeline state requested : %s\n", gst_element_state_get_name(state));
    GstStateChangeReturn validity = gst_element_set_state(pipeline, state);
    g_print("Validity : %s\n", gst_element_state_change_return_get_name(validity));

    return validity;
}

void rct_gst_init(RctGstConfiguration *configuration)
{
    g_print(rct_gst_player, "Initializing pipeline (debugging=%s, uri=%s)",
                 rct_gst_get_configuration()->isDebugging ? "yes" : "no",
                 rct_gst_get_configuration()->uri ? rct_gst_get_configuration()->uri : "(none)");

    if (!rct_gst_get_configuration()->isDebugging) {
        g_print(rct_gst_player, "Creating playbin3 pipeline");
        pipeline = gst_element_factory_make("playbin3", "pipeline");
    } else {
        g_print(rct_gst_player, "Creating debug pipeline (videotestsrc)");
        GError *error = NULL;
        pipeline = gst_parse_launch("videotestsrc ! glimagesink name=video-sink", &error);
        if (error != NULL) {
            g_printerr(rct_gst_player, "Error creating debug pipeline: %s", error->message);
            g_error_free(error);
            return;
        }
    }

    if (!pipeline) {
        g_printerr(rct_gst_player, "Failed to create pipeline");
        return;
    }

    g_print(rct_gst_player, "Pipeline created successfully");

    // Build a video-sink bin: videorate ! capsfilter ! glimagesink
    // The capsfilter starts as a passthrough; GST_MESSAGE_TAG sets framerate=10/1
    // when an MJPEG stream is detected.
    GstElement *video_sink_bin = gst_bin_new("video-sink-bin");
    GstElement *videorate = gst_element_factory_make("videorate", NULL);
    g_object_set(videorate, "drop-out-of-segment", TRUE, "drop-only", TRUE, NULL);
    framerate_filter = gst_element_factory_make("capsfilter", "framerate-filter");
    // Start with a high ceiling (60/1) so videorate has a valid target and passes
    // most streams through unmodified. When MJPEG is detected via GST_MESSAGE_TAG
    // this is tightened to 10/1.
    GstCaps *passthrough = gst_caps_new_simple("video/x-raw",
                                               "framerate", GST_TYPE_FRACTION, 60, 1,
                                               NULL);
    g_object_set(framerate_filter, "caps", passthrough, NULL);
    gst_caps_unref(passthrough);
    GstElement *glsink = gst_element_factory_make("glimagesink", NULL);
    g_object_set(glsink, "sync", FALSE, "qos", TRUE, "max-lateness", (gint64)(20 * GST_MSECOND), NULL);
    gst_bin_add_many(GST_BIN(video_sink_bin), videorate, framerate_filter, glsink, NULL);
    gst_element_link_many(videorate, framerate_filter, glsink, NULL);
    GstPad *vs_sink_pad = gst_element_get_static_pad(videorate, "sink");
    gst_element_add_pad(video_sink_bin, gst_ghost_pad_new("sink", vs_sink_pad));
    gst_object_unref(vs_sink_pad);
    g_object_set(pipeline, "video-sink", video_sink_bin, NULL);
    gst_object_unref(video_sink_bin);
    g_print(rct_gst_player, "Video sink bin (videorate + capsfilter + glimagesink) installed");
    
    // Preparing bus
    bus = gst_element_get_bus(pipeline);
    bus_watch_id = gst_bus_add_watch(bus, cb_bus_watch, NULL);
    
    // First time, need a surface to draw on - then use rct_gst_set_drawable_surface
    rct_gst_set_drawable_surface(rct_gst_get_configuration()->initialDrawableSurface);
    gst_bus_set_sync_handler(bus,(GstBusSyncHandler)cb_create_window, pipeline, NULL);
    gst_object_unref(bus);

    // Only playbin-like pipelines expose audio-sink.
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(pipeline), "audio-sink") != NULL) {
        audio_sink = gst_element_factory_make("fakesink", "audio-sink");
        g_object_set(pipeline, "audio-sink", audio_sink, NULL);
    }

    // Apply URI
    if (!rct_gst_get_configuration()->isDebugging && pipeline != NULL)
        apply_uri();
    
    if (rct_gst_get_configuration()->onInit) {
        rct_gst_get_configuration()->onInit();
    }
}

void rct_gst_run_loop()
{
    main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);
}

void rct_gst_terminate()
{
    /* Free resources */
    framerate_filter = NULL;

    if(video_sink != NULL)
        gst_object_unref(video_sink);
    
    if(video_overlay != NULL)
        gst_object_unref(video_overlay);

    if(drawable_surface)
        drawable_surface = 0;
    
    rct_gst_set_pipeline_state(GST_STATE_NULL);
    gst_object_unref(pipeline);
    
    g_source_remove(bus_watch_id);
    g_main_loop_unref(main_loop);
    
    g_free(configuration->audioLevelRefreshRate);
    g_free(configuration);
    g_free(audio_level);
    
    pipeline = NULL;
    configuration = NULL;
    audio_level = NULL;
}

gchar *rct_gst_get_info()
{
    return gst_version_string();
}

void apply_uri()
{
    g_print(rct_gst_player, "Applying URI: %s", rct_gst_get_configuration()->uri ? rct_gst_get_configuration()->uri : "(null)");
    rct_gst_set_pipeline_state(GST_STATE_READY);
    g_object_set(pipeline, "uri", rct_gst_get_configuration()->uri, NULL);

    if (rct_gst_get_configuration()->onUriChanged) {
        rct_gst_get_configuration()->onUriChanged(rct_gst_get_configuration()->uri);
    }
}
