//
//  gstreamer_frame_sink.c
//
//  See gstreamer_frame_sink.h.
//

#include "gstreamer_frame_sink.h"
#include "gstreamer_backend.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

// Owned by gstreamer_backend.c
extern GstElement *video_tee;

// A model's input, not a camera image.
#define MAX_SIDE 4096

// The thread that builds and destroys pipelines and the inference thread both
// change what follows.
static GMutex lock;
static GstVideoInfo out_info;  // RGBA at the size asked for; width 0 while frames aren't wanted
static gboolean attached;      // from attach to detach, on a pipeline with a video-tee
static GstAppSink *sink;       // ref, while built

// Built for the first frame converted, and kept until a new pipeline or stop:
// the frames' format doesn't change while a pipeline is up, and the size can't
// change before stop. One built for another size would write past the caller's
// buffer.
static GMutex convert_lock;
static GstVideoConverter *converter;
// Bilinear is plenty for a detector's input, and cheaper than the default cubic.
static const gchar CONVERTER_OPTIONS[] = "GstVideoConverter, "
    GST_VIDEO_CONVERTER_OPT_RESAMPLER_METHOD "=(GstVideoResamplerMethod)linear, "
    GST_VIDEO_CONVERTER_OPT_CHROMA_RESAMPLER_METHOD "=(GstVideoResamplerMethod)linear, "
    GST_VIDEO_CONVERTER_OPT_DITHER_METHOD "=(GstVideoDitherMethod)none";

static void converter_clear(void)
{
    g_mutex_lock(&convert_lock);
    g_clear_pointer(&converter, gst_video_converter_free);
    g_mutex_unlock(&convert_lock);
}

// Build the branch - one appsink on video-tee - if frames are wanted and there
// is a pipeline to hang it on. -1 if it should be there and isn't, otherwise 0.
// Call with lock.
static int branch_build(void)
{
    if (sink || !attached || !GST_VIDEO_INFO_WIDTH(&out_info))
        return 0;

    GstElement *element = gst_element_factory_make("appsink", "frame-sink");
    if (!element) {
        g_printerr("frame sink: no appsink element\n");
        return -1;
    }
    g_autoptr(GstCaps) caps = gst_caps_new_empty_simple("video/x-raw");
    g_object_set(element,
                 "caps", caps,
                 "max-buffers", 1u,
                 "drop", TRUE,
                 "sync", FALSE,
                 "async", FALSE,
                 "qos", FALSE,
                 "enable-last-sample", FALSE,
                 "wait-on-eos", FALSE,
                 NULL);

    sink = GST_APP_SINK(gst_object_ref_sink(element));
    if (!rct_gst_tap_video(element)) {
        g_printerr("frame sink: failed to attach to video-tee\n");
        gst_clear_object(&sink);
        return -1;
    }

    g_print("frame sink: %dx%d RGBA frames for inference\n",
            GST_VIDEO_INFO_WIDTH(&out_info), GST_VIDEO_INFO_HEIGHT(&out_info));
    return 0;
}

// Call with lock.
static void branch_remove(void)
{
    rct_gst_untap_video(GST_ELEMENT(sink));
    gst_clear_object(&sink);   // so branch_build makes the next one
}

// Call with lock.
static void detach_locked(void)
{
    branch_remove();
    attached = FALSE;
}

void rct_gst_frame_sink_attach(void)
{
    g_mutex_lock(&lock);
    detach_locked();
    attached = video_tee != NULL;   // the debug pipeline has no tee
    branch_build();
    g_mutex_unlock(&lock);
    converter_clear();   // the new pipeline's frames may differ
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
    // The converter is built for the first size and kept: another one would
    // have it write past the caller's buffer.
    guint out_width = GST_VIDEO_INFO_WIDTH(&out_info);
    guint out_height = GST_VIDEO_INFO_HEIGHT(&out_info);
    if (out_width && (new_width != out_width || new_height != out_height)) {
        g_printerr("frame sink: already asked for %ux%u; stop before asking for %ux%u\n",
                   out_width, out_height, new_width, new_height);
        g_mutex_unlock(&lock);
        return -1;
    }
    gst_video_info_set_format(&out_info, GST_VIDEO_FORMAT_RGBA, new_width, new_height);
    int ret = branch_build();
    g_mutex_unlock(&lock);
    return ret;
}

void rct_gst_frame_sink_stop(void)
{
    g_mutex_lock(&lock);
    gst_video_info_init(&out_info);   // width 0: frames no longer wanted
    branch_remove();
    g_mutex_unlock(&lock);
    converter_clear();
}

// Call with convert_lock.
static void create_converter(const GstVideoInfo *in_info, const GstVideoInfo *rgba_info)
{
    // The options only parse once their enum types are registered; a NULL
    // config would quietly give the defaults.
    g_type_ensure(GST_TYPE_VIDEO_RESAMPLER_METHOD);
    g_type_ensure(GST_TYPE_VIDEO_DITHER_METHOD);
    converter = gst_video_converter_new(in_info, rgba_info,
                                        gst_structure_from_string(CONVERTER_OPTIONS, NULL));
    if (converter)
        g_print("frame sink: converting %s %dx%d to RGBA %dx%d\n",
                gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(in_info)),
                GST_VIDEO_INFO_WIDTH(in_info), GST_VIDEO_INFO_HEIGHT(in_info),
                GST_VIDEO_INFO_WIDTH(rgba_info), GST_VIDEO_INFO_HEIGHT(rgba_info));
    else
        g_printerr("frame sink: no converter from %s to RGBA\n",
                   gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(in_info)));
}

// Scale and convert the frame in `buffer`, laid out as `in_info` says, into
// `rgba`, laid out as `rgba_info` says - the caller's copy, which gets the
// frame's rate and interlacing - stretched to fill it.
static gboolean convert(GstBuffer *buffer, const GstVideoInfo *in_info,
                        GstVideoInfo *rgba_info, guint8 *rgba, gsize size)
{
    // The converter only changes format and size; it asserts the rest matches.
    GST_VIDEO_INFO_FPS_N(rgba_info) = GST_VIDEO_INFO_FPS_N(in_info);
    GST_VIDEO_INFO_FPS_D(rgba_info) = GST_VIDEO_INFO_FPS_D(in_info);
    GST_VIDEO_INFO_INTERLACE_MODE(rgba_info) = GST_VIDEO_INFO_INTERLACE_MODE(in_info);

    // Maps through the frame's video meta, so the decoder's strides are honoured.
    GstVideoFrame in_frame;
    if (!gst_video_frame_map(&in_frame, in_info, buffer, GST_MAP_READ))
        return FALSE;

    // The caller's memory as the output frame, written in place.
    g_autoptr(GstBuffer) out = gst_buffer_new_wrapped_full(0, rgba, size, 0,
                                                           GST_VIDEO_INFO_SIZE(rgba_info), NULL, NULL);
    GstVideoFrame out_frame;
    if (!gst_video_frame_map(&out_frame, rgba_info, out, GST_MAP_WRITE)) {
        gst_video_frame_unmap(&in_frame);
        return FALSE;
    }

    g_mutex_lock(&convert_lock);
    if (!converter)
        create_converter(in_info, rgba_info);
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
    if (!rgba || size == 0) {
        g_printerr("frame sink: next called with no buffer\n");
        return -1;   
    }

    g_mutex_lock(&lock);
    g_autoptr(GstAppSink) sink_ref = sink ? gst_object_ref(sink) : NULL;
    // A copy: checked and converted at the same size, whatever stop and
    // configure do meanwhile.
    GstVideoInfo rgba_info = out_info;
    g_mutex_unlock(&lock);
    if (!sink_ref)
        return -1;

    // Checked before pulling, so a buffer too small doesn't use the frame up.
    gsize frame_size = GST_VIDEO_INFO_SIZE(&rgba_info);
    if (size < frame_size) {
        g_printerr("frame sink: this frame needs %" G_GSIZE_FORMAT " bytes, got %" G_GSIZE_FORMAT "\n",
                   frame_size, (gsize)size);
        return -1;
    }

    // Whatever the sink holds is newer than the last frame handed out: it lets
    // go of a frame as it is pulled.
    g_autoptr(GstSample) sample = gst_app_sink_try_pull_sample(sink_ref, 0);

    if (!sample) {
        g_printerr("frame sink: no frame available\n");
        return -1;
    }

    // Both borrowed from the sample, which outlives the conversion.
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo in_info;
    if (!buffer || !caps || !gst_video_info_from_caps(&in_info, caps))
        return -1;

    return convert(buffer, &in_info, &rgba_info, rgba, size) ? 0 : -1;
}
