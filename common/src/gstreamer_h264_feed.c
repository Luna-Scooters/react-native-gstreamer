//
//  gstreamer_h264_feed.c
//
//  See gstreamer_h264_feed.h.
//

#include "gstreamer_h264_feed.h"
#include "gstreamer_encoder.h"

// Owned by gstreamer_backend.c
extern GstElement *pipeline;

typedef struct {
    GstElement *src_tee;       // borrowed: the camera tee (decode chain's) or enc-tee (encoder's)
    gboolean    direct_stream; // tapping the camera's own H.264: nothing to encode
    int         refcount;
} RctGstH264Feed;

static RctGstH264Feed feed = { NULL, FALSE, 0 };

/* ---- the camera's GOP ---- */

// Kept on the camera tee, so it goes with the decode chain.
#define GOP_KEY "rct-gst-camera-gop"

// The longest GOP kept: 10 s at 60 fps.
static const guint GOP_MAX_FRAMES = 600;

// On the camera tee's sink: keep the camera's current GOP, its frames since the
// latest keyframe, so a recording that joins mid-GOP can open on that keyframe.
// A new stream starts over. The tee hands each frame to its branches on the
// thread it got it on, so this and gop_replay_probe never run at once.
static GstPadProbeReturn gop_keep_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    GPtrArray *gop = user_data;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        gboolean keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        // A GOP too long to keep is dropped; keeping starts again at the next keyframe.
        if (keyframe || gop->len >= GOP_MAX_FRAMES)
            g_ptr_array_set_size(gop, 0);
        if (keyframe || gop->len > 0)
            g_ptr_array_add(gop, gst_buffer_ref(buffer));
        return GST_PAD_PROBE_OK;
    }
    switch (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info))) {
    case GST_EVENT_STREAM_START:
    case GST_EVENT_SEGMENT:
    case GST_EVENT_FLUSH_STOP:
        g_ptr_array_set_size(gop, 0);
        break;
    default:
        break;
    }
    return GST_PAD_PROBE_OK;
}

// On a recording's ghost pad, at its first frame once linked: replay the GOP up
// to that frame - the frame itself is the last one kept - then let it through.
// With no GOP kept yet (the camera's stream opened mid-GOP), drop frames until a
// keyframe. Either way the recording opens on a keyframe.
static GstPadProbeReturn gop_replay_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GPtrArray *gop = user_data;
    g_autoptr(GstPad) peer = gst_pad_get_peer(pad);
    if (!peer)
        return GST_PAD_PROBE_OK;   // not linked yet: the frame goes nowhere

    GstBuffer *live = GST_PAD_PROBE_INFO_BUFFER(info);
    if (gop->len == 0 || g_ptr_array_index(gop, gop->len - 1) != live)
        return GST_BUFFER_FLAG_IS_SET(live, GST_BUFFER_FLAG_DELTA_UNIT)
            ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_REMOVE;

    for (guint i = 0; i + 1 < gop->len; i++)
        gst_pad_chain(peer, gst_buffer_ref(g_ptr_array_index(gop, i)));
    if (gop->len > 1)
        g_print("h264 feed: a recording joined mid-GOP; replayed %u frames from the last keyframe\n",
                gop->len - 1);
    return GST_PAD_PROBE_REMOVE;
}

void rct_gst_h264_on_decode_chain(void)
{
    g_autoptr(GstElement) camera_tee = gst_bin_get_by_name(GST_BIN(pipeline), RCT_GST_CAMERA_H264_TEE);
    if (!camera_tee)
        return;
    GPtrArray *gop = g_ptr_array_new_with_free_func((GDestroyNotify)gst_buffer_unref);
    g_object_set_data_full(G_OBJECT(camera_tee), GOP_KEY, gop, (GDestroyNotify)g_ptr_array_unref);
    g_autoptr(GstPad) sink = gst_element_get_static_pad(camera_tee, "sink");
    gst_pad_add_probe(sink,
        GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
        gop_keep_probe, gop, NULL);
}

/* ---- the feed ---- */

// Picks the tee to tap on the first request: the camera's own H.264 when the
// decode chain has it, the shared encoder's output otherwise.
static gboolean feed_start(void)
{
    if (feed.src_tee)
        return TRUE;
    if (!pipeline) {
        g_printerr("h264 feed: pipeline not ready\n");
        return FALSE;
    }

    // The camera already sends H.264: record it as it comes, from the tee the
    // decode chain keeps ahead of the decoder.
    g_autoptr(GstElement) camera_tee = gst_bin_get_by_name(GST_BIN(pipeline), RCT_GST_CAMERA_H264_TEE);
    if (camera_tee) {
        feed.src_tee = camera_tee;   // borrowed; the decode chain owns it
        feed.direct_stream = TRUE;
        g_print("H.264 feed: the camera's own stream, nothing to encode\n");
        return TRUE;
    }

    feed.src_tee = rct_gst_encoder_start();
    return feed.src_tee != NULL;
}

GstPad *rct_gst_h264_request_src_pad(void)
{
    if (!feed_start())
        return NULL;
    g_autoptr(GstPad) tee_src = gst_element_request_pad_simple(feed.src_tee, "src_%u");
    if (!tee_src) {
        g_printerr("h264 feed: %s gave no src pad\n", GST_OBJECT_NAME(feed.src_tee));
        return NULL;
    }
    // Consumers link from outside the tee's bin (the decode chain or the
    // encoder's), so they get a ghost pad on it.
    g_autoptr(GstObject) bin = gst_element_get_parent(feed.src_tee);
    GstPad *ghost = bin ? gst_ghost_pad_new(NULL, tee_src) : NULL;
    if (!ghost) {
        gst_element_release_request_pad(feed.src_tee, tee_src);
        return NULL;
    }
    GPtrArray *gop = feed.direct_stream ? g_object_get_data(G_OBJECT(feed.src_tee), GOP_KEY) : NULL;
    if (gop)
        gst_pad_add_probe(ghost, GST_PAD_PROBE_TYPE_BUFFER, gop_replay_probe, gop, NULL);
    gst_pad_set_active(ghost, TRUE);
    gst_element_add_pad(GST_ELEMENT(bin), ghost);  // sinks the floating ref; the bin owns it
    feed.refcount++;
    return gst_object_ref(ghost);                  // hand the caller its own ref
}

void rct_gst_h264_release_src_pad(GstPad *pad)
{
    if (!pad)
        return;
    g_autoptr(GstPad) tee_src = NULL;
    if (GST_IS_GHOST_PAD(pad))
        tee_src = gst_ghost_pad_get_target(GST_GHOST_PAD(pad));
    gst_pad_set_active(pad, FALSE);
    g_autoptr(GstObject) bin = gst_object_get_parent(GST_OBJECT(pad));
    if (bin)
        gst_element_remove_pad(GST_ELEMENT(bin), pad);  // drops the bin's ref
    if (tee_src && feed.src_tee)
        gst_element_release_request_pad(feed.src_tee, tee_src);
    gst_object_unref(pad);  // drops the caller's ref from request_src_pad

    // The src pads ARE the feed's refcount: stop it with the last one.
    if (feed.refcount > 0)
        feed.refcount--;
    if (feed.refcount == 0)
        rct_gst_h264_reset();
}

void rct_gst_h264_force_keyframe(void)
{
    if (feed.src_tee && !feed.direct_stream)
        rct_gst_encoder_force_keyframe();
}

void rct_gst_h264_reset(void)
{
    // The camera tee goes with the decode chain; only the encoder is ours to stop.
    rct_gst_encoder_reset();
    feed.src_tee = NULL;
    feed.direct_stream = FALSE;
    feed.refcount = 0;
}
