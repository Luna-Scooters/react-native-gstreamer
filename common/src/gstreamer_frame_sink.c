//
//  gstreamer_frame_sink.c
//
//  See gstreamer_frame_sink.h.
//

#include "gstreamer_frame_sink.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

// Owned by gstreamer_backend.c
extern GstElement *pipeline;
extern GstElement *video_tee;

// A model's input, not a camera image.
#define MAX_SIDE 4096

// The branch on video-tee. The queue and the sink each keep one frame and drop
// the older one, so a slow reader - or none - never holds the tee back.
// async=false: a sink that has to preroll, added to a PLAYING pipeline, makes
// it lose its state (see display_up in gstreamer_display.c). caps=video/x-raw:
// system memory, which the converter can read.
#define FRAME_SINK_BRANCH                                                       \
    "queue name=frame-sink-queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream " \
    "! appsink name=frame-sink caps=video/x-raw max-buffers=1 drop=true "      \
    "sync=false async=false qos=false enable-last-sample=false"

// The thread that builds and destroys pipelines and the inference thread both
// change what follows.
static GMutex lock;
static guint width, height;            // asked for; 0 while frames aren't wanted
static GstElement *attached_pipeline;  // ref, from attach to detach
static GstElement *attached_tee;       // ref, likewise
static GstElement *branch;             // ref, while built
static GstPad *tee_pad;                // ref: video-tee's request pad feeding the branch
static GstAppSink *sink;               // ref, while built

// Kept for as long as the frames' format and the size asked for stay the same.
static GMutex convert_lock;
static GstVideoConverter *converter;
static GstVideoInfo converter_in, converter_out;

// Call with lock.
static void branch_remove(void)
{
    // The tee's pad first: once it is released the tee pushes nothing more
    // here, and a push still in flight counts as not-linked, not as an error.
    if (tee_pad) {
        gst_element_release_request_pad(attached_tee, tee_pad);
        gst_clear_object(&tee_pad);
    }
    if (branch) {
        // Down, and kept down whatever the pipeline does in the meantime.
        gst_element_set_locked_state(branch, TRUE);
        gst_element_set_state(branch, GST_STATE_NULL);
        g_autoptr(GstObject) parent = gst_element_get_parent(branch);
        if (parent)
            gst_bin_remove(GST_BIN(parent), branch);
        gst_clear_object(&branch);
    }
    gst_clear_object(&sink);
}

// Build the branch if frames are wanted and there is a pipeline to hang it on.
// FALSE if it should be there and isn't. Call with lock.
static int branch_build(void)
{
    if (branch || !attached_pipeline || !width)
        return 0;

    g_autoptr(GError) error = NULL;
    GstElement *bin = gst_parse_bin_from_description(FRAME_SINK_BRANCH, TRUE, &error);
    if (error) {
        g_printerr("frame sink: branch build failed: %s\n", error->message);
        // A failed parse can still hand back part of the bin.
        if (bin)
            gst_object_unref(bin);
        return -1;
    }
    g_autoptr(GstElement) app_sink = gst_bin_get_by_name(GST_BIN(bin), "frame-sink");

    gst_bin_add(GST_BIN(attached_pipeline), bin);   // sinks the floating ref
    branch = gst_object_ref(bin);

    // Up to the pipeline's state before linking: a push into a pad that isn't
    // active yet returns FLUSHING, which the tee passes upstream, and the
    // source stops for good.
    if (!gst_element_sync_state_with_parent(branch))
        g_printerr("frame sink: could not bring the branch to the pipeline's state\n");

    tee_pad = gst_element_request_pad_simple(attached_tee, "src_%u");
    g_autoptr(GstPad) bin_sink = gst_element_get_static_pad(branch, "sink");
    if (!tee_pad || !bin_sink || GST_PAD_LINK_FAILED(gst_pad_link(tee_pad, bin_sink))) {
        g_printerr("frame sink: failed to link video-tee -> frame sink\n");
        branch_remove();
        return -1;
    }

    sink = GST_APP_SINK(g_steal_pointer(&app_sink));
    g_print("frame sink: %ux%u RGBA frames for inference\n", width, height);
    return 0;
}

// Call with lock.
static void detach_locked(void)
{
    branch_remove();
    gst_clear_object(&attached_tee);
    gst_clear_object(&attached_pipeline);
}

void rct_gst_frame_sink_attach(void)
{
    g_mutex_lock(&lock);
    detach_locked();
    if (pipeline && video_tee) {   // the debug pipeline has no tee
        attached_pipeline = gst_object_ref(pipeline);
        attached_tee = gst_object_ref(video_tee);
        branch_build();
    }
    g_mutex_unlock(&lock);
}

void rct_gst_frame_sink_detach(void)
{
    g_mutex_lock(&lock);
    detach_locked();
    g_mutex_unlock(&lock);
}

int rct_gst_frame_sink_configure(uint32_t new_width, uint32_t new_height)
{
    if (new_width == 0 || new_height == 0 || new_width > MAX_SIDE || new_height > MAX_SIDE) {
        g_printerr("frame sink: %ux%u is not a size to infer at\n", new_width, new_height);
        return -1;
    }

    g_mutex_lock(&lock);
    width = new_width;
    height = new_height;
    // The branch doesn't depend on the size: one that is up stays up.
    int ret = branch_build();
    g_mutex_unlock(&lock);
    return ret;
}

void rct_gst_frame_sink_stop(void)
{
    g_mutex_lock(&lock);
    width = height = 0;
    branch_remove();
    g_mutex_unlock(&lock);

    g_mutex_lock(&convert_lock);
    g_clear_pointer(&converter, gst_video_converter_free);
    g_mutex_unlock(&convert_lock);
}

// Bilinear is plenty for a detector's input, and cheaper than the default cubic.
static GstStructure *converter_options(void)
{
    return gst_structure_new("GstVideoConverter",
        GST_VIDEO_CONVERTER_OPT_RESAMPLER_METHOD, GST_TYPE_VIDEO_RESAMPLER_METHOD,
        GST_VIDEO_RESAMPLER_METHOD_LINEAR,
        GST_VIDEO_CONVERTER_OPT_CHROMA_RESAMPLER_METHOD, GST_TYPE_VIDEO_RESAMPLER_METHOD,
        GST_VIDEO_RESAMPLER_METHOD_LINEAR,
        GST_VIDEO_CONVERTER_OPT_DITHER_METHOD, GST_TYPE_VIDEO_DITHER_METHOD,
        GST_VIDEO_DITHER_NONE,
        NULL);
}

// Scale and convert the sample's frame into `rgba`, stretched to fill it.
static gboolean convert(GstSample *sample, guint8 *rgba, gsize size, guint out_width, guint out_height)
{
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo in_info, out_info;
    if (!buffer || !caps || !gst_video_info_from_caps(&in_info, caps))
        return FALSE;
    if (!gst_video_info_set_format(&out_info, GST_VIDEO_FORMAT_RGBA, out_width, out_height))
        return FALSE;
    // The converter only changes format and size; it asserts the rest matches.
    GST_VIDEO_INFO_FPS_N(&out_info) = GST_VIDEO_INFO_FPS_N(&in_info);
    GST_VIDEO_INFO_FPS_D(&out_info) = GST_VIDEO_INFO_FPS_D(&in_info);
    GST_VIDEO_INFO_INTERLACE_MODE(&out_info) = GST_VIDEO_INFO_INTERLACE_MODE(&in_info);

    // Maps through the frame's video meta, so the decoder's strides are honoured.
    GstVideoFrame in_frame;
    if (!gst_video_frame_map(&in_frame, &in_info, buffer, GST_MAP_READ))
        return FALSE;

    // The caller's memory as the output frame, written in place.
    g_autoptr(GstBuffer) out = gst_buffer_new_wrapped_full(0, rgba, size, 0,
                                                           GST_VIDEO_INFO_SIZE(&out_info), NULL, NULL);
    GstVideoFrame out_frame;
    if (!gst_video_frame_map(&out_frame, &out_info, out, GST_MAP_WRITE)) {
        gst_video_frame_unmap(&in_frame);
        return FALSE;
    }

    g_mutex_lock(&convert_lock);
    if (!converter || !gst_video_info_is_equal(&in_info, &converter_in) ||
        !gst_video_info_is_equal(&out_info, &converter_out)) {
        g_clear_pointer(&converter, gst_video_converter_free);
        converter = gst_video_converter_new(&in_info, &out_info, converter_options());
        converter_in = in_info;
        converter_out = out_info;
        if (converter)
            g_print("frame sink: converting %s %dx%d to RGBA %ux%u\n",
                    gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&in_info)),
                    GST_VIDEO_INFO_WIDTH(&in_info), GST_VIDEO_INFO_HEIGHT(&in_info),
                    out_width, out_height);
        else
            g_printerr("frame sink: no converter from %s to RGBA\n",
                       gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&in_info)));
    }
    gboolean ok = converter != NULL;
    if (ok)
        gst_video_converter_frame(converter, &in_frame, &out_frame);
    g_mutex_unlock(&convert_lock);

    gst_video_frame_unmap(&out_frame);
    gst_video_frame_unmap(&in_frame);
    return ok;
}

int rct_gst_frame_sink_next(uint8_t *rgba, size_t size)
{
    g_mutex_lock(&lock);
    GstAppSink *from = sink ? gst_object_ref(sink) : NULL;
    guint out_width = width, out_height = height;
    g_mutex_unlock(&lock);
    if (!from)
        return -1;

    // Checked before pulling, so a buffer too small doesn't use the frame up.
    gsize needed = (gsize)out_width * out_height * 4;
    if (!rgba || size < needed) {
        g_printerr("frame sink: a %ux%u frame needs %" G_GSIZE_FORMAT " bytes, got %" G_GSIZE_FORMAT "\n",
                   out_width, out_height, needed, (gsize)size);
        gst_object_unref(from);
        return -1;
    }

    // Whatever the sink holds is newer than the last frame handed out: it lets
    // go of a frame as it is pulled.
    g_autoptr(GstSample) sample = gst_app_sink_try_pull_sample(from, 0);
    gst_object_unref(from);
    return sample && convert(sample, rgba, size, out_width, out_height) ? 0 : -1;
}
