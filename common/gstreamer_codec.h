//
//  gstreamer_codec.h
//
//  Runtime selection of the best available codec elements, preferring hardware.
//

#ifndef gstreamer_codec_h
#define gstreamer_codec_h

#include <gst/gst.h>

gchar *rct_gst_find_jpeg_decoder(void);
gchar *rct_gst_find_h264_encoder(void);

#endif /* gstreamer_codec_h */
