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

/* GST_IOS_PLUGIN_LIST(F)=F(coreelements)F(rtsp)...F(applemedia), derived from
 * gst-plugins.cmake and passed in as a build setting. gst_ios_init.m expands it
 * through the two helpers below.
 */
#ifndef GST_IOS_PLUGIN_LIST
#error "GST_IOS_PLUGIN_LIST is undefined -- the podspec puts it in the pod's GCC_PREPROCESSOR_DEFINITIONS; a CMake target passes GST_IOS_PLUGIN_LIST_DEFINE from gst-plugins.cmake."
#endif

#define GST_IOS_PLUGIN_DECLARE(name) GST_PLUGIN_STATIC_DECLARE(name);
#define GST_IOS_PLUGIN_REGISTER(name) GST_PLUGIN_STATIC_REGISTER(name);

void gst_ios_init (void);

G_END_DECLS

#endif
