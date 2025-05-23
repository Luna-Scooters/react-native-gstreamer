#ifndef __GST_IOS_INIT_H__
#define __GST_IOS_INIT_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_G_IO_MODULE_DECLARE(name) \
extern void G_PASTE(g_io_module_, G_PASTE(name, _load_static)) (void)

#define GST_G_IO_MODULE_LOAD(name) \
G_PASTE(g_io_module_, G_PASTE(name, _load_static)) ()

/* Plugin system macros */
#define GST_PLUGIN_STATIC_DECLARE(name) extern void G_PASTE(gst_plugin_, G_PASTE(name, _register)) (void)
#define GST_PLUGIN_STATIC_REGISTER(name) G_PASTE(gst_plugin_, G_PASTE(name, _register)) ()

/* Uncomment each line to enable the plugin categories that your application needs.
 * You can also enable individual plugins. See gst_ios_init.c to see their names
 */

#define GST_IOS_PLUGINS_CORE
#define GST_IOS_PLUGINS_CODECS_RESTRICTED
#define GST_IOS_PLUGINS_ENCODING
#define GST_IOS_PLUGINS_CODECS_GPL
#define GST_IOS_PLUGINS_NET_RESTRICTED
#define GST_IOS_PLUGINS_SYS
#define GST_IOS_PLUGINS_VIS
#define GST_IOS_PLUGINS_PLAYBACK
#define GST_IOS_PLUGINS_EFFECTS
#define GST_IOS_PLUGINS_CODECS
#define GST_IOS_PLUGINS_NET

void gst_ios_init (void);

G_END_DECLS

#endif
