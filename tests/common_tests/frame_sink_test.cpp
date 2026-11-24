// The frame sink (gstreamer_frame_sink.c): frames for on-device inference,
// taken off the same tee the camera view reads.

#include "support/backend.h"
#include "support/gst_test.h"

extern "C" {
#include <gst/app/gstappsrc.h>

extern GstElement *pipeline;    // owned by gstreamer_backend.c
extern GstElement *video_tee;
}

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// 16:9, so a frame letterboxed into a square would show black bands.
constexpr int kCameraWidth = 320;
constexpr int kCameraHeight = 180;

// Colors as the decoder hands them over: NV12, BT.601 limited range.
struct Yuv {
    guint8 y, u, v;
};
constexpr Yuv kRed{81, 90, 240};
constexpr Yuv kGreen{145, 54, 34};
constexpr Yuv kBlue{41, 240, 110};

struct Rgba {
    guint8 r, g, b, a;
};

// Loose enough not to depend on the converter's rounding.
bool isRed(Rgba p) { return p.r > 200 && p.g < 60 && p.b < 60 && p.a == 255; }
bool isGreen(Rgba p) { return p.r < 60 && p.g > 200 && p.b < 60 && p.a == 255; }
bool isBlue(Rgba p) { return p.r < 60 && p.g < 60 && p.b > 200 && p.a == 255; }

std::vector<guint8> frameOf(int width, int height)
{
    return std::vector<guint8>(static_cast<size_t>(width) * height * 4, 0);
}

Rgba pixel(const std::vector<guint8> &rgba, int width, int x, int y)
{
    const guint8 *p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
    return Rgba{p[0], p[1], p[2], p[3]};
}

bool everyPixel(const std::vector<guint8> &rgba, int width, int height, bool (*test)(Rgba))
{
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (!test(pixel(rgba, width, x, y)))
                return false;
    return true;
}

bool next(std::vector<guint8> &rgba)
{
    return rct_gst_frame_sink_next(rgba.data(), rgba.size()) == 0;
}

class FrameSink : public testing::Test {
protected:
    void SetUp() override { buildPipeline(); }

    void TearDown() override
    {
        rct_gst_frame_sink_stop();
        destroyPipeline();
    }

    // A stand-in for the backend's pipeline: frames pushed by hand into
    // video-tee, and a live consumer standing in for the camera view, which
    // must keep getting frames whatever the frame sink does.
    void buildPipeline()
    {
        liveFrames = 0;
        errors = 0;
        eos = 0;
        pushed = 0;

        GError *error = nullptr;
        pipeline = gst_parse_launch(
            "appsrc name=camera is-live=true format=time ! "
            "tee name=video-tee ! queue ! fakesink name=live sync=false signal-handoffs=true",
            &error);
        ASSERT_EQ(error, nullptr) << error->message;

        camera = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "camera"));
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, kCameraWidth,
            "height", G_TYPE_INT, kCameraHeight,
            "framerate", GST_TYPE_FRACTION, 30, 1,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
            "colorimetry", G_TYPE_STRING, "bt601",
            nullptr);
        gst_app_src_set_caps(camera, caps);
        gst_caps_unref(caps);

        // A ref, the way the backend holds it.
        video_tee = gst_bin_get_by_name(GST_BIN(pipeline), "video-tee");

        GstPtr<GstElement> live(gst_bin_get_by_name(GST_BIN(pipeline), "live"));
        g_signal_connect(live.get(), "handoff", G_CALLBACK(+[](GstElement *, GstBuffer *, GstPad *, gpointer self) {
            static_cast<FrameSink *>(self)->liveFrames++;
        }), this);

        GstPtr<GstBus> bus(gst_element_get_bus(pipeline));
        watchId = gst_bus_add_watch(bus.get(), +[](GstBus *, GstMessage *msg, gpointer self) -> gboolean {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                static_cast<FrameSink *>(self)->errors++;
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
                static_cast<FrameSink *>(self)->eos++;
            return TRUE;
        }, this);

        rct_gst_frame_sink_attach();   // as rct_gst_init does
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    }

    void destroyPipeline()
    {
        if (!pipeline)
            return;
        rct_gst_frame_sink_detach();   // as destroy_pipeline does
        gst_element_set_state(pipeline, GST_STATE_NULL);
        g_source_remove(watchId);
        gst_clear_object(&camera);
        gst_object_unref(pipeline);
        pipeline = nullptr;
        gst_clear_object(&video_tee);
    }

    GstFlowReturn push(Yuv color)
    {
        const gsize lumaSize = static_cast<gsize>(kCameraWidth) * kCameraHeight;
        const gsize size = lumaSize * 3 / 2;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_WRITE);
        std::memset(map.data, color.y, lumaSize);
        for (gsize i = lumaSize; i + 1 < size; i += 2) {
            map.data[i] = color.u;
            map.data[i + 1] = color.v;
        }
        gst_buffer_unmap(buffer, &map);

        int n = pushed++;
        GST_BUFFER_PTS(buffer) = n * 33 * GST_MSECOND;
        GST_BUFFER_DURATION(buffer) = 33 * GST_MSECOND;
        return gst_app_src_push_buffer(camera, buffer);
    }

    // Services the default main context (the bus watch) meanwhile.
    bool waitUntil(const std::function<bool()> &condition, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            while (g_main_context_iteration(nullptr, FALSE)) {}
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    bool waitForFrame(std::vector<guint8> &rgba) { return waitUntil([&] { return next(rgba); }, 2s); }

    // Long enough for a pushed frame to cross the live queue and land in the sink.
    void letFramesSettle() { waitUntil([] { return false; }, 200ms); }

    // The camera view still gets every frame pushed.
    bool liveKeepsFlowing()
    {
        int before = liveFrames;
        for (int i = 0; i < 5; i++)
            if (push(kRed) != GST_FLOW_OK)
                return false;
        return waitUntil([&] { return liveFrames >= before + 5; }, 2s);
    }

    int teeSrcPads()
    {
        return video_tee ? static_cast<int>(video_tee->numsrcpads) : 0;
    }

    GstAppSrc *camera = nullptr;
    guint watchId = 0;
    std::atomic<int> liveFrames{0};
    std::atomic<int> pushed{0};
    int errors = 0;
    int eos = 0;   // the pipeline's EOS: posted once every sink has it
};

} // namespace

TEST_F(FrameSink, HandsOutTheFrameAsRgbaAtTheSizeAskedFor)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(64, 48), 0);
    ASSERT_EQ(push(kRed), GST_FLOW_OK);

    auto rgba = frameOf(64, 48);
    ASSERT_TRUE(waitForFrame(rgba)) << "no frame came out";

    // Red where red was: RGBA byte order, not BGRA, with opaque alpha.
    EXPECT_TRUE(everyPixel(rgba, 64, 48, isRed))
        << "pixel (0,0) is " << int(pixel(rgba, 64, 0, 0).r) << "," << int(pixel(rgba, 64, 0, 0).g) << ","
        << int(pixel(rgba, 64, 0, 0).b) << "," << int(pixel(rgba, 64, 0, 0).a);
    EXPECT_EQ(errors, 0);
}

// The model's coordinates are normalized against the whole camera image, so the
// frame is stretched to the model's shape: a 16:9 camera into a square, no bands.
TEST_F(FrameSink, StretchesTheFrameRatherThanLetterboxingIt)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(32, 32), 0);
    ASSERT_EQ(push(kBlue), GST_FLOW_OK);

    auto rgba = frameOf(32, 32);
    ASSERT_TRUE(waitForFrame(rgba));

    EXPECT_TRUE(isBlue(pixel(rgba, 32, 16, 0))) << "the top row is not picture";
    EXPECT_TRUE(isBlue(pixel(rgba, 32, 16, 31))) << "the bottom row is not picture";
    EXPECT_TRUE(everyPixel(rgba, 32, 32, isBlue));
}

TEST_F(FrameSink, HandsEachFrameOutOnce)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(16, 16), 0);
    ASSERT_EQ(push(kRed), GST_FLOW_OK);

    auto rgba = frameOf(16, 16);
    ASSERT_TRUE(waitForFrame(rgba));

    std::memset(rgba.data(), 0x5a, rgba.size());
    EXPECT_FALSE(next(rgba)) << "the same frame was handed out twice";
    EXPECT_EQ(rgba[0], 0x5a) << "a call without a new frame wrote into the buffer";

    ASSERT_EQ(push(kGreen), GST_FLOW_OK);
    ASSERT_TRUE(waitForFrame(rgba)) << "the next frame never came";
    EXPECT_TRUE(everyPixel(rgba, 16, 16, isGreen));
}

// A reader slower than the camera gets the latest frame, not a backlog.
TEST_F(FrameSink, AReaderThatFallsBehindGetsTheNewestFrame)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(16, 16), 0);
    ASSERT_EQ(push(kRed), GST_FLOW_OK);
    ASSERT_EQ(push(kGreen), GST_FLOW_OK);
    ASSERT_EQ(push(kBlue), GST_FLOW_OK);
    ASSERT_TRUE(waitUntil([&] { return liveFrames >= 3; }, 2s));
    letFramesSettle();

    auto rgba = frameOf(16, 16);
    ASSERT_TRUE(next(rgba));
    EXPECT_TRUE(everyPixel(rgba, 16, 16, isBlue)) << "an older frame was handed out";
    EXPECT_FALSE(next(rgba)) << "older frames were queued up behind the newest";
}

TEST_F(FrameSink, ABufferTooSmallLeavesTheFrameForTheNextCall)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(16, 16), 0);
    ASSERT_EQ(push(kRed), GST_FLOW_OK);
    letFramesSettle();

    auto small = frameOf(16, 15);
    EXPECT_FALSE(next(small));

    auto rgba = frameOf(16, 16);
    EXPECT_TRUE(waitForFrame(rgba)) << "the frame was used up by the failed call";
}

// The sink runs on the tee's thread. With a frame left unpulled, an EOS must
// still go through it rather than wait for a reader, or the tee is stuck on it
// and the view's branch never finishes either.
TEST_F(FrameSink, EosGoesThroughAFrameNobodyPulled)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(16, 16), 0);
    ASSERT_EQ(push(kRed), GST_FLOW_OK);
    ASSERT_TRUE(waitUntil([&] { return liveFrames >= 1; }, 2s));
    letFramesSettle();

    ASSERT_EQ(gst_app_src_end_of_stream(camera), GST_FLOW_OK);
    EXPECT_TRUE(waitUntil([&] { return eos > 0; }, 2s)) << "EOS stuck in the frame sink";
    EXPECT_EQ(errors, 0);

    // The frame left behind is still there for the reader.
    auto rgba = frameOf(16, 16);
    EXPECT_TRUE(next(rgba));
    EXPECT_TRUE(everyPixel(rgba, 16, 16, isRed));
}

TEST_F(FrameSink, RejectsSizesNoModelTakes)
{
    EXPECT_EQ(rct_gst_frame_sink_configure(0, 16), -1);
    EXPECT_EQ(rct_gst_frame_sink_configure(16, 0), -1);
    EXPECT_EQ(rct_gst_frame_sink_configure(5000, 16), -1);
    EXPECT_EQ(teeSrcPads(), 1) << "a rejected size still built the branch";
}

TEST_F(FrameSink, NothingIsHandedOutUntilFramesAreAskedFor)
{
    ASSERT_EQ(push(kRed), GST_FLOW_OK);
    letFramesSettle();

    auto rgba = frameOf(16, 16);
    EXPECT_FALSE(next(rgba));
    EXPECT_EQ(teeSrcPads(), 1) << "the branch is up without anyone asking for frames";
}

// The size is fixed until stop: the converter is built for it once.
TEST_F(FrameSink, TheSizeIsFixedUntilStop)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(16, 16), 0);
    ASSERT_EQ(push(kRed), GST_FLOW_OK);
    auto first = frameOf(16, 16);
    ASSERT_TRUE(waitForFrame(first));

    EXPECT_EQ(rct_gst_frame_sink_configure(40, 24), -1) << "another size was taken before stop";
    EXPECT_EQ(rct_gst_frame_sink_configure(16, 16), 0) << "asking again for the same size failed";
    EXPECT_EQ(teeSrcPads(), 2) << "asking again built a second branch";
    ASSERT_EQ(push(kGreen), GST_FLOW_OK);
    auto same = frameOf(16, 16);
    ASSERT_TRUE(waitForFrame(same));
    EXPECT_TRUE(everyPixel(same, 16, 16, isGreen));

    rct_gst_frame_sink_stop();
    ASSERT_EQ(rct_gst_frame_sink_configure(40, 24), 0) << "a new size after stop failed";
    ASSERT_EQ(push(kGreen), GST_FLOW_OK);
    auto resized = frameOf(40, 24);
    ASSERT_TRUE(waitForFrame(resized));
    EXPECT_TRUE(everyPixel(resized, 40, 24, isGreen));
}

TEST_F(FrameSink, StopTakesTheBranchOffTheTee)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(16, 16), 0);
    EXPECT_EQ(teeSrcPads(), 2);

    rct_gst_frame_sink_stop();

    EXPECT_EQ(teeSrcPads(), 1) << "the branch's tee pad was not released";
    GstPtr<GstElement> branchSink(gst_bin_get_by_name(GST_BIN(pipeline), "frame-sink"));
    EXPECT_FALSE(branchSink) << "the branch is still in the pipeline";

    ASSERT_EQ(push(kRed), GST_FLOW_OK);
    letFramesSettle();
    auto rgba = frameOf(16, 16);
    EXPECT_FALSE(next(rgba)) << "a frame came out after stop";
    EXPECT_TRUE(liveKeepsFlowing());
}

// The inference engine may ask before the camera screen opens, and keeps
// running across visits: each new pipeline gets the branch back.
TEST_F(FrameSink, TheSizeAskedForOutlivesThePipeline)
{
    destroyPipeline();
    ASSERT_EQ(rct_gst_frame_sink_configure(24, 24), 0) << "asking with no pipeline failed";

    for (int visit = 0; visit < 3; visit++) {
        buildPipeline();
        ASSERT_EQ(push(kGreen), GST_FLOW_OK);
        auto rgba = frameOf(24, 24);
        ASSERT_TRUE(waitForFrame(rgba)) << "visit " << visit << ": no frame";
        EXPECT_TRUE(everyPixel(rgba, 24, 24, isGreen));
        destroyPipeline();
    }
}

// The frame sink comes and goes on a streaming tee; the camera view must not
// notice: no error, no stall, every frame still reaches it.
TEST_F(FrameSink, StartingAndStoppingWhileStreamingLeavesTheViewAlone)
{
    std::atomic<bool> streaming{true};
    std::atomic<int> failedPushes{0};
    std::thread cameraThread([&] {
        while (streaming) {
            if (push(kRed) != GST_FLOW_OK)
                failedPushes++;
            std::this_thread::sleep_for(2ms);
        }
    });

    auto rgba = frameOf(16, 16);
    int framesOut = 0;
    for (int i = 0; i < 50; i++) {
        if (rct_gst_frame_sink_configure(16, 16) != 0) {
            ADD_FAILURE() << "start " << i << " failed";
            break;   // not ASSERT: the camera thread has to be joined
        }
        waitUntil([] { return false; }, 10ms);
        framesOut += next(rgba) ? 1 : 0;
        rct_gst_frame_sink_stop();
    }
    streaming = false;
    cameraThread.join();

    EXPECT_GT(framesOut, 0) << "no frame came out in 50 starts";
    EXPECT_EQ(failedPushes, 0);
    EXPECT_EQ(errors, 0);
    EXPECT_EQ(teeSrcPads(), 1);
    EXPECT_TRUE(liveKeepsFlowing()) << "the camera view stalled";
}

// A subclass of `parent` registered as element `name`, standing in for an
// element this GStreamer doesn't have.
void registerStandIn(const char *name, const char *parent)
{
    if (GstPtr<GstElementFactory>(gst_element_factory_find(name)))
        return;
    GstPtr<GstElementFactory> parentFactory(gst_element_factory_find(parent));
    ASSERT_TRUE(parentFactory) << "no " << parent;
    GstPtr<GstPluginFeature> loaded(gst_plugin_feature_load(GST_PLUGIN_FEATURE(parentFactory.get())));
    GType parentType = gst_element_factory_get_element_type(GST_ELEMENT_FACTORY(loaded.get()));

    GTypeQuery query;
    g_type_query(parentType, &query);
    gchar *typeName = g_strdup_printf("StandIn_%s", name);
    GType type = g_type_register_static_simple(parentType, typeName, query.class_size,
        +[](gpointer klass, gpointer) {
            gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "stand-in", "Generic",
                                                  "stands in for a missing element", "tests");
        },
        query.instance_size, nullptr, static_cast<GTypeFlags>(0));
    g_free(typeName);
    ASSERT_TRUE(gst_element_register(nullptr, name, GST_RANK_NONE, type));
}

// With the backend's own pipelines.
class FrameSinkBackend : public testing::Test {
protected:
    // The camera pipeline's display branch is Vulkan, which a host GStreamer
    // may not have; nothing here needs it to draw.
    static void SetUpTestSuite()
    {
        registerStandIn("vulkanupload", "identity");
        registerStandIn("vulkancolorconvert", "identity");
        registerStandIn("vulkansink", "fakesink");
    }

    void TearDown() override
    {
        rct_gst_frame_sink_stop();
        rct_gst_terminate();
    }

    bool hasBranch()
    {
        GstPtr<GstElement> branchSink(pipeline ? gst_bin_get_by_name(GST_BIN(pipeline), "frame-sink") : nullptr);
        return branchSink != nullptr;
    }
};

TEST_F(FrameSinkBackend, TheCameraPipelineGetsTheBranchAndTerminateTakesItDown)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(64, 64), 0);
    rct_gst_get_configuration()->isDebugging = FALSE;

    rct_gst_init(nullptr);
    ASSERT_NE(pipeline, nullptr) << "the camera pipeline wasn't built";
    EXPECT_TRUE(hasBranch()) << "rct_gst_init didn't build the branch";

    rct_gst_terminate();
    EXPECT_EQ(pipeline, nullptr);
    auto rgba = frameOf(64, 64);
    EXPECT_FALSE(next(rgba));

    // And the next camera screen gets it back.
    rct_gst_get_configuration()->isDebugging = FALSE;
    rct_gst_init(nullptr);
    ASSERT_NE(pipeline, nullptr);
    EXPECT_TRUE(hasBranch()) << "a second rct_gst_init didn't build the branch";
}

TEST_F(FrameSinkBackend, AskingOnceTheCameraPipelineExistsBuildsTheBranchThen)
{
    rct_gst_get_configuration()->isDebugging = FALSE;
    rct_gst_init(nullptr);
    ASSERT_NE(pipeline, nullptr);
    EXPECT_FALSE(hasBranch());

    ASSERT_EQ(rct_gst_frame_sink_configure(64, 64), 0);
    EXPECT_TRUE(hasBranch());

    rct_gst_frame_sink_stop();
    EXPECT_FALSE(hasBranch());
}

// The debug pipeline has no video-tee: nothing to hang the branch on, and
// nothing breaks.
TEST_F(FrameSinkBackend, TheDebugPipelineHasNoFramesAndNoTrouble)
{
    ASSERT_EQ(rct_gst_frame_sink_configure(64, 64), 0);
    rct_gst_get_configuration()->isDebugging = TRUE;

    rct_gst_init(nullptr);
    ASSERT_NE(pipeline, nullptr);

    auto rgba = frameOf(64, 64);
    EXPECT_FALSE(next(rgba));
}
