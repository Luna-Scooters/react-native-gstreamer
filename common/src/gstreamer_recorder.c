//
//  gstreamer_recorder.c
//

#include "gstreamer_recorder.h"
#include "gstreamer_backend.h"
#include "gstreamer_h264_feed.h"
#include "gstreamer_video_writer.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

typedef struct {
    GstPad           *tee_pad;        // shared-h264 src pad (ghost) feeding the writer
    GstState          deferred_state; // pipeline state postponed until finalize completes
    RctGstVideoWriter video_writer;   // owns queue ! h264parse ! mp4mux ! filesink
} RctGstRecorder;

static RctGstRecorder recorder = {
    .deferred_state = GST_STATE_VOID_PENDING,
};

void rct_gst_recorder_reset(void)
{
    recorder.deferred_state = GST_STATE_VOID_PENDING;
    writer_reset(&recorder.video_writer);
    if (recorder.tee_pad) {
        rct_gst_h264_release_src_pad(recorder.tee_pad);
        recorder.tee_pad = NULL;
    }
}

// Writer finalize callback (main loop): the recording is over — drop our state,
// fire onRecordingFinished, then apply any deferred pipeline state. A failed
// recording ends the same way: fragmented MP4 keeps it playable up to the last
// fragment, and the app still needs to learn the recording stopped.
static void recorder_on_done(gchar *file_path, gboolean ok)
{
    (void)file_path;   // the app is notified via onRecordingFinished
    GstState deferred = recorder.deferred_state;

    rct_gst_recorder_reset();
    if (ok)
        g_print("Recording stopped and file finalized\n");
    else
        g_printerr("Recording failed, file kept up to its last fragment\n");

    RctGstConfiguration *cfg = rct_gst_get_configuration();
    if (cfg->onRecordingFinished)
        cfg->onRecordingFinished();

    // Recording is inactive now, so this cannot re-enter the defer path.
    if (deferred != GST_STATE_VOID_PENDING)
        rct_gst_set_pipeline_state(deferred);
}

void rct_gst_start_recording(const gchar *file_path)
{
    if (file_path == NULL || *file_path == '\0') {
        g_printerr("start_recording: no output path provided\n");
        return;
    }
    if (rct_gst_is_recording()) {
        g_printerr("start_recording: already recording\n");
        return;
    }
    if (!pipeline) {
        g_printerr("start_recording: pipeline not ready\n");
        return;
    }

    // Tap the shared h264 feed and hand it to the writer
    recorder.tee_pad = rct_gst_h264_request_src_pad();
    if (!recorder.tee_pad) {
        g_printerr("start_recording: h264 feed gave no src pad\n");
        return;
    }
    if (!writer_start(&recorder.video_writer, recorder.tee_pad, file_path, recorder_on_done)) {
        g_printerr("start_recording: writer failed to start\n");
        rct_gst_recorder_reset();
        return;
    }

    g_print("Recording started -> %s\n", file_path);
}

void rct_gst_stop_recording(void)
{
    if (!rct_gst_is_recording()) {
        g_print("stop_recording: not recording\n");
        return;
    }
    writer_close(&recorder.video_writer);
}

gboolean rct_gst_is_recording(void)
{
    return recorder.tee_pad != NULL;
}

void rct_gst_recorder_defer_state(GstState state)
{
    recorder.deferred_state = state;
    rct_gst_stop_recording();
}
