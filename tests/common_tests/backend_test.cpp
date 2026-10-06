// The backend's configuration and lifecycle (gstreamer_backend.c), without a
// camera.

#include "support/backend.h"
#include "support/gst_test.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

constexpr const char *kCameraUri = "rtsp://192.168.0.1:554/livestream/1";

bool initFired;

// The backend takes the uri as a mutable gchar *, which it copies.
gchar *uriArgument(const char *uri)
{
    return const_cast<gchar *>(uri);
}

class Backend : public testing::Test {
protected:
    void TearDown() override { rct_gst_terminate(); }

    RctGstConfiguration *setupConfiguration()
    {
        RctGstConfiguration *config = rct_gst_get_configuration();

        initFired = false;

        config->isDebugging = TRUE;
        config->onInit = [] { initFired = true; };

        return config;
    }
};

} // namespace

TEST_F(Backend, ConfigurationIsAProcessSingleton)
{
    RctGstConfiguration *config = rct_gst_get_configuration();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config, rct_gst_get_configuration()) << "configuration is not a singleton";
    ASSERT_NE(config->audioLevelRefreshRate, nullptr);
    EXPECT_EQ(*config->audioLevelRefreshRate, 100);
}

// Callers hand over transient buffers - JNI GetStringUTFChars, -[NSString
// UTF8String] - that die as soon as they return.
TEST_F(Backend, UriIsCopiedNotBorrowed)
{
    gchar *transient = g_strdup(kCameraUri);

    rct_gst_set_uri(transient);

    std::memset(transient, 'x', std::strlen(transient));
    g_free(transient);

    EXPECT_STREQ(rct_gst_get_configuration()->uri, kCameraUri);
}

TEST_F(Backend, SettingTheSameUriTwiceIsIdempotent)
{
    rct_gst_set_uri(uriArgument(kCameraUri));
    rct_gst_set_uri(uriArgument(kCameraUri));

    EXPECT_STREQ(rct_gst_get_configuration()->uri, kCameraUri);
}

TEST_F(Backend, TerminateClearsTheUri)
{
    rct_gst_set_uri(uriArgument(kCameraUri));
    rct_gst_terminate();

    EXPECT_EQ(rct_gst_get_configuration()->uri, nullptr) << "terminate left a stale uri behind";
}

TEST_F(Backend, VideoInfoIsUnavailableWithoutAStream)
{
    gint width = -1, height = -1, fps = -1;

    EXPECT_FALSE(rct_gst_get_video_info(&width, &height, &fps))
        << "video info was reported with no pipeline running";
}

TEST_F(Backend, GetInfoReportsAGStreamerVersion)
{
    std::string info(rct_gst_get_info());

    ASSERT_FALSE(info.empty());
    EXPECT_TRUE(g_str_has_prefix(info.c_str(), "GStreamer")) << "unexpected info: " << info;
}

TEST_F(Backend, InitBuildsAPipelineAndReportsIt)
{
    setupConfiguration();

    rct_gst_init(nullptr);

    EXPECT_TRUE(initFired) << "onInit never fired";
}

// rct_gst_init() ignores its argument entirely: it reads the singleton from
// rct_gst_get_configuration(), and the parameter shadows the global of the same
// name. Passing a different struct has no effect, so pin that contract.
TEST_F(Backend, InitIgnoresItsConfigurationArgument)
{
    RctGstConfiguration decoy = {};

    setupConfiguration();
    decoy.isDebugging = FALSE;
    decoy.onInit = nullptr;

    rct_gst_init(&decoy);

    EXPECT_TRUE(initFired) << "init used the passed-in struct instead of the singleton";
}

TEST_F(Backend, RepeatedInitAndTerminate)
{
    for (int i = 0; i < 10; i++) {
        setupConfiguration();
        rct_gst_set_uri(uriArgument(kCameraUri));

        rct_gst_init(nullptr);
        EXPECT_TRUE(initFired) << "cycle " << i << ": onInit never fired";

        rct_gst_terminate();
        EXPECT_EQ(rct_gst_get_configuration()->uri, nullptr)
            << "cycle " << i << ": terminate left a stale uri behind";
    }
}
