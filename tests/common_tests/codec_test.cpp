// Runtime codec selection (gstreamer_codec.c): what it picks has to be
// registered, parse where the backend splices it in, and be configured in the
// units each encoder expects.

#include "support/backend.h"
#include "support/gst_test.h"

#include <gtest/gtest.h>

#include <iostream>

namespace {

testing::AssertionResult IsRegistered(const char *name)
{
    GstPtr<GstElementFactory> factory(gst_element_factory_find(name));
    if (!factory)
        return testing::AssertionFailure() << "selected element \"" << name << "\" is not registered";
    return testing::AssertionSuccess();
}

} // namespace

TEST(Codec, H264DecoderSelectionIsRegistered)
{
    std::string name(rct_gst_find_h264_decoder());

    ASSERT_FALSE(name.empty());
    std::cout << "selected H264 decoder: " << name << "\n";
    EXPECT_TRUE(IsRegistered(name.c_str()));
}

TEST(Codec, JpegDecoderSelectionIsRegistered)
{
    std::string name(rct_gst_find_jpeg_decoder());

    ASSERT_FALSE(name.empty());
    std::cout << "selected JPEG decoder: " << name << "\n";
    EXPECT_TRUE(IsRegistered(name.c_str()));
}

TEST(Codec, H264EncoderSelectionIsRegistered)
{
    std::string name(rct_gst_find_h264_encoder());

    ASSERT_FALSE(name.empty());
    std::cout << "selected H264 encoder: " << name << "\n";
    EXPECT_TRUE(IsRegistered(name.c_str()));
    EXPECT_FALSE(g_strstr_len(name.c_str(), -1, "mtk"))
        << "selection returned the broken MediaTek encoder: " << name;
}

// The selected name is interpolated into the parse-launch string that
// rct_gst_link_decode_chain() builds, so it has to parse in place.
TEST(Codec, SelectedDecodersParseInTheDecodeChain)
{
    std::string h264(rct_gst_find_h264_decoder());
    std::string jpeg(rct_gst_find_jpeg_decoder());
    const std::string chains[] = {
        std::string("rtph264depay ! h264parse ! ") + h264,
        std::string("rtpjpegdepay name=rtpjpegdepay0 ! jpegparse ! ") + jpeg,
    };

    for (const auto &chain : chains) {
        GError *error = nullptr;
        GstPtr<GstElement> bin(gst_parse_bin_from_description(chain.c_str(), TRUE, &error));

        EXPECT_TRUE(bin && !error) << "decode chain did not parse: " << chain << " ("
                                   << (error ? error->message : "no error set") << ")";
        g_clear_error(&error);
    }
}

// Every encoder spells the bitrate unit and the GOP length differently, and
// getting either wrong is silent rather than an error.
TEST(Codec, EncoderConfigurationUnits)
{
    const int fps = 10;
    const int keyframeIntervalSec = 2;
    const int bitrateKbps = 2000;
    const int expectedFrames = keyframeIntervalSec * fps;

    std::string name(rct_gst_find_h264_encoder());
    ASSERT_FALSE(name.empty());
    GstPtr<GstElement> encoder(gst_element_factory_make(name.c_str(), "enc"));
    ASSERT_NE(encoder, nullptr) << "could not instantiate encoder " << name;

    rct_gst_configure_h264_encoder(encoder.get(), fps, keyframeIntervalSec, bitrateKbps);

    GObjectClass *klass = G_OBJECT_GET_CLASS(encoder.get());

    if (g_object_class_find_property(klass, "bitrate")) {
        const bool bitsPerSecond = g_str_has_prefix(name.c_str(), "openh264") ||
                                   g_str_has_prefix(name.c_str(), "amcvidenc");
        guint actual = 0;

        g_object_get(encoder.get(), "bitrate", &actual, nullptr);
        EXPECT_EQ(actual, bitsPerSecond ? guint(bitrateKbps) * 1000 : guint(bitrateKbps));
    }

    const struct {
        const char *property;
        int expected;
    } gop[] = {
        { "key-int-max", expectedFrames },            // x264enc: frames
        { "gop-size", expectedFrames },               // openh264enc: frames
        { "max-keyframe-interval", expectedFrames },  // vtenc_h264: frames
        { "i-frame-interval", keyframeIntervalSec },  // amcvidenc: seconds
    };
    bool sawGopProperty = false;

    for (const auto &[property, expected] : gop) {
        if (!g_object_class_find_property(klass, property))
            continue;

        gint actual = 0;
        g_object_get(encoder.get(), property, &actual, nullptr);
        sawGopProperty = true;
        EXPECT_EQ(actual, expected) << property;
    }

    if (!sawGopProperty)
        std::cout << name << " exposes none of the known GOP properties\n";
}

TEST(Codec, EncoderConfigurationRejectsNonsense)
{
    std::string name(rct_gst_find_h264_encoder());
    ASSERT_FALSE(name.empty());
    GstPtr<GstElement> encoder(gst_element_factory_make(name.c_str(), "enc"));
    ASSERT_NE(encoder, nullptr);

    const bool hasBitrate = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder.get()), "bitrate");
    guint before = 0, after = 0;

    if (hasBitrate)
        g_object_get(encoder.get(), "bitrate", &before, nullptr);

    rct_gst_configure_h264_encoder(encoder.get(), 0, 2, 2000);
    rct_gst_configure_h264_encoder(nullptr, 10, 2, 2000);

    if (hasBitrate) {
        g_object_get(encoder.get(), "bitrate", &after, nullptr);
        EXPECT_EQ(after, before);
    }
}

// find_best_element() asks for `type | GST_ELEMENT_FACTORY_TYPE_HARDWARE` first,
// intending to prefer hardware. That does not work: the category bits are OR-ed
// by gst_element_factory_list_is_type, so adding HARDWARE widens the query
// instead of restricting it. Both tiers return the same list and selection
// reduces to plain rank order. Pinned here because the consequence is silent -
// on Android amcviddec* rank SECONDARY while avdec_h264 ranks PRIMARY, so the
// software decoder wins even though hardware was asked for first.
TEST(Codec, HardwareFactoryTypeDoesNotFilter)
{
    GList *hardwareOnly = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO |
            GST_ELEMENT_FACTORY_TYPE_HARDWARE,
        GST_RANK_MARGINAL);
    const char *softwareDecoder = nullptr;

    for (GList *l = hardwareOnly; l && !softwareDecoder; l = l->next) {
        auto *factory = GST_ELEMENT_FACTORY(l->data);
        const char *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);

        if (klass && !g_strstr_len(klass, -1, "Hardware"))
            softwareDecoder = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    }

    if (softwareDecoder)
        std::cout << "non-hardware factory returned by a HARDWARE query: " << softwareDecoder << "\n";
    gst_plugin_feature_list_free(hardwareOnly);

    EXPECT_TRUE(softwareDecoder)
        << "GST_ELEMENT_FACTORY_TYPE_HARDWARE now filters by klass; find_best_element()'s two "
           "tiers are no longer equivalent and its hardware preference should be re-checked";
}
