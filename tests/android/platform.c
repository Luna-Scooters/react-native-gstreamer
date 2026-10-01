#include "tests_platform.h"

// Nothing to do: gstreamer-1.0.mk generates gstreamer_android.c, whose
// gst_init_static_plugins() is called by gst_init() and registers every plugin
// in GSTREAMER_PLUGINS.
void
tests_platform_setup (void)
{
}
