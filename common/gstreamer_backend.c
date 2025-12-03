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

// Getters
RctGstConfiguration *rct_gst_get_configuration()
{
    if (!configuration) {
        configuration = g_malloc(sizeof(RctGstConfiguration));
        configuration->audioLevelRefreshRate = 100;
        configuration->uri = NULL;
        configuration->isDebugging = FALSE;
        
        configuration->onElementError = NULL;
        configuration->onStateChanged = NULL;
        configuration->onVolumeChanged = NULL;
        configuration->onUriChanged = NULL;
        
        configuration->onInit = NULL;
        configuration->onEOS = NULL;
    }
    return configuration;
}

RctGstAudioLevel *rct_gst_get_audio_level()
{
    if (!audio_level) {
        audio_level = g_malloc(sizeof(RctGstAudioLevel));
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
    rct_gst_get_configuration()->audioLevelRefreshRate = audio_level_refresh_rate;
    g_object_set(audio_level_element, "interval", audio_level_refresh_rate * 1000000, NULL);
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
    if(drawable_surface != (guintptr) NULL)
        drawable_surface = (guintptr) NULL;
    
    drawable_surface = _drawableSurface;
    
    if(pipeline)
    {
        // Always try to get the video-sink by name first (works for both debug and rtspsrc pipelines)
        video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "video-sink");
        
        if (!video_sink) {
            // If no named video-sink found, check if this is a playbin pipeline
            GstElement *src_element = gst_bin_get_by_name(GST_BIN(pipeline), "src");
            if (!src_element) {
                // This is likely a playbin pipeline - create and set glimagesink
                video_sink = gst_element_factory_make("glimagesink", "video-sink");
                g_object_set(GST_OBJECT(pipeline), "video-sink", video_sink, NULL);
            } else {
                gst_object_unref(src_element);
            }
        }
        
        // Configure the video sink if we have one
        if (video_sink) {
            // Configure for lower latency if it's not in debug mode
            if (!rct_gst_get_configuration()->isDebugging) {
                g_object_set(G_OBJECT(video_sink),
                            "sync", FALSE,
                            "async", FALSE,
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
    
    gst_video_overlay_set_window_handle(video_overlay, drawable_surface);
    
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

static const guint64 qos_count_max = 100;
static guint refresh_qos_count_ms = 10000; // 10 seconds
static guint64 qos_count = 0;
static gboolean cb_qos(GstBus *bus, GstMessage *message, gpointer user_data)
{
    // Show element name
    const GstStructure *s = gst_message_get_structure(message);
    if (s) {
        const gchar *element = gst_element_get_name(GST_MESSAGE_SRC(message));
        g_print("QoS event from element: %s\n", element);
    }

    // Increment the QoS counter
    qos_count++;
    if (qos_count >= qos_count_max) {
        g_print("QoS counter reached maximum: %lu\n", qos_count);
        // Reset the counter
        qos_count = 0;
        restart_stream(user_data);
    }

    return TRUE;
}

static gboolean cb_reset_qos_counter(gpointer data) {
    qos_count = 0;
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
            
        case GST_MESSAGE_QOS:
            cb_qos(bus, message, user_data);
            break;

        default:
            break;
    }
    
    return TRUE;
}

static void cb_source_created(GstElement *pipe, GstElement *source) {
    g_object_set(source,
                "latency", 150, /* 150 ms */
                "buffer-mode", 1, /* Slave receiver to sender clock */
                "drop-on-latency", FALSE,
                "ntp-sync", FALSE,
                "max-ts-offset", 50 * 1000 * 1000, /* 50 ms */
                "protocols", 0x04, /* TCP */
                "udp-buffer-size", 5242880, /* 5 MB */
                "max-rtcp-rtp-time-diff", 200 * 1000 * 1000, /* 200 ms */
                NULL);
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
    gchar *launch_command_debug = "videotestsrc ! glimagesink name=video-sink";
    gchar *launch_command_app;

    const gchar *decoder_name = "amcviddec-omxgoogleh264decoder";
    GstElementFactory *factory = gst_element_factory_find(decoder_name);
    if (factory)
    {
        g_print("Hardware Decoder available: [%s]\n", decoder_name);
        launch_command_app = "rtspsrc name=src is-live=true ! rtph264depay ! h264parse ! amcviddec-omxgoogleh264decoder ! glimagesink sync=false qos=false name=video-sink";
    }
    else
    {
        launch_command_app = "playbin video-sink=\"queue ! autovideosink sync=false\"";
    }

    // Prepare pipeline. If not working, will display an error video signal
    gchar *launch_command = (!rct_gst_get_configuration()->isDebugging) ? launch_command_app : launch_command_debug;
    GError *error = NULL;
    pipeline = gst_parse_launch(launch_command, &error);
    if (error != NULL) {
        g_printerr("Error creating pipeline: %s\n", error->message);
        g_error_free(error);
        return;
    }
    
    // Preparing bus
    bus = gst_element_get_bus(pipeline);
    bus_watch_id = gst_bus_add_watch(bus, cb_bus_watch, NULL);
    
    // First time, need a surface to draw on - then use rct_gst_set_drawable_surface
    rct_gst_set_drawable_surface(rct_gst_get_configuration()->initialDrawableSurface);
    gst_bus_set_sync_handler(bus,(GstBusSyncHandler)cb_create_window, pipeline, NULL);
    gst_object_unref(bus);

    g_signal_connect(pipeline, "source-setup", G_CALLBACK(cb_source_created), NULL);

    // Restart QoS Counter once in a while
    g_timeout_add(refresh_qos_count_ms, cb_reset_qos_counter, NULL);

    // Use fakesink to ignore audio
    audio_sink = gst_element_factory_make("fakesink", "audio-sink");
    g_object_set(pipeline, "audio-sink", audio_sink, NULL);

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
    if(video_sink != NULL)
        gst_object_unref(video_sink);
    
    if(video_overlay != NULL)
        gst_object_unref(video_overlay);
    
    if(drawable_surface != (guintptr) NULL)
        drawable_surface = (guintptr) NULL;
    
    rct_gst_set_pipeline_state(GST_STATE_NULL);
    gst_object_unref(pipeline);
    
    g_source_remove(bus_watch_id);
    g_main_loop_unref(main_loop);
    
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
    rct_gst_set_pipeline_state(GST_STATE_READY);

    // Check if this is a rtspsrc pipeline or playbin pipeline
    GstElement *src_element = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (src_element) {
        // This is the rtspsrc pipeline - set location on the rtspsrc element
        g_object_set(src_element, "location", rct_gst_get_configuration()->uri, NULL);
        gst_object_unref(src_element);
    } else {
        // This is the playbin pipeline - set uri on the pipeline
        g_object_set(pipeline, "uri", rct_gst_get_configuration()->uri, NULL);
    }

    if (rct_gst_get_configuration()->onUriChanged) {
        rct_gst_get_configuration()->onUriChanged(rct_gst_get_configuration()->uri);
    }
}
