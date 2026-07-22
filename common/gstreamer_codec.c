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
    GList *factories = gst_element_factory_list_get_elements(type, GST_RANK_MARGINAL);
    GstCaps *caps = gst_caps_from_string(caps_str);
    GList *filtered = gst_element_factory_list_filter(factories, caps, direction, FALSE);

    gchar *best_name = NULL;
    guint best_rank = 0;
    gboolean best_hw = FALSE;
    for (GList *l = filtered; l != NULL; l = l->next) {
        GstElementFactory *f = GST_ELEMENT_FACTORY(l->data);
        const gchar *klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
        gboolean hw = (klass && g_strstr_len(klass, -1, "Hardware")) ? TRUE : FALSE;
        guint rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(f));
        gboolean better = (best_name == NULL) || (hw && !best_hw) ||
                          (hw == best_hw && rank > best_rank);
        if (better) {
            g_free(best_name);
            best_name = g_strdup(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)));
            best_rank = rank;
            best_hw = hw;
        }
    }

    gst_caps_unref(caps);
    gst_plugin_feature_list_free(filtered);
    gst_plugin_feature_list_free(factories);
    return best_name ? best_name : g_strdup(fallback);
}

gchar *rct_gst_find_jpeg_decoder(void)
{
    gchar *name = find_best_element(
        GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
        "image/jpeg", GST_PAD_SINK, "jpegdec");
    g_print("Using JPEG decoder: [%s]\n", name);
    return name;
}

gchar *rct_gst_find_h264_encoder(void)
{
    return find_best_element(
        GST_ELEMENT_FACTORY_TYPE_ENCODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
        "video/x-h264", GST_PAD_SRC, "x264enc");
}

void rct_gst_configure_h264_encoder(GstElement *encoder, gint fps,
                                    gint keyframe_interval_sec,
                                    gint bitrate_kbps)
{
    if (encoder == NULL)
        return;
    if (fps <= 0)
        return

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
