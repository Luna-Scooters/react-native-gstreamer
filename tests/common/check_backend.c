#include <gst/check/gstcheck.h>

#include "tests_platform.h"

#include "gstreamer_backend.h"

#define CAMERA_URI "rtsp://192.168.0.1:554/livestream/1"

static gboolean init_fired;
static GstState last_state;

static void
on_init (void)
{
  init_fired = TRUE;
}

static void
on_state_changed (GstState old_state, GstState new_state)
{
  last_state = new_state;
}

/* isDebugging swaps the vulkan pipeline for "videotestsrc ! glimagesink",
 * which is the only one that parses on a host without the vulkan plugins. */
static RctGstConfiguration *
configure_debug_backend (void)
{
  RctGstConfiguration *config = rct_gst_get_configuration ();

  init_fired = FALSE;
  last_state = GST_STATE_VOID_PENDING;

  config->isDebugging = TRUE;
  config->initialDrawableSurface = 0;
  config->onInit = on_init;
  config->onStateChanged = on_state_changed;

  return config;
}

GST_START_TEST (test_configuration_is_a_process_singleton)
{
  RctGstConfiguration *config = rct_gst_get_configuration ();
  RctGstConfiguration *again = rct_gst_get_configuration ();
  gint *refresh_rate;

  fail_unless (config != NULL);
  fail_unless (config == again, "configuration is not a singleton");

  refresh_rate = config->audioLevelRefreshRate;
  fail_unless (refresh_rate != NULL);
  fail_unless_equals_int (*refresh_rate, 100);
}

GST_END_TEST;

/* Callers hand over transient buffers - JNI GetStringUTFChars, -[NSString
 * UTF8String] - that die as soon as they return. */
GST_START_TEST (test_uri_is_copied_not_borrowed)
{
  gchar *transient = g_strdup (CAMERA_URI);

  rct_gst_set_uri (transient);

  memset (transient, 'x', strlen (transient));
  g_free (transient);

  fail_unless_equals_string (rct_gst_get_configuration ()->uri, CAMERA_URI);

  rct_gst_terminate ();
}

GST_END_TEST;

GST_START_TEST (test_setting_the_same_uri_twice_is_idempotent)
{
  rct_gst_set_uri (CAMERA_URI);
  rct_gst_set_uri (CAMERA_URI);

  fail_unless_equals_string (rct_gst_get_configuration ()->uri, CAMERA_URI);

  rct_gst_terminate ();
}

GST_END_TEST;

GST_START_TEST (test_terminate_clears_the_uri)
{
  rct_gst_set_uri (CAMERA_URI);
  rct_gst_terminate ();

  fail_unless (rct_gst_get_configuration ()->uri == NULL,
      "terminate left a stale uri behind");
}

GST_END_TEST;

GST_START_TEST (test_video_info_is_unavailable_without_a_stream)
{
  gint width = -1, height = -1, fps = -1;

  rct_gst_terminate ();

  fail_if (rct_gst_get_video_info (&width, &height, &fps),
      "video info was reported with no pipeline running");
}

GST_END_TEST;

GST_START_TEST (test_get_info_reports_a_gstreamer_version)
{
  gchar *info = rct_gst_get_info ();

  fail_unless (info != NULL);
  fail_unless (g_str_has_prefix (info, "GStreamer"), "unexpected info: %s", info);

  g_free (info);
}

GST_END_TEST;

GST_START_TEST (test_init_builds_a_pipeline_and_reports_it)
{
  configure_debug_backend ();

  rct_gst_init (NULL);

  fail_unless (init_fired, "onInit never fired");

  rct_gst_terminate ();
}

GST_END_TEST;

/* rct_gst_init() ignores its argument entirely: it reads the singleton from
 * rct_gst_get_configuration(), and the parameter shadows the global of the same
 * name. Passing a different struct has no effect, so pin that contract. */
GST_START_TEST (test_init_ignores_its_configuration_argument)
{
  RctGstConfiguration decoy = { 0 };

  configure_debug_backend ();
  decoy.isDebugging = FALSE;
  decoy.onInit = NULL;

  rct_gst_init (&decoy);

  fail_unless (init_fired,
      "init used the passed-in struct instead of the singleton");

  rct_gst_terminate ();
}

GST_END_TEST;

/* Entering and leaving the camera screen repeatedly. */
GST_START_TEST (test_repeated_init_and_terminate)
{
  for (int i = 0; i < 10; i++) {
    configure_debug_backend ();
    rct_gst_set_uri (CAMERA_URI);

    rct_gst_init (NULL);
    fail_unless (init_fired, "cycle %d: onInit never fired", i);

    rct_gst_terminate ();
    fail_unless (rct_gst_get_configuration ()->uri == NULL,
        "cycle %d: terminate left a stale uri behind", i);
  }
}

GST_END_TEST;

GST_START_TEST (test_audio_level_refresh_rate_is_stored)
{
  gint *refresh_rate;

  rct_gst_set_audio_level_refresh_rate (250);
  refresh_rate = rct_gst_get_configuration ()->audioLevelRefreshRate;
  fail_unless_equals_int (*refresh_rate, 250);

  rct_gst_terminate ();
  refresh_rate = rct_gst_get_configuration ()->audioLevelRefreshRate;
  fail_unless_equals_int (*refresh_rate, 100);
}

GST_END_TEST;

static Suite *
backend_suite (void)
{
  Suite *s = suite_create ("backend");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (s, tc);
  tcase_add_unchecked_fixture (tc, tests_platform_setup, NULL);
  tcase_add_test (tc, test_configuration_is_a_process_singleton);
  tcase_add_test (tc, test_uri_is_copied_not_borrowed);
  tcase_add_test (tc, test_setting_the_same_uri_twice_is_idempotent);
  tcase_add_test (tc, test_terminate_clears_the_uri);
  tcase_add_test (tc, test_video_info_is_unavailable_without_a_stream);
  tcase_add_test (tc, test_get_info_reports_a_gstreamer_version);
  tcase_add_test (tc, test_init_builds_a_pipeline_and_reports_it);
  tcase_add_test (tc, test_init_ignores_its_configuration_argument);
  tcase_add_test (tc, test_repeated_init_and_terminate);
  tcase_add_test (tc, test_audio_level_refresh_rate_is_stored);

  return s;
}

GST_CHECK_MAIN (backend);
