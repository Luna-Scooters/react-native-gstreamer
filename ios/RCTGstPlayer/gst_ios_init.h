#ifndef __GST_IOS_INIT_H__
#define __GST_IOS_INIT_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_G_IO_MODULE_DECLARE(name) \
extern void G_PASTE(g_io_module_, G_PASTE(name, _load_static)) (void)

#define GST_G_IO_MODULE_LOAD(name) \
G_PASTE(g_io_module_, G_PASTE(name, _load_static)) ()

/* Uncomment each line to enable the plugin categories that your application needs.
 * You can also enable individual plugins. See gst_ios_init.c to see their names
 */

#define GSTREAMER_PLUGINS_CORE
#define GSTREAMER_PLUGINS_PLAYBACK
#define GSTREAMER_PLUGINS_CODECS   
#define GSTREAMER_PLUGINS_NET     
#define GSTREAMER_PLUGINS_SYS
#define GSTREAMER_PLUGINS_CODECS_RESTRICTED
#define GSTREAMER_CODECS_GPL
#define GSTREAMER_PLUGINS_ENCODING
#define GSTREAMER_PLUGINS_VIS   
#define GSTREAMER_PLUGINS_EFFECTS
#define GSTREAMER_PLUGINS_NET_RESTRICTED

void gst_ios_init (void);

G_END_DECLS

#endif
