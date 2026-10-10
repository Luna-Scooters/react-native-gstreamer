#import "RctGstVulkanViewGuard.h"
#import <objc/runtime.h>
#import <gst/vulkan/vulkan.h>

static GstVulkanWindow *vulkanWindowFromUIView(UIView *view)
{
    Ivar ivar = class_getInstanceVariable([view class], "window_ios");
    if (!ivar)
        return NULL;
    return *(GstVulkanWindow **)((uint8_t *)(__bridge void *)view + ivar_getOffset(ivar));
}

// GstVulkanUIView's setter (gstvkwindow_ios.m), which no public header declares.
@interface UIView (GstVulkanUIView)
- (void)setGstWindow:(GstVulkanWindow *)window;
@end

@interface RctGstVulkanViewGuard ()
- (void)retire;
@end

// Emitted by gst_vulkan_window_close()
static gboolean onWindowClose(GstVulkanWindow *window, gpointer guard)
{
    RctGstVulkanViewGuard *strongGuard = (__bridge RctGstVulkanViewGuard *)guard;
    dispatch_async(dispatch_get_main_queue(), ^{
        [strongGuard retire];
    });
    return FALSE;
}

static void releaseGuard(gpointer guard, GClosure *closure)
{
    CFBridgingRelease(guard);
}

@implementation RctGstVulkanViewGuard {
    UIView *_view;
    GstVulkanWindow *_window;
}

- (instancetype)initWithView:(UIView *)view
{
    self = [super init];
    if (self) {
        GstVulkanWindow *window = vulkanWindowFromUIView(view);
        if (!window) {
            NSLog(@"GstVulkanUIView has no window_ios: its stopped views will not be detached");
            return nil;
        }
        _view = view;
        _window = gst_object_ref(window);
        g_signal_connect_data(window, "close", G_CALLBACK(onWindowClose),
                                         (void *)CFBridgingRetain(self), releaseGuard, 0);
    }
    return self;
}

// Main thread, once vulkansink has closed the window.
- (void)retire
{
    if (!_window)
        return;

    // The close vfunc gst_vulkan_window_close() skipped: it detaches the view and
    // drops GStreamer's own references to the view and its layer.
    if (vulkanWindowFromUIView(_view) == _window)
        GST_VULKAN_WINDOW_GET_CLASS(_window)->close(_window);

    [_view removeFromSuperview];

    GstVulkanWindow *window = _window;
    _window = NULL;
    gst_object_unref(window);
}

@end
