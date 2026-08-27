//
//  gstreamer_encoder.c
//
//  See gstreamer_encoder.h.
//

#include "gstreamer_encoder.h"
#include "gstreamer_backend.h"
#include "gstreamer_codec.h"
#include <gst/video/video.h>

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
static const GstClockTime ENC_STATE_TIMEOUT = 500 * GST_MSECOND;

typedef struct {
    GstElement *bin;      // queue ! videorate ! videoconvert ! enc ! h264parse ! tee(enc-tee)
    GstElement *enc_tee;  // borrowed (owned by bin) — internal tap point
    GstPad     *tee_pad;  // video-tee src pad feeding the branch
    int         refcount;
} RctGstEncoder;

RctGstEncoder enc = { NULL, NULL, NULL, 0 };

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

// Anchor for enc_out_fixup_probe below: the timestamp of the encoder's very first
// input frame, whether the camera supplied it or the repair below did.
static GstClockTime enc_first_in_pts = GST_CLOCK_TIME_NONE;
static gboolean     enc_out_checked  = FALSE;

// Give videorate/the encoder a valid PTS even when the camera delivers broken
// MJPEG frames (jpegparse flushes them with PTS=NONE). Repair only — consumers
// rebase their own output to zero. Runs on the streaming thread.
static GstPadProbeReturn enc_pts_repair_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == NULL)
        return GST_PAD_PROBE_OK;
    if (GST_BUFFER_PTS_IS_VALID(buffer)) {
        if (!GST_CLOCK_TIME_IS_VALID(enc_first_in_pts))
            enc_first_in_pts = GST_BUFFER_PTS(buffer);
        return GST_PAD_PROBE_OK;
    }
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
    if (!GST_CLOCK_TIME_IS_VALID(enc_first_in_pts))
        enc_first_in_pts = now - base;
    return GST_PAD_PROBE_OK;
}

// Repair the encoder's first OUTPUT buffer.
//
// amcvidenc (Exynos) returns its first encoded buffer stamped pts=0 with no DTS,
// no matter where the input timeline actually starts.

static GstPadProbeReturn enc_out_fixup_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad; (void)user_data;
    if (enc_out_checked)
        return GST_PAD_PROBE_OK;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf || !GST_CLOCK_TIME_IS_VALID(enc_first_in_pts))
        return GST_PAD_PROBE_OK;   // nothing to anchor against yet
    enc_out_checked = TRUE;

    // The encoder cannot legitimately emit a frame from before its first input.
    gboolean bogus = !GST_BUFFER_PTS_IS_VALID(buf) ||
                     GST_BUFFER_PTS(buf) + GST_SECOND < enc_first_in_pts;
    if (!bogus)
        return GST_PAD_PROBE_OK;

    g_print("encoder: re-stamped first output buffer (pts was %.3f, first input %.3f)\n",
            GST_BUFFER_PTS_IS_VALID(buf) ? (gdouble)GST_BUFFER_PTS(buf) / GST_SECOND : -1.0,
            (gdouble)enc_first_in_pts / GST_SECOND);

    buf = gst_buffer_make_writable(buf);
    GST_BUFFER_PTS(buf) = enc_first_in_pts;
    if (!GST_BUFFER_DTS_IS_VALID(buf))
        GST_BUFFER_DTS(buf) = enc_first_in_pts;
    GST_PAD_PROBE_INFO_DATA(info) = buf;
    return GST_PAD_PROBE_OK;
}

static gboolean encoder_build(void)
{
    if (enc.bin)
        return TRUE;
    if (!video_tee || !pipeline) {
        g_printerr("encoder: no video-tee available (pipeline not ready)\n");
        return FALSE;
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
        "videorate skip-to-first=true ! videoconvert ! %s ! "
        "%s name=venc ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=avc,alignment=au ! tee name=enc-tee allow-not-linked=true",
        caps->str, selected_encoder);

    GError *error = NULL;
    enc.bin = gst_parse_bin_from_description(desc, TRUE, &error);
    g_string_free(caps, TRUE);
    g_free(desc);
    g_free(selected_encoder);
    if (!enc.bin) {
        g_printerr("encoder: branch build failed: %s\n", error ? error->message : "?");
        if (error) g_error_free(error);
        return FALSE;
    }

    configure_h264_encoder();

    enc.enc_tee = gst_bin_get_by_name(GST_BIN(enc.bin), "enc-tee");
    if (enc.enc_tee)
        gst_object_unref(enc.enc_tee);  // borrowed; the bin owns it

    // Watch the encoder's OUTPUT: amcvidenc stamps its first buffer 0 (see
    // enc_out_fixup_probe). enc-tee's sink is the single point every consumer's
    // frames pass through.
    if (enc.enc_tee) {
        GstPad *tee_sink = gst_element_get_static_pad(enc.enc_tee, "sink");
        if (tee_sink) {
            gst_pad_add_probe(tee_sink, GST_PAD_PROBE_TYPE_BUFFER,
                              enc_out_fixup_probe, NULL, NULL);
            gst_object_unref(tee_sink);
        }
    }

    gst_bin_add(GST_BIN(pipeline), enc.bin);

    // Reach PLAYING BEFORE linking, pushing into a pad that is not active yet returns 
    // GST_FLOW_FLUSHING, which the tee propagates upstream and the source task pauses for good
    gst_element_sync_state_with_parent(enc.bin);
    gst_element_get_state(enc.bin, NULL, NULL, ENC_STATE_TIMEOUT);
    enc.tee_pad = gst_element_request_pad_simple(video_tee, "src_%u");
    gst_pad_add_probe(enc.tee_pad, GST_PAD_PROBE_TYPE_BUFFER, enc_pts_repair_probe, NULL, NULL);
    GstPad *bin_sink = gst_element_get_static_pad(enc.bin, "sink");
    if (gst_pad_link(enc.tee_pad, bin_sink) != GST_PAD_LINK_OK) {
        g_printerr("encoder: failed to link video-tee -> encoder\n");
        gst_object_unref(bin_sink);
        rct_gst_encoder_reset();
        return FALSE;
    }
    gst_object_unref(bin_sink);
    return TRUE;
}

GstPad *rct_gst_encoder_request_src_pad(void)
{
    if (!encoder_build())
        return NULL;
    GstPad *tee_src = gst_element_request_pad_simple(enc.enc_tee, "src_%u");
    if (!tee_src) {
        g_printerr("encoder: enc-tee gave no src pad\n");
        return NULL;
    }
    GstPad *ghost = gst_ghost_pad_new(NULL, tee_src);
    if (!ghost) {
        gst_element_release_request_pad(enc.enc_tee, tee_src);
        gst_object_unref(tee_src);
        return NULL;
    }
    gst_object_unref(tee_src);            // the ghost references the target now
    gst_pad_set_active(ghost, TRUE);
    gst_element_add_pad(enc.bin, ghost);  // sinks the floating ref; the bin owns it
    enc.refcount++;
    return gst_object_ref(ghost);         // hand the caller its own ref
}

void rct_gst_encoder_release_src_pad(GstPad *pad)
{
    if (!pad)
        return;
    GstPad *tee_src = NULL;
    if (GST_IS_GHOST_PAD(pad))
        tee_src = gst_ghost_pad_get_target(GST_GHOST_PAD(pad));
    gst_pad_set_active(pad, FALSE);
    if (enc.bin)
        gst_element_remove_pad(enc.bin, pad);  // drops the bin's ref
    if (tee_src) {
        if (enc.enc_tee)
            gst_element_release_request_pad(enc.enc_tee, tee_src);
        gst_object_unref(tee_src);
    }
    gst_object_unref(pad);  // drops the caller's ref from request_src_pad

    // The src pads ARE the encoder's refcount: tear it down with the last one.
    if (enc.refcount > 0)
        enc.refcount--;
    if (enc.refcount == 0)
        rct_gst_encoder_reset();
}

void rct_gst_encoder_force_keyframe(void)
{
    if (!enc.enc_tee)
        return;
    GstPad *sink = gst_element_get_static_pad(enc.enc_tee, "sink");
    if (!sink)
        return;
    GstPad *peer = gst_pad_get_peer(sink);
    gst_object_unref(sink);
    if (peer) {
        gst_pad_send_event(peer, gst_video_event_new_upstream_force_key_unit(
            GST_CLOCK_TIME_NONE, TRUE, 0));
        gst_object_unref(peer);
    }
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
    enc_first_in_pts = GST_CLOCK_TIME_NONE;
    enc_out_checked = FALSE;
}
