#ifndef RCT_GST_TESTS_PLATFORM_H
#define RCT_GST_TESTS_PLATFORM_H

// Runs once per test case, after gst_init. On Android it registers the static
// plugins the suites need; on the host the shared libraries are found normally.
void tests_platform_setup (void);

#endif
