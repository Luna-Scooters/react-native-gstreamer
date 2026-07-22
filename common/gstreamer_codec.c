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
