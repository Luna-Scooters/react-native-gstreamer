//
//  gstreamer_codec.h
//
//  Runtime selection of the best available codec elements, preferring hardware.
//

#ifndef gstreamer_codec_h
#define gstreamer_codec_h

#include <gst/gst.h>

gchar *rct_gst_find_h264_decoder(void);
gchar *rct_gst_find_jpeg_decoder(void);
gchar *rct_gst_find_h264_encoder(void);

// Tune an H.264 encoder element for small output files. Encoder-agnostic:
// translates each setting to whichever property the chosen encoder exposes.
//  - keyframe_interval_sec: maximum IDR interval in seconds. Longer means fewer
//    expensive I-frames. Maps to amcvidenc i-frame-interval (seconds) or
//    x264enc key-int-max / openh264enc gop-size / vtenc max-keyframe-interval
//    (frames, derived from `fps`). Pass <= 0 to leave untouched.
//  - bitrate_kbps: target bitrate in kbit/s. Maps to the encoder's `bitrate`
//    property, converting to bits/s for encoders that expect it (amcvidenc,
//    openh264enc). Pass <= 0 to leave the encoder default.
void rct_gst_configure_h264_encoder(GstElement *encoder, gint fps,
                                    gint keyframe_interval_sec,
                                    gint bitrate_kbps);

#endif /* gstreamer_codec_h */
