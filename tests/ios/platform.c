#include "tests_platform.h"

#include "gst_ios_init.h"

// iOS has no generated gst_init_static_plugins(): the app registers plugins
// explicitly in gst_ios_init(), so the tests use that same entry point.
// gst_init() has already run inside gst_check_init() and is idempotent.
void
tests_platform_setup (void)
{
  static gboolean done = FALSE;

  if (done)
    return;
  done = TRUE;

  gst_ios_init ();
}
