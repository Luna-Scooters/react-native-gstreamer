//
//  gstreamer_codec.c
//
//  See gstreamer_codec.h.
//

#include "gstreamer_codec.h"

// Name of the best registered factory of `type` that handles `caps_str` on its
// `direction` pad. Prefers Hardware-classed factories, then rank — rank alone
// can't express "prefer hardware": MediaCodec elements register at SECONDARY
// while software ones like x264enc are PRIMARY. Falls back to `fallback`.
// Returned string must be g_free'd.
static gchar *find_best_element(GstElementFactoryListType type,
                                const gchar *caps_str,
                                GstPadDirection direction,
                                const gchar *fallback)
{
    GstCaps *caps = gst_caps_from_string(caps_str);
    GstElementFactoryListType tiers[2] = {
        type | GST_ELEMENT_FACTORY_TYPE_HARDWARE, // hardware first
        type,                                     // then software
    };

    gchar *name = NULL;
    for (gint i = 0; i < 2 && name == NULL; i++) {
        GList *factories = gst_element_factory_list_get_elements(tiers[i], GST_RANK_MARGINAL);
        GList *filtered = gst_element_factory_list_filter(factories, caps, direction, FALSE);
        if (filtered)
            name = g_strdup(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(filtered->data)));
        gst_plugin_feature_list_free(filtered);
        gst_plugin_feature_list_free(factories);
    }

    gst_caps_unref(caps);
    return name ? name : g_strdup(fallback);
}

gchar *rct_gst_find_h264_decoder(void)
{
    gchar *name = find_best_element(
        GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
        "video/x-h264", GST_PAD_SINK, "avdec_h264");
    g_print("Using H264 decoder: [%s]\n", name);
    return name;
}

gchar *rct_gst_find_h264_encoder(void)
{
    gchar *name = find_best_element(
        GST_ELEMENT_FACTORY_TYPE_ENCODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
        "video/x-h264", GST_PAD_SRC, "openh264enc");

    // MediaTek's amc H.264 encoder not working
    // Fallback to software encoder
    if (name && g_strstr_len(name, -1, "mtk")) {
        g_print("H264 encoder [%s] is the broken MediaTek amc encoder; "
                "using software openh264enc instead\n", name);
        g_free(name);
        name = g_strdup("openh264enc");
    }
    return name;
}

void rct_gst_configure_h264_encoder(GstElement *encoder, gint fps,
                                    gint keyframe_interval_sec,
                                    gint bitrate_kbps)
{
    if (encoder == NULL || fps <= 0)
        return;

    GObjectClass *klass = G_OBJECT_GET_CLASS(encoder);

    if (keyframe_interval_sec > 0) {
        gint frames = keyframe_interval_sec * fps;

        // Each encoder exposes the GOP length under a different property name and unit
        GstStructure *props = gst_structure_new(
            "keyframe-props",
            "i-frame-interval", G_TYPE_INT, keyframe_interval_sec,            // amcvidenc: seconds
            "key-int-max", G_TYPE_INT, frames,                                // x264enc: frames
            "gop-size", G_TYPE_INT, frames,                                   // openh264enc: frames
            "max-keyframe-interval", G_TYPE_INT, frames,                      // vtenc_h264: frames
            "max-keyframe-interval-duration",
            G_TYPE_UINT64, (guint64)keyframe_interval_sec * GST_SECOND,       // vtenc_h264: ns
            NULL);

        gint n = gst_structure_n_fields(props);
        for (gint i = 0; i < n; i++) {
            const gchar *name = gst_structure_nth_field_name(props, i);
            if (!g_object_class_find_property(klass, name))
                continue;
            g_object_set_property(G_OBJECT(encoder), name,
                                  gst_structure_get_value(props, name));
        }
        gst_structure_free(props);

        g_print("Encoder keyframe interval set to %ds (%d frames @ %dfps)\n",
                keyframe_interval_sec, frames, fps);
    }

    if (bitrate_kbps > 0 && g_object_class_find_property(klass, "bitrate")) {
        // The `bitrate` property unit is not consistent across encoders:
        // x264enc || vtenc_h264    -> kbit/s
        // amcvidenc || openh264enc -> bits/s.
        GstElementFactory *factory = gst_element_get_factory(encoder);
        const gchar *fname = factory ? GST_OBJECT_NAME(factory) : "";
        guint value = (guint)bitrate_kbps; // kbit/s: x264enc, vtenc_h264
        if (g_str_has_prefix(fname, "amcvidenc") ||
            g_str_has_prefix(fname, "openh264"))
            value = (guint)bitrate_kbps * 1000; // bits/s: amcvidenc, openh264enc

        g_object_set(encoder, "bitrate", value, NULL);
        g_print("Encoder bitrate set to %d kbps (property value %u on %s)\n",
                bitrate_kbps, value, fname);
    }
}
