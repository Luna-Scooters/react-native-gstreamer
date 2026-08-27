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

// Serializes video_sink between the main thread (which swaps it on surface
// changes and drops it on re-init / terminate) and the image-capture thread
// (rct_gst_pull_last_sample). All backend state lives in these file globals and
// is shared by every player instance, so a new instance's rct_gst_init can race
// an old instance's still-running capture loop.
static GMutex video_sink_lock;

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

    // Own the string: callers pass transient buffers (JNI GetStringUTFChars,
    // -[NSString UTF8String]) that are released as soon as they return, so
    // storing the caller's pointer would leave configuration->uri dangling.
    RctGstConfiguration *cfg = rct_gst_get_configuration();
    gchar *previous_uri = cfg->uri;
    cfg->uri = g_strdup(_uri);
    g_free(previous_uri);

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
        video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "video-sink");
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

static void rct_gst_dump_pipeline(GstBin *bin, gint depth);

// Builds the depay/parse/decode chain matching whichever codec the camera
// actually negotiated, then wires it between the jitterbuffer and the tee.
// Deferred to here because the codec isn't known until RTSP SETUP completes
// at runtime. `caps` must be the negotiated RTP caps (from the CAPS event,
// not gst_pad_get_current_caps() — at pad-added time the pad may not have
// its current caps set yet, even though the CAPS event carrying them is
// about to be pushed).
static void rct_gst_link_decode_chain(GstCaps *caps)
{
    GstElement *jitterbuffer = gst_bin_get_by_name(GST_BIN(pipeline), "jitterbuffer");
    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), "video-tee");
    if (!jitterbuffer || !tee) {
        if (jitterbuffer) gst_object_unref(jitterbuffer);
        if (tee) gst_object_unref(tee);
        return;
    }

    GstPad *jb_src = gst_element_get_static_pad(jitterbuffer, "src");
    gboolean already_built = gst_pad_is_linked(jb_src);
    gst_object_unref(jb_src);
    if (already_built) {
        gst_object_unref(jitterbuffer);
        gst_object_unref(tee);
        return;
    }

    const GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : NULL;
    const gchar *encoding = s ? gst_structure_get_string(s, "encoding-name") : NULL;
    gint payload = -1;
    if (s) gst_structure_get_int(s, "payload", &payload);

    // RFC 3551 assigns PT 26 to JPEG statically, so a server sending it may
    // legally omit the a=rtpmap line that would otherwise set encoding-name.
    gboolean is_jpeg = (encoding && g_ascii_strcasecmp(encoding, "JPEG") == 0) ||
                        (!encoding && payload == 26);
    gboolean is_h264 = encoding && g_ascii_strcasecmp(encoding, "H264") == 0;

    gchar *decoder;
    gchar *chain_desc;
    if (is_jpeg) {
        decoder = rct_gst_find_jpeg_decoder();
        chain_desc = g_strdup_printf("rtpjpegdepay name=rtpjpegdepay0 ! jpegparse ! %s", decoder);
    } else if (is_h264) {
        decoder = rct_gst_find_h264_decoder();
        chain_desc = g_strdup_printf("rtph264depay ! h264parse ! %s", decoder);
    } else {
        gchar *caps_str = caps ? gst_caps_to_string(caps) : NULL;
        GST_ELEMENT_ERROR(pipeline, STREAM, CODEC_NOT_FOUND,
                           ("Unsupported RTSP video encoding"),
                           ("encoding-name=%s, caps=%s",
                            encoding ? encoding : "unknown",
                            caps_str ? caps_str : "none"));
        g_free(caps_str);
        gst_object_unref(jitterbuffer);
        gst_object_unref(tee);
        return;
    }
    g_free(decoder);

    GError *error = NULL;
    GstElement *chain = gst_parse_bin_from_description(chain_desc, TRUE, &error);
    g_print("Decode chain: %s\n", chain_desc);
    g_free(chain_desc);
    if (error != NULL) {
        g_printerr("Failed to build decode chain: %s\n", error->message);
        g_error_free(error);
        gst_object_unref(jitterbuffer);
        gst_object_unref(tee);
        return;
    }

    gst_bin_add(GST_BIN(pipeline), chain);

    if (is_jpeg) {
        GstElement *depay_elem = gst_bin_get_by_name(GST_BIN(chain), "rtpjpegdepay0");
        if (depay_elem) {
            GstPad *depay_src = gst_element_get_static_pad(depay_elem, "src");
            gst_pad_add_probe(depay_src, GST_PAD_PROBE_TYPE_BUFFER, strip_rtpjpeg_header, NULL, NULL);
            gst_object_unref(depay_src);
            gst_object_unref(depay_elem);
        }
    }

    GstPad *chain_sink = gst_element_get_static_pad(chain, "sink");
    jb_src = gst_element_get_static_pad(jitterbuffer, "src");
    if (GST_PAD_LINK_FAILED(gst_pad_link(jb_src, chain_sink)))
        g_printerr("Failed to link rtpjitterbuffer -> decode chain\n");
    gst_object_unref(chain_sink);
    gst_object_unref(jb_src);

    GstPad *chain_src = gst_element_get_static_pad(chain, "src");
    GstPad *tee_sink = gst_element_get_static_pad(tee, "sink");
    if (GST_PAD_LINK_FAILED(gst_pad_link(chain_src, tee_sink)))
        g_printerr("Failed to link decode chain -> tee\n");
    gst_object_unref(chain_src);
    gst_object_unref(tee_sink);

    gst_element_sync_state_with_parent(chain);

    gst_object_unref(jitterbuffer);
    gst_object_unref(tee);

    // The pipeline may already be PLAYING by the time this dynamic chain is
    // wired in (RTSP negotiation completes asynchronously), so the state-
    // changed-driven dump in cb_state_changed can miss it entirely. Dump here
    // too so the printed layout always reflects the codec actually in use.
    rct_gst_dump_pipeline(GST_BIN(pipeline), 0);
}

// Catches the CAPS event on rtspsrc's dynamic pad — the authoritative point
// at which the negotiated encoding (JPEG/H264) is known — and builds the
// decode chain from it.
static GstPadProbeReturn cb_rtsp_pad_caps_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
        return GST_PAD_PROBE_OK;

    GstCaps *caps;
    gst_event_parse_caps(event, &caps);
    rct_gst_link_decode_chain(caps);

    return GST_PAD_PROBE_REMOVE;
}

// Enforces link between sink and src (bug on iOS)
static void cb_rtsp_pad_added(GstElement *src, GstPad *new_pad, gpointer user_data)
{
    (void)src; (void)user_data;
    GstElement *jitterbuffer = gst_bin_get_by_name(GST_BIN(pipeline), "jitterbuffer");
    if (!jitterbuffer)
        return;
    GstPad *sinkpad = gst_element_get_static_pad(jitterbuffer, "sink");
    if (!gst_pad_is_linked(sinkpad)) {
        GstPadLinkReturn ret = gst_pad_link(new_pad, sinkpad);
        if (GST_PAD_LINK_FAILED(ret))
            g_printerr("pad-added: rtspsrc -> rtpjitterbuffer link failed (%d)\n", ret);
    }
    gst_object_unref(sinkpad);
    gst_object_unref(jitterbuffer);

    gst_pad_add_probe(new_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                       cb_rtsp_pad_caps_probe, NULL, NULL);
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

    if (!pipeline) {
        g_printerr("Pipeline state change ignored : no pipeline\n");
        return GST_STATE_CHANGE_FAILURE;
    }

    if (rct_gst_is_recording() && pipeline && state < GST_STATE_PLAYING) {
        g_print("Recording active - deferring state change until finalized\n");
        rct_gst_recorder_defer_state(state);
        return GST_STATE_CHANGE_ASYNC;
    }

    GstStateChangeReturn validity = gst_element_set_state(pipeline, state);
    g_print("Validity : %s\n", gst_element_state_change_return_get_name(validity));

    return validity;
}

// Destroy the pipeline and everything hanging off it. Ordering matters: nothing
// may still be able to reach the pipeline once its last ref is dropped.
//
//   1. consumer branches (they hold request pads on the pipeline's tees),
//   2. the bus watch AND the bus sync handler - the sync handler runs on a
//      streaming thread and reads video_overlay / drawable_surface, and the watch
//      dereferences `pipeline` from the GMainLoop thread, which is NOT the thread
//      calling this on either platform,
//   3. the state change to NULL, which joins the streaming threads,
//   4. the last ref.
//
// Safe to call with no pipeline, and safe to call twice.
static void destroy_pipeline(void)
{
    rct_gst_recorder_reset();
    rct_gst_event_recorder_reset();
    rct_gst_encoder_reset();

    if (bus_watch_id) {
        g_source_remove(bus_watch_id);
        bus_watch_id = 0;
    }

    if (pipeline) {
        GstBus *pipeline_bus = gst_element_get_bus(pipeline);
        if (pipeline_bus) {
            gst_bus_set_sync_handler(pipeline_bus, NULL, NULL, NULL);
            gst_object_unref(pipeline_bus);
        }

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    }
    bus = NULL;   // unreffed in rct_gst_init; never dereference it again

    // video_overlay is a cast of video_sink, NOT a second ref - clearing the
    // pointer is the whole job. Unreffing it too would over-unref the sink.
    g_mutex_lock(&video_sink_lock);
    GstElement *sink = video_sink;
    video_sink = NULL;
    g_mutex_unlock(&video_sink_lock);
    if (sink)
        gst_object_unref(sink);

    video_overlay = NULL;
    video_tee = NULL;
    audio_level_element = NULL;   // owned by the bin from create_audio_sink()
}

void rct_gst_init(RctGstConfiguration *configuration)
{
    const gchar *launch_command_debug = "videotestsrc ! glimagesink name=video-sink";

    destroy_pipeline();

    // The depay/parse/decode chain depends on the codec the camera actually
    // negotiates (JPEG or H264), which isn't known until RTSP SETUP completes,
    // so it's built dynamically in rct_gst_link_decode_chain() once rtspsrc's
    // pad appears. Only the codec-agnostic tail is static here.
    const gchar *launch_command_app =
        "rtspsrc is-live=true protocols=tcp latency=0 name=src "
        "! rtpjitterbuffer latency=500 drop-on-latency=true do-lost=true name=jitterbuffer "
        "tee name=video-tee "
        "! queue "
        "! videoconvert ! video/x-raw,format=NV12 "
        "! vulkanupload ! vulkancolorconvert "
        "! vulkansink sync=false name=video-sink enable-last-sample=true";
    g_print("Launch command: %s\n", launch_command_app);

    // Prepare pipeline. If not working, will display an error video signal
    const gchar *launch_command = (!rct_gst_get_configuration()->isDebugging) ? launch_command_app : launch_command_debug;
    GError *error = NULL;
    pipeline = gst_parse_launch(launch_command, &error);
    if (error != NULL) {
        g_printerr("Error creating pipeline: %s\n", error->message);
        g_error_free(error);
        // A failed parse can still hand back a partial pipeline; don't keep it,
        // every other entry point treats a non-NULL pipeline as usable.
        if (pipeline) {
            gst_object_unref(pipeline);
            pipeline = NULL;
        }
        return;
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
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    main_loop = loop;

    g_main_loop_run(loop);

    // The loop's ref belongs here, to the function that created it:
    // rct_gst_terminate() only quits it. g_main_loop_run holds its own ref while
    // running, so this is the unref that actually frees it - and it happens on
    // the loop thread, after the loop has stopped.
    if (main_loop == loop)
        main_loop = NULL;
    g_main_loop_unref(loop);
}

void rct_gst_terminate()
{
    // Tear the pipeline down first: this cancels the bus watch and every timer,
    // probe and branch that could still call back into the state freed below.
    destroy_pipeline();

    drawable_surface = 0;

    // Stop the GMainLoop. It is running on another thread (a pthread on Android,
    // a dispatch queue on iOS), so it owns its own ref and unrefs it in
    // rct_gst_run_loop once g_main_loop_run returns. Callers that need the
    // thread to be gone before they release the surface must join it.
    if (main_loop)
        g_main_loop_quit(main_loop);

    // `configuration` and `audio_level` are deliberately NOT freed. Both are lazy
    // process-lifetime singletons, and the bus watch reads them from the GMainLoop
    // thread: g_source_remove (in destroy_pipeline) unregisters the watch but does
    // NOT wait for a dispatch that is already running to return, so freeing them
    // here can race a live cb_state_changed / cb_message_element. Resetting them
    // instead costs one fixed allocation for the life of the process and removes
    // the use-after-free entirely - and the next rct_gst_init repopulates both.
    if (configuration) {
        g_free(configuration->uri);
        configuration->uri = NULL;

        configuration->isDebugging = FALSE;
        configuration->initialDrawableSurface = 0;
        if (configuration->audioLevelRefreshRate)
            *(configuration->audioLevelRefreshRate) = 100;

        configuration->onInit = NULL;
        configuration->onStateChanged = NULL;
        configuration->onVolumeChanged = NULL;
        configuration->onUriChanged = NULL;
        configuration->onEOS = NULL;
        configuration->onElementError = NULL;
        configuration->onRecordingFinished = NULL;
        configuration->onEventSaved = NULL;
    }

    if (audio_level) {
        audio_level->rms = 0;
        audio_level->peak = 0;
        audio_level->decay = 0;
    }

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
