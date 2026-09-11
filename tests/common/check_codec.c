#include <gst/check/gstcheck.h>

#include "tests_platform.h"

#include "gstreamer_codec.h"

static GstElementFactory *
require_factory (const gchar * name)
{
  GstElementFactory *factory = gst_element_factory_find (name);

  fail_unless (factory != NULL, "selected element \"%s\" is not registered", name);

  return factory;
}

GST_START_TEST (test_h264_decoder_selection_is_registered)
{
  gchar *name = rct_gst_find_h264_decoder ();

  fail_unless (name != NULL, "decoder selection returned NULL");
  GST_INFO ("selected H264 decoder: %s", name);
  gst_object_unref (require_factory (name));

  g_free (name);
}

GST_END_TEST;

GST_START_TEST (test_jpeg_decoder_selection_is_registered)
{
  gchar *name = rct_gst_find_jpeg_decoder ();

  fail_unless (name != NULL, "decoder selection returned NULL");
  GST_INFO ("selected JPEG decoder: %s", name);
  gst_object_unref (require_factory (name));

  g_free (name);
}

GST_END_TEST;

GST_START_TEST (test_h264_encoder_selection_is_registered)
{
  gchar *name = rct_gst_find_h264_encoder ();

  fail_unless (name != NULL, "encoder selection returned NULL");
  GST_INFO ("selected H264 encoder: %s", name);
  gst_object_unref (require_factory (name));

  fail_if (g_strstr_len (name, -1, "mtk") != NULL,
      "selection returned the broken MediaTek encoder: %s", name);

  g_free (name);
}

GST_END_TEST;

/* The selected name is interpolated into the parse-launch string that
 * rct_gst_link_decode_chain() builds, so it has to parse in place. */
GST_START_TEST (test_selected_decoders_parse_in_the_decode_chain)
{
  gchar *h264 = rct_gst_find_h264_decoder ();
  gchar *jpeg = rct_gst_find_jpeg_decoder ();
  gchar *chains[] = {
    g_strdup_printf ("rtph264depay ! h264parse ! %s", h264),
    g_strdup_printf ("rtpjpegdepay name=rtpjpegdepay0 ! jpegparse ! %s", jpeg),
  };

  for (gsize i = 0; i < G_N_ELEMENTS (chains); i++) {
    GError *error = NULL;
    GstElement *bin = gst_parse_bin_from_description (chains[i], TRUE, &error);

    fail_unless (bin != NULL && error == NULL, "decode chain did not parse: %s (%s)",
        chains[i], error ? error->message : "no error set");
    g_clear_error (&error);
    if (bin)
      gst_object_unref (bin);
    g_free (chains[i]);
  }

  g_free (h264);
  g_free (jpeg);
}

GST_END_TEST;

/* Every encoder spells the bitrate unit and the GOP length differently, and
 * getting either wrong is silent rather than an error. */
GST_START_TEST (test_encoder_configuration_units)
{
  const gint fps = 10;
  const gint keyframe_interval_sec = 2;
  const gint bitrate_kbps = 2000;
  const gint expected_frames = keyframe_interval_sec * fps;

  gchar *name = rct_gst_find_h264_encoder ();
  GstElement *encoder;
  GObjectClass *klass;
  gboolean saw_gop_property = FALSE;

  fail_unless (name != NULL);

  encoder = gst_element_factory_make (name, "enc");
  fail_unless (encoder != NULL, "could not instantiate encoder %s", name);

  rct_gst_configure_h264_encoder (encoder, fps, keyframe_interval_sec, bitrate_kbps);

  klass = G_OBJECT_GET_CLASS (encoder);

  if (g_object_class_find_property (klass, "bitrate")) {
    gboolean bits_per_second = g_str_has_prefix (name, "openh264")
        || g_str_has_prefix (name, "amcvidenc");
    guint expected = bits_per_second ? (guint) bitrate_kbps * 1000 : (guint) bitrate_kbps;
    guint actual = 0;

    g_object_get (encoder, "bitrate", &actual, NULL);
    fail_unless_equals_int (actual, expected);
  }

  {
    struct
    {
      const gchar *property;
      gint expected;
    } gop[] = {
      { "key-int-max", expected_frames },          /* x264enc: frames */
      { "gop-size", expected_frames },             /* openh264enc: frames */
      { "max-keyframe-interval", expected_frames },/* vtenc_h264: frames */
      { "i-frame-interval", keyframe_interval_sec },/* amcvidenc: seconds */
    };

    for (gsize i = 0; i < G_N_ELEMENTS (gop); i++) {
      gint actual = 0;

      if (!g_object_class_find_property (klass, gop[i].property))
        continue;

      g_object_get (encoder, gop[i].property, &actual, NULL);
      saw_gop_property = TRUE;
      fail_unless_equals_int (actual, gop[i].expected);
    }
  }

  if (!saw_gop_property)
    GST_INFO ("%s exposes none of the known GOP properties", name);

  gst_object_unref (encoder);
  g_free (name);
}

GST_END_TEST;

GST_START_TEST (test_encoder_configuration_rejects_nonsense)
{
  gchar *name = rct_gst_find_h264_encoder ();
  GstElement *encoder;
  GObjectClass *klass;
  guint before = 0, after = 0;

  fail_unless (name != NULL);
  encoder = gst_element_factory_make (name, "enc");
  fail_unless (encoder != NULL);
  klass = G_OBJECT_GET_CLASS (encoder);

  if (g_object_class_find_property (klass, "bitrate"))
    g_object_get (encoder, "bitrate", &before, NULL);

  rct_gst_configure_h264_encoder (encoder, 0, 2, 2000);
  rct_gst_configure_h264_encoder (NULL, 10, 2, 2000);

  if (g_object_class_find_property (klass, "bitrate")) {
    g_object_get (encoder, "bitrate", &after, NULL);
    fail_unless_equals_int (after, before);
  }

  gst_object_unref (encoder);
  g_free (name);
}

GST_END_TEST;

// find_best_element() asks for `type | GST_ELEMENT_FACTORY_TYPE_HARDWARE` first,
// intending to prefer hardware. That does not work: the category bits are OR-ed
// by gst_element_factory_list_is_type, so adding HARDWARE widens the query
// instead of restricting it. Both tiers return the same list and selection
// reduces to plain rank order. Pinned here because the consequence is silent -
// on Android amcviddec* rank SECONDARY while avdec_h264 ranks PRIMARY, so the
// software decoder wins even though hardware was asked for first.
GST_START_TEST (test_hardware_factory_type_does_not_filter)
{
  GList *hardware_only = gst_element_factory_list_get_elements (
      GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO |
      GST_ELEMENT_FACTORY_TYPE_HARDWARE, GST_RANK_MARGINAL);
  gboolean found_software_decoder = FALSE;

  for (GList *l = hardware_only; l; l = l->next) {
    GstElementFactory *factory = GST_ELEMENT_FACTORY (l->data);
    const gchar *klass = gst_element_factory_get_metadata (factory,
        GST_ELEMENT_METADATA_KLASS);

    if (klass && g_strstr_len (klass, -1, "Hardware") == NULL) {
      GST_INFO ("non-hardware factory returned by a HARDWARE query: %s (%s)",
          gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), klass);
      found_software_decoder = TRUE;
      break;
    }
  }

  gst_plugin_feature_list_free (hardware_only);

  fail_unless (found_software_decoder,
      "GST_ELEMENT_FACTORY_TYPE_HARDWARE now filters by klass; "
      "find_best_element()'s two tiers are no longer equivalent and its "
      "hardware preference should be re-checked");
}

GST_END_TEST;

static Suite *
codec_suite (void)
{
  Suite *s = suite_create ("codec");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (s, tc);
  tcase_add_unchecked_fixture (tc, tests_platform_setup, NULL);
  tcase_add_test (tc, test_h264_decoder_selection_is_registered);
  tcase_add_test (tc, test_jpeg_decoder_selection_is_registered);
  tcase_add_test (tc, test_h264_encoder_selection_is_registered);
  tcase_add_test (tc, test_selected_decoders_parse_in_the_decode_chain);
  tcase_add_test (tc, test_encoder_configuration_units);
  tcase_add_test (tc, test_encoder_configuration_rejects_nonsense);
  tcase_add_test (tc, test_hardware_factory_type_does_not_filter);

  return s;
}

GST_CHECK_MAIN (codec);
