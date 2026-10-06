#include <gtest/gtest.h>
#include <gst/gst.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    gst_init(&argc, &argv);

    return RUN_ALL_TESTS();
}
