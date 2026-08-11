//
//  gstreamer_backend.c
//
//  Created by Alann on 13/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#include "gstreamer_backend.h"
#include "gstreamer_codec.h"

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

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
static gchar *last_applied_uri = NULL;

// Audio
GstElement* audio_level_element;

// Sinks
GstElement *video_sink;
GstElement *audio_sink;

GstElement *video_tee; // Recording pipeline will attached here

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
        configuration->onRecordingFinished = NULL;
        configuration->onEventSaved = NULL;
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
    if (pipeline && last_applied_uri && _uri && g_strcmp0(last_applied_uri, _uri) == 0) {
        return;
    }

    g_free(last_applied_uri);
    last_applied_uri = g_strdup(_uri);

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
        video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "video-sink");
        
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
    if (!video_sink)
        return NULL;
    GstSample *sample = NULL;
    g_object_get(video_sink, "last-sample", &sample, NULL);
    return sample;
}

// Read the decoded stream's resolution and framerate
// Returns TRUE if width/height were resolved.
gboolean rct_gst_get_video_info(gint *width, gint *height, gint *fps)
{
    if (!video_tee)
        return FALSE;

    GstPad *sinkpad = gst_element_get_static_pad(video_tee, "sink");
    if (!sinkpad)
        return FALSE;
    GstCaps *caps = gst_pad_get_current_caps(sinkpad);
    gst_object_unref(sinkpad);
    if (!caps)
        return FALSE;

    const GstStructure *s = gst_caps_get_structure(caps, 0);
    gint w = 0, h = 0, fps_n = 0, fps_d = 1;
    gboolean ok = FALSE;

    if (gst_structure_get_int(s, "width", &w) &&
        gst_structure_get_int(s, "height", &h)) {
        if (width)  *width = w;
        if (height) *height = h;
        ok = TRUE;
    }
    if (fps && gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d) &&
        fps_n > 0 && fps_d > 0)
        *fps = fps_n / fps_d;

    gst_caps_unref(caps);
    return ok;
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

// Skip audio stream
static gboolean cb_select_stream(GstElement *src, guint num, GstCaps *caps, gpointer user_data)
{
    const GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : NULL;
    const gchar *media = s ? gst_structure_get_string(s, "media") : NULL;
    if (media && g_strcmp0(media, "audio") == 0)
        return FALSE;
    return TRUE;
}

// Enforces link between sink and src (bug on iOS)
static void cb_rtsp_pad_added(GstElement *src, GstPad *new_pad, gpointer user_data)
{
    (void)src; (void)user_data;
    GstElement *depay = gst_bin_get_by_name(GST_BIN(pipeline), "rtpjpegdepay0");
    if (!depay)
        return;
    GstPad *sinkpad = gst_element_get_static_pad(depay, "sink");
    if (!gst_pad_is_linked(sinkpad)) {
        GstPadLinkReturn ret = gst_pad_link(new_pad, sinkpad);
        if (GST_PAD_LINK_FAILED(ret))
            g_printerr("pad-added: rtspsrc -> rtpjpegdepay link failed (%d)\n", ret);
    }
    gst_object_unref(sinkpad);
    gst_object_unref(depay);
}

static void rct_gst_dump_pipeline(GstBin *bin, gint depth)
{
    GstIterator *it = gst_bin_iterate_elements(bin);
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
            case GST_ITERATOR_OK: {
                GstElement *el = GST_ELEMENT(g_value_get_object(&item));
                gchar *name = gst_element_get_name(el);
                GstElementFactory *f = gst_element_get_factory(el);
                g_print("[GST-PIPE] %*s%s [%s]\n", depth * 2, "",
                        name, f ? GST_OBJECT_NAME(f) : "?");
                g_free(name);
                if (GST_IS_BIN(el))
                    rct_gst_dump_pipeline(GST_BIN(el), depth + 1);
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(it);
                break;
            default:
                done = TRUE;
                break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
}

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
#if defined(__APPLE__)
    dispatch_sync(dispatch_get_main_queue(), ^{
        rct_gst_set_pipeline_state(GST_STATE_NULL);
    });
#else
    rct_gst_set_pipeline_state(GST_STATE_NULL);
#endif
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
        if (new_state == GST_STATE_PLAYING) {
            rct_gst_dump_pipeline(GST_BIN(pipeline), 0);
        }

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

/*************
 OTHER METHODS
 ************/
GstStateChangeReturn rct_gst_set_pipeline_state(GstState state)
{
    g_print("Pipeline state requested : %s\n", gst_element_state_get_name(state));

    if (rct_gst_is_recording() && pipeline && state < GST_STATE_PLAYING) {
        g_print("Recording active - deferring state change until finalized\n");
        rct_gst_recorder_defer_state(state);
        return GST_STATE_CHANGE_ASYNC;
    }

    GstStateChangeReturn validity = gst_element_set_state(pipeline, state);
    g_print("Validity : %s\n", gst_element_state_change_return_get_name(validity));

    return validity;
}

static GstPadProbeReturn
strip_rtpjpeg_header(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gsize size = gst_buffer_get_size(buffer);

    if (size < 4)
        return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_OK;

    /* RTSP server might send the full JPEG (SOI→EOI) as the RTP payload instead of
     * RFC 2435 scan-data-only, so rtpjpegdepay prepends its own reconstructed
     * header before the original JPEG, producing a buffer with two SOI markers.
     * Find the second 0xFF 0xD8 and strip everything before it so jpegparse
     * sees exactly one valid JPEG per buffer. */
    gsize second_soi = (gsize)-1;
    for (gsize i = 2; i + 1 < map.size; i++) {
        if (map.data[i] == 0xFF && map.data[i + 1] == 0xD8) {
            second_soi = i;
            break;
        }
    }
    gst_buffer_unmap(buffer, &map);

    if (second_soi == (gsize)-1)
        return GST_PAD_PROBE_OK;

    GST_LOG_OBJECT(pad, "Stripping %zu-byte header prefix in rtpjpegdepay ", second_soi);

    GstBuffer *stripped = gst_buffer_copy_region(buffer, GST_BUFFER_COPY_ALL,
                                                  second_soi, size - second_soi);
    if (!stripped)
        return GST_PAD_PROBE_OK;
    gst_mini_object_replace((GstMiniObject **)&info->data, (GstMiniObject *)stripped);
    gst_buffer_unref(stripped);

    return GST_PAD_PROBE_OK;
}

void rct_gst_init(RctGstConfiguration *configuration)
{
    gchar *launch_command_debug = "videotestsrc ! glimagesink name=video-sink";
    gchar *launch_command_app;

    rct_gst_recorder_reset();
    rct_gst_event_recorder_reset();
    rct_gst_encoder_reset();
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus_watch_id) {
            g_source_remove(bus_watch_id);
            bus_watch_id = 0;
        }
        gst_object_unref(pipeline);
        pipeline = NULL;
    }
    if (video_sink) {
        gst_object_unref(video_sink);
        video_sink = NULL;
    }
    video_overlay = NULL;
    video_tee = NULL;

#if defined(__APPLE__)
    const gchar *render_tail =
        "vulkanupload ! vulkancolorconvert "
        "! vulkansink name=video-sink enable-last-sample=true";
#else
    const gchar *render_tail =
        "autovideoconvert ! glimagesink sync=false name=video-sink";
#endif
    gchar *selected_decoder = rct_gst_find_jpeg_decoder();
    gchar *pipeline_template =
        "rtspsrc is-live=true protocols=tcp latency=0 name=src "
        "! rtpjitterbuffer latency=500 drop-on-latency=true do-lost=true name=jitterbuffer "
        "! rtpjpegdepay name=rtpjpegdepay0 "
        "! jpegparse "
        "! %s "
        "! tee name=video-tee "
        "! queue "
        "! %s";
    launch_command_app = g_strdup_printf(pipeline_template, selected_decoder, render_tail);
    g_free(selected_decoder);

    // Prepare pipeline. If not working, will display an error video signal
    gchar *launch_command = (!rct_gst_get_configuration()->isDebugging) ? launch_command_app : launch_command_debug;
    GError *error = NULL;
    pipeline = gst_parse_launch(launch_command, &error);
    if (error != NULL) {
        g_printerr("Error creating pipeline: %s\n", error->message);
        g_error_free(error);
        return;
    }

    /* Strip the rtpjpegdepay-prepended reconstructed header before jpegparse
     * sees the buffer, so it receives exactly one valid JPEG per frame. */
    GstElement *depay_elem = gst_bin_get_by_name(GST_BIN(pipeline), "rtpjpegdepay0");
    if (depay_elem) {
        GstPad *src_pad = gst_element_get_static_pad(depay_elem, "src");
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, strip_rtpjpeg_header, NULL, NULL);
        gst_object_unref(src_pad);
        gst_object_unref(depay_elem);
    }

    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), "video-tee");
    if (tee) {
        video_tee = tee;               // borrowed, the pipeline owns it
        gst_object_unref(tee);
    }

    // Reject the camera's audio stream at RTSP SETUP time. Critical on iOS: a
    // dangling (or even set-up) audio stream starves video over the shared
    // interleaved-TCP connection -> black screen. It also stops the camera from
    // sending PCMA packets the pipeline would only discard.
    GstElement *src_element = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (src_element) {
        g_signal_connect(src_element, "select-stream", G_CALLBACK(cb_select_stream), NULL);
        g_signal_connect(src_element, "pad-added", G_CALLBACK(cb_rtsp_pad_added), NULL);
        gst_object_unref(src_element);
    }

    // Preparing bus
    bus = gst_element_get_bus(pipeline);
    bus_watch_id = gst_bus_add_watch(bus, cb_bus_watch, NULL);
    
    // First time, need a surface to draw on - then use rct_gst_set_drawable_surface
    rct_gst_set_drawable_surface(rct_gst_get_configuration()->initialDrawableSurface);
    gst_bus_set_sync_handler(bus,(GstBusSyncHandler)cb_create_window, pipeline, NULL);
    gst_object_unref(bus);

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
    
    if(drawable_surface)
        drawable_surface = 0;

    rct_gst_recorder_reset();
    rct_gst_event_recorder_reset();
    rct_gst_encoder_reset();

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
    video_sink = NULL;
    video_overlay = NULL;
    video_tee = NULL;
    g_free(last_applied_uri);
    last_applied_uri = NULL;
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
