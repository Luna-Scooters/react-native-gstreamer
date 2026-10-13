// The MP4 writer branch (gstreamer_video_writer.c) on a live test stream: when
// it may hand a file over.

#include "support/backend.h"
#include "support/gst_test.h"

extern "C" {
#include "gstreamer_video_writer.h"
#include <glib/gstdio.h>

extern GstElement *pipeline;   // owned by gstreamer_backend.c
}

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

// What the writer hands back through on_done. on_done is a plain function
// pointer, so the fixture talks to it through this.
struct Done {
    bool fired = false;
    bool afterEos = false;   // filesink had finished EOS when on_done fired
    bool playable = false;   // checked inside on_done, i.e. when the app gets it
};
Done done;

// Set from the posting thread once filesink has handled EOS: basesink posts its
// EOS message only after the subclass flushed (the record bin forwards it).
std::atomic<bool> recsinkEos{false};

// Demux the whole file. An MP4 whose tail never reached the disk fails here.
bool isPlayable(const char *path)
{
    gchar *desc = g_strdup_printf("filesrc location=\"%s\" ! qtdemux ! h264parse ! fakesink", path);
    GstElement *check = gst_parse_launch(desc, nullptr);
    g_free(desc);
    if (!check)
        return false;
    gst_element_set_state(check, GST_STATE_PLAYING);
    GstBus *checkBus = gst_element_get_bus(check);
    GstMessage *msg = gst_bus_timed_pop_filtered(checkBus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    bool eos = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg)
        gst_message_unref(msg);
    gst_object_unref(checkBus);
    gst_element_set_state(check, GST_STATE_NULL);
    gst_object_unref(check);
    return eos;
}

void onDone(gchar *path)
{
    done.fired = true;
    done.afterEos = recsinkEos;
    done.playable = isPlayable(path);
}

// Holds filesink in the EOS event before it gets to flush its buffered tail,
// widening the window in which an early teardown would cut the file short.
GstPadProbeReturn slowEos(GstPad *, GstPadProbeInfo *info, gpointer)
{
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS)
        std::this_thread::sleep_for(300ms);
    return GST_PAD_PROBE_OK;
}

constexpr auto kHoldProbe = static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER);

// A block probe: what a caller uses to stop feeding the writer before closing.
GstPadProbeReturn hold(GstPad *, GstPadProbeInfo *, gpointer)
{
    return GST_PAD_PROBE_OK;
}

class VideoWriter : public testing::Test {
protected:
    // Encoded live test stream into a tee, with a live consumer standing in for
    // the camera view: it must keep getting frames whatever the writer does.
    void SetUp() override
    {
        done = Done{};
        recsinkEos = false;
        liveFrames = 0;
        pipelineErrors = 0;
        errors = 0;

        GError *error = nullptr;
        pipeline = gst_parse_launch(
            "videotestsrc is-live=true ! video/x-raw,width=320,height=240,framerate=30/1 ! "
            "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=10 ! "
            "h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! "
            "tee name=t allow-not-linked=true "
            "t. ! queue ! fakesink name=live sync=false signal-handoffs=true",
            &error);
        ASSERT_EQ(error, nullptr) << error->message;

        GstPtr<GstElement> live(gst_bin_get_by_name(GST_BIN(pipeline), "live"));
        g_signal_connect(live.get(), "handoff", G_CALLBACK(+[](GstElement *, GstBuffer *, GstPad *, gpointer self) {
            static_cast<VideoWriter *>(self)->liveFrames++;
        }), this);

        // Route bus messages the way the backend's bus watch does.
        GstPtr<GstBus> pipelineBus(gst_element_get_bus(pipeline));
        gst_bus_set_sync_handler(pipelineBus.get(), +[](GstBus *, GstMessage *msg, gpointer) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT &&
                gst_structure_has_name(gst_message_get_structure(msg), "GstBinForwarded")) {
                GstMessage *forwarded = nullptr;
                gst_structure_get(gst_message_get_structure(msg), "message", GST_TYPE_MESSAGE, &forwarded, nullptr);
                if (forwarded && GST_MESSAGE_TYPE(forwarded) == GST_MESSAGE_EOS &&
                    g_strcmp0(GST_OBJECT_NAME(GST_MESSAGE_SRC(forwarded)), "recsink") == 0)
                    recsinkEos = true;
                if (forwarded)
                    gst_message_unref(forwarded);
            }
            return GST_BUS_PASS;
        }, nullptr, nullptr);
        watchId = gst_bus_add_watch(pipelineBus.get(), +[](GstBus *, GstMessage *msg, gpointer self) -> gboolean {
            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR:
                    static_cast<VideoWriter *>(self)->errors++;
                    [[fallthrough]];
                case GST_MESSAGE_ELEMENT:
                    if (!writer_handle_message(msg) && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                        static_cast<VideoWriter *>(self)->pipelineErrors++;
                    break;
                default:
                    break;
            }
            return TRUE;
        }, this);

        GstPtr<GstElement> tee(gst_bin_get_by_name(GST_BIN(pipeline), "t"));
        teePad = gst_element_request_pad_simple(tee.get(), "src_%u");

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        ASSERT_TRUE(waitUntil([&] { return liveFrames > 0; }, 5s)) << "test stream never started";
    }

    void TearDown() override
    {
        writer_reset(&writer);
        if (pipeline)
            gst_element_set_state(pipeline, GST_STATE_NULL);
        if (teePad) {
            GstPtr<GstElement> tee(gst_bin_get_by_name(GST_BIN(pipeline), "t"));
            gst_element_release_request_pad(tee.get(), teePad);
            gst_object_unref(teePad);
        }
        g_source_remove(watchId);
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }

    // Services the default main context (bus watch, writer timers) meanwhile.
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

    // The live consumer is still getting frames.
    bool liveKeepsFlowing()
    {
        int before = liveFrames;
        return waitUntil([&] { return liveFrames > before + 5; }, 2s);
    }

    std::string tempPath(const char *name)
    {
        gchar *path = g_build_filename(g_get_tmp_dir(), name, nullptr);
        std::string result(path);
        g_free(path);
        g_unlink(result.c_str());
        return result;
    }

    RctGstVideoWriter writer{};
    GstPad *teePad = nullptr;
    guint watchId = 0;
    std::atomic<int> liveFrames{0};
    int pipelineErrors = 0;   // errors the writer didn't claim
    int errors = 0;           // every error, the writer's included
};

} // namespace

// on_done means the file is complete: it must not fire before filesink has
// flushed it (the old EOS-probe finalize tore the branch down mid-write).
TEST_F(VideoWriter, HandsTheFileOverOnlyOnceItIsWritten)
{
    std::string path = tempPath("rngst_writer_complete.mp4");
    ASSERT_TRUE(writer_start(&writer, teePad, path.c_str(), onDone));

    GstPtr<GstElement> recsink(gst_bin_get_by_name(GST_BIN(pipeline), "recsink"));
    ASSERT_TRUE(recsink);
    GstPtr<GstPad> recsinkPad(gst_element_get_static_pad(recsink.get(), "sink"));
    gst_pad_add_probe(recsinkPad.get(), GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, slowEos, nullptr, nullptr);

    waitUntil([] { return false; }, 1500ms);   // record a few fragments

    gulong holdId = gst_pad_add_probe(teePad, kHoldProbe, hold, nullptr, nullptr);
    writer_close(&writer);
    ASSERT_TRUE(waitUntil([] { return done.fired; }, 5s)) << "on_done never fired";
    gst_pad_remove_probe(teePad, holdId);

    EXPECT_TRUE(done.afterEos) << "the file was handed over before filesink had written it all";
    EXPECT_TRUE(done.playable);
    // Tearing down while filesink still flushes its tail fails that flush
    // ("Error while writing to file") and loses the final moov.
    EXPECT_EQ(errors, 0);
    EXPECT_TRUE(liveKeepsFlowing());
    g_unlink(path.c_str());
}
