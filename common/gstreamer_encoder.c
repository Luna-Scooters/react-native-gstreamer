//
//  gstreamer_encoder.c
//
//  See gstreamer_encoder.h.
//

#include "gstreamer_encoder.h"
#include "gstreamer_backend.h"
#include "gstreamer_codec.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;
extern GstElement *video_tee;

// File-size controls, shared by every consumer (ride + event recorder)
static const gint FALLBACK_HEIGHT = 360;
static const gint FALLBACK_WIDTH = 480;
static const gint FALLBACK_FPS = 10;
static const gint MIN_BITRATE_KBPS = 500;
static const gint MAX_BITRATE_KBPS = 4000;
static const gdouble BITS_PER_PIXEL = 0.1;
static const gint KEYFRAME_INTERVAL_SEC = 3;

static struct {
    GstElement *bin;      // queue ! videorate ! videoconvert ! enc ! h264parse ! tee(enc-tee)
    GstElement *enc_tee;  // borrowed (owned by bin) — consumers request src pads here
    GstPad     *tee_pad;  // video-tee src pad feeding the branch
    int         refcount;
} enc;

static void configure_h264_encoder()
{
    gint width = FALLBACK_WIDTH, height = FALLBACK_HEIGHT, fps = FALLBACK_FPS;
    if (!rct_gst_get_video_info(&width, &height, &fps))
        g_print("Failed to get video info, using fallback %dx%d @ %dfps\n", width, height, fps);

    GstElement *venc = gst_bin_get_by_name(GST_BIN(enc.bin), "venc");
    if (venc) {
        gdouble bits = (gdouble)width * height * fps * BITS_PER_PIXEL;
        gint bitrate_kbps = (gint)(bits / 1000.0);
        bitrate_kbps = CLAMP(bitrate_kbps, MIN_BITRATE_KBPS, MAX_BITRATE_KBPS);
        rct_gst_configure_h264_encoder(venc, fps, KEYFRAME_INTERVAL_SEC, bitrate_kbps);
        gst_object_unref(venc);
    }
}

// Give videorate/the encoder a valid PTS even when the camera delivers broken
// MJPEG frames (jpegparse flushes them with PTS=NONE). Repair only — consumers
// rebase their own output to zero. Runs on the streaming thread.
static GstPadProbeReturn enc_pts_repair_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == NULL || GST_BUFFER_PTS_IS_VALID(buffer))
        return GST_PAD_PROBE_OK;
    if (!pipeline)
        return GST_PAD_PROBE_OK;

    GstClock *clock = gst_element_get_clock(pipeline);
    if (!clock)
        return GST_PAD_PROBE_OK;
    GstClockTime now = gst_clock_get_time(clock);
    GstClockTime base = gst_element_get_base_time(pipeline);
    gst_object_unref(clock);
    if (now == GST_CLOCK_TIME_NONE || base == GST_CLOCK_TIME_NONE || now < base)
        return GST_PAD_PROBE_OK;

    buffer = gst_buffer_make_writable(buffer);
    GST_BUFFER_PTS(buffer) = now - base;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

GstElement *rct_gst_encoder_acquire(void)
{
    if (enc.bin) {
        enc.refcount++;
        return enc.enc_tee;
    }
    if (!video_tee || !pipeline) {
        g_printerr("encoder: no video-tee available (pipeline not ready)\n");
        return NULL;
    }

    gint w, h, fps = FALLBACK_FPS;
    if (rct_gst_get_video_info(&w, &h, &fps))
        g_print("Shared encoder using camera stream info: %dx%d @ %dfps\n",
                w, h, fps);

    gchar *selected_encoder = rct_gst_find_h264_encoder();
    g_print("Shared encoder: %s\n", selected_encoder);

    // No pinned raw format: videoconvert negotiates whatever the encoder accepts.
    GString *caps = g_string_new("video/x-raw");
    g_string_append_printf(caps, ",framerate=%d/1", fps);

    // allow-not-linked keeps the tee running through the brief windows where a
    // consumer branch is being added or removed.
    gchar *desc = g_strdup_printf(
        "queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 leaky=downstream ! "
        "videorate ! videoconvert ! %s ! "
        "%s name=venc ! h264parse config-interval=-1 ! tee name=enc-tee allow-not-linked=true",
        caps->str, selected_encoder);

    GError *error = NULL;
    enc.bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_string_free(caps, TRUE);
    g_free(desc);
    g_free(selected_encoder);
    if (!enc.bin) {
        g_printerr("encoder: branch build failed: %s\n", error ? error->message : "?");
        if (error) g_error_free(error);
        return NULL;
    }

    configure_h264_encoder();

    enc.enc_tee = gst_bin_get_by_name(GST_BIN(enc.bin), "enc-tee");
    if (enc.enc_tee)
        gst_object_unref(enc.enc_tee);  // borrowed; the bin owns it

    gst_bin_add(GST_BIN(pipeline), enc.bin);
    enc.tee_pad = gst_element_request_pad_simple(video_tee, "src_%u");
    gst_pad_add_probe(enc.tee_pad, GST_PAD_PROBE_TYPE_BUFFER, enc_pts_repair_probe, NULL, NULL);
    GstPad *bin_sink = gst_element_get_static_pad(enc.bin, "sink");
    if (gst_pad_link(enc.tee_pad, bin_sink) != GST_PAD_LINK_OK) {
        g_printerr("encoder: failed to link video-tee -> encoder\n");
        gst_object_unref(bin_sink);
        rct_gst_encoder_reset();
        return NULL;
    }
    gst_object_unref(bin_sink);
    gst_element_sync_state_with_parent(enc.bin);

    enc.refcount = 1;
    return enc.enc_tee;
}

void rct_gst_encoder_release(void)
{
    if (enc.refcount > 0)
        enc.refcount--;
    if (enc.refcount == 0)
        rct_gst_encoder_reset();
}

void rct_gst_encoder_reset(void)
{
    if (enc.bin) {
        gst_element_set_state(enc.bin, GST_STATE_NULL);
        GstObject *parent = gst_element_get_parent(enc.bin);
        if (parent) {
            gst_bin_remove(GST_BIN(parent), enc.bin);
            gst_object_unref(parent);
        } else {
            gst_object_unref(enc.bin);
        }
        enc.bin = NULL;
    }
    if (enc.tee_pad) {
        if (video_tee)
            gst_element_release_request_pad(video_tee, enc.tee_pad);
        gst_object_unref(enc.tee_pad);
        enc.tee_pad = NULL;
    }
    enc.enc_tee = NULL;
    enc.refcount = 0;
}
