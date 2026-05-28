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

// RTSP dual-branch selector state
GstElement *video_selector;
GstPad *video_selector_jpeg_pad;
GstPad *video_selector_h264_pad;
gboolean video_selector_locked;

static const DecodersMap decoders_map[] = {
    // List of Android JPEG decoders
    {
        "JPEG",
        (const gchar*[]) {
            "amcviddec-omxgooglejpegdecoder",     // Google OMX decoder
            "amcviddec-qcomjpegdecoder",          // Qualcomm decoder
            "amcviddec-omxqcomjpegdecoder",       // Qualcomm OMX decoder
            "amcviddec-c2googlejpegdecoder",      // Codec 2.0 Google decoder
            "amcviddec",                          // Generic Android Media Codec
            "avdec_mjpeg",                        // libav software decoder
            "jpegdec",                            // Software decoder (fallback)
            NULL
        }
    },
    // List of Android H264 decoders
    {
        "H264",
        (const gchar*[]) {
            "amcviddec-omxgoogleh264decoder",     // Google OMX decoder
            "amcviddec-c2googleh264decoder",      // Codec 2.0 Google decoder
            "amcviddec-omxqcomvideodecoderavc",   // Qualcomm OMX decoder
            "amcviddec-qcomvideodecoderavc",      // Qualcomm decoder
            "avdec_h264",                         // Software decoder
            "vtdec",                              // iOS VideoToolbox decoder
            "openh264dec",                        // OpenH264 decoder
            "decodebin",                          // Generic fallback decoder
            NULL
        }
     },
     { NULL, NULL } // Sentinel to mark the end of the array
};
    
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
            
        default:
            break;
    }
    
    return TRUE;
}

static void select_video_branch(GstPad *selector_pad, const gchar *branch_name)
{
    if (video_selector_locked || video_selector == NULL || selector_pad == NULL)
        return;

    g_object_set(video_selector, "active-pad", selector_pad, NULL);
    video_selector_locked = TRUE;
    g_print("Active video branch selected : [%s]\n", branch_name);
}

static GstPadProbeReturn cb_probe_jpeg_branch(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        select_video_branch(video_selector_jpeg_pad, "JPEG");
        return GST_PAD_PROBE_REMOVE;
    }

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn cb_probe_h264_branch(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        select_video_branch(video_selector_h264_pad, "H264");
        return GST_PAD_PROBE_REMOVE;
    }

    return GST_PAD_PROBE_OK;
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
    gchar *pipeline_builder =
        "rtspsrc is-live=true protocols=tcp latency=0 name=src "
        "input-selector name=video-selector sync-mode=clock cache-buffers=false "
        "! videorate drop-out-of-segment=true "
        "! video/x-raw,framerate=10/1 "
        "! videoconvert "
        "! glimagesink sync=false name=video-sink ";
    gchar *branch_template =
        "src. ! application/x-rtp,media=video,encoding-name=%s "
        "! rtp%sdepay "
        "! %sparse "
        "! %s "
        "! queue name=%s-queue leaky=downstream max-size-buffers=2 "
        "! video-selector.sink_%d ";

    for (int i = 0; decoders_map[i].stream_type != NULL; i++) {
        GstElementFactory *factory = NULL;
        const gchar *selected_decoder = NULL;
        for (int j = 0; decoders_map[i].decoders[j] != NULL; j++) {
            factory = gst_element_factory_find(decoders_map[i].decoders[j]);
            if (factory) {
                g_print("Selected %s decoder: %s\n", decoders_map[i].stream_type, decoders_map[i].decoders[j]);
                selected_decoder = decoders_map[i].decoders[j];
                gst_object_unref(factory);

                g_print("Selected %s decoder: %s\n", decoders_map[i].stream_type, selected_decoder);
                break;
            }
        }

        if (selected_decoder == NULL) {
            g_printerr("No decoder found for stream type %s; skipping branch.\n", decoders_map[i].stream_type);
            continue;
        }

        gchar *stream_type_lower = g_ascii_strdown(decoders_map[i].stream_type, -1);
        gchar *branch = g_strdup_printf(branch_template,
                                        decoders_map[i].stream_type,
                                        stream_type_lower,
                                        stream_type_lower,
                                        selected_decoder,
                                        stream_type_lower,
                                        i);
        pipeline_builder = g_strconcat(pipeline_builder, branch, NULL);
        g_free(branch);
        g_free(stream_type_lower);
    }

    // Prepare pipeline. If not working, will display an error video signal
    gchar *launch_command = (!rct_gst_get_configuration()->isDebugging) ? pipeline_builder : launch_command_debug;
    GError *error = NULL;
    pipeline = gst_parse_launch(launch_command, &error);
    if (pipeline_builder) {
        g_free(pipeline_builder);
    }

    if (error != NULL) {
        g_printerr("Error creating pipeline: %s\n", error->message);
        g_error_free(error);
        return;
    }

    // Reset selector state for the new pipeline instance.
    video_selector = NULL;
    video_selector_jpeg_pad = NULL;
    video_selector_h264_pad = NULL;
    video_selector_locked = FALSE;

    // Use the first branch that really receives buffers.
    video_selector = gst_bin_get_by_name(GST_BIN(pipeline), "video-selector");
    GstElement *jpeg_queue = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg-queue");
    GstElement *h264_queue = gst_bin_get_by_name(GST_BIN(pipeline), "h264-queue");
    if (video_selector && jpeg_queue && h264_queue) {
        video_selector_jpeg_pad = gst_element_get_static_pad(video_selector, "sink_0");
        video_selector_h264_pad = gst_element_get_static_pad(video_selector, "sink_1");

        // Prefer H264 initially, but allow runtime switch if JPEG buffers arrive first.
        if (video_selector_h264_pad) {
            g_object_set(video_selector, "active-pad", video_selector_h264_pad, NULL);
        }

        GstPad *jpeg_src_pad = gst_element_get_static_pad(jpeg_queue, "src");
        GstPad *h264_src_pad = gst_element_get_static_pad(h264_queue, "src");
        if (jpeg_src_pad) {
            gst_pad_add_probe(jpeg_src_pad, GST_PAD_PROBE_TYPE_BUFFER, cb_probe_jpeg_branch, NULL, NULL);
            gst_object_unref(jpeg_src_pad);
        }
        if (h264_src_pad) {
            gst_pad_add_probe(h264_src_pad, GST_PAD_PROBE_TYPE_BUFFER, cb_probe_h264_branch, NULL, NULL);
            gst_object_unref(h264_src_pad);
        }
    }
    if (jpeg_queue)
        gst_object_unref(jpeg_queue);
    if (h264_queue)
        gst_object_unref(h264_queue);
    
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
    if(video_sink != NULL)
        gst_object_unref(video_sink);
    
    if(video_overlay != NULL)
        gst_object_unref(video_overlay);

    if(video_selector_jpeg_pad != NULL) {
        gst_object_unref(video_selector_jpeg_pad);
        video_selector_jpeg_pad = NULL;
    }

    if(video_selector_h264_pad != NULL) {
        gst_object_unref(video_selector_h264_pad);
        video_selector_h264_pad = NULL;
    }

    if(video_selector != NULL) {
        gst_object_unref(video_selector);
        video_selector = NULL;
    }

    video_selector_locked = FALSE;
    
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
