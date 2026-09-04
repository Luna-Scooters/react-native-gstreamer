//
//  RCTGstPlayerController.m
//  RCTGstPlayer
//
//  Created by Alann on 20/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#import "RCTGstPlayerController.h"
#import "ImageCache.h"
#import <stdatomic.h>
#import <Metal/Metal.h>
#import <gst/vulkan/vulkan.h>
#import <MoltenVK/mvk_deprecated_api.h>

@interface RCTGstPlayerController ()
{
    RctGstConfiguration *configuration;
    EaglUIView *drawableSurface;
    
    atomic_bool runCopyImageThread;
    atomic_bool captureFrames;
    dispatch_queue_t imageCaptureQueue;
    dispatch_semaphore_t imageCaptureDone;
    long lastCaptureTimeMs;
    long captureFps;
    long capturePeriodMs;
    UIGraphicsImageRenderer *imageRenderer;

    _Atomic(NSUInteger) teardownGeneration;
}

- (void)performTeardown;

@end

@implementation RCTGstPlayerController

// For access in pure C callbacks
static RCTGstPlayerController *currentInstance = nil;

gchar *new_uri;
gchar *source, *message, *debug_info;

dispatch_queue_t background_queue = NULL;
dispatch_queue_t events_queue;
static dispatch_queue_t pipeline_state_queue = NULL;

+ (void)initialize
{
    if (self == [RCTGstPlayerController class])
        pipeline_state_queue = dispatch_queue_create("RctGstPipelineStateQueue", 0);
}

+ (void)enqueuePipelineState:(GstState)state
{
    dispatch_async(pipeline_state_queue, ^{
        rct_gst_set_pipeline_state(state);
    });
}

+ (void)enqueuePipelineWork:(dispatch_block_t)work
{
    if (work)
        dispatch_async(pipeline_state_queue, work);
}

// Generate custom view to return to react-native (for events handle)
@dynamic view;
- (void)loadView {
    if (!_view) {
        _view = [[RctGstParentView alloc] init];
    }
    self.view = _view;
}

- (instancetype)init
{
    
    background_queue = dispatch_queue_create("RctGstBackgroundQueue", 0);
    events_queue = dispatch_get_main_queue();
    
    self = [super init];
    if (self) {
        _view = [[RctGstParentView alloc] init];
        currentInstance = self;
        

        [self stopImageCapture];
        captureFps = 15;
        capturePeriodMs = (1000 / captureFps);
        lastCaptureTimeMs = (long)([[NSDate date] timeIntervalSince1970] * 1000);;

        imageRenderer = nil;
        
        new_uri = g_malloc(sizeof(gchar));
        
        source = g_malloc(sizeof(gchar));
        message = g_malloc(sizeof(gchar));
        debug_info = g_malloc(sizeof(gchar));
    }
    return self;
}

// Get configuration
- (RctGstConfiguration *)getConfiguration
{
    return self->configuration;
}

// Create a new drawable surface
- (void) createDrawableSurface
{
    g_print("createDrawableSurface\n");
    self->drawableSurface = [DrawableSurfaceFactory getView:self.view];
    [self.view addSubview:self->drawableSurface];
    rct_gst_set_drawable_surface([self->drawableSurface getHandle]);
}

// Destroy an old drawable surface
- (void) destroyDrawableSurface
{
    if (self->drawableSurface) {
        if ([NSThread isMainThread]) {
            [self->drawableSurface removeFromSuperview];
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{
                [self->drawableSurface removeFromSuperview];
            });
        }
        
        self->drawableSurface = nil;
    }
}

// Cached Metal objects for the GPU->CPU readback blit.
static id<MTLCommandQueue> s_blitQueue = nil;
static id<MTLBuffer> s_blitBuffer = nil;
static NSUInteger s_blitBufferLen = 0;

// Capture runs ~30x/sec, so log only on outcome transitions (not every frame).
static int s_capState = -1;        // 0 no-sample, 1 not-vulkan, 2 no-proc, 3 no-texture, 4 ok
static BOOL s_loggedTexInfo = NO;  // one-time texture format/usage log
static BOOL s_loggedBlitErr = NO;  // one-time blit-error log
static void capLogOnce(int st, NSString *msg) {
    if (st != s_capState) { s_capState = st; NSLog(@"[GST-VK] capture: %@", msg); }
}

// Grabs vulkansink's last rendered frame
- (UIImage *)captureVulkanFrame {
    GstSample *sample = rct_gst_pull_last_sample();
    if (!sample) {
        capLogOnce(0, @"no last-sample yet (sink not playing / enable-last-sample off?)");
        return nil;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMemory *mem = buffer ? gst_buffer_peek_memory(buffer, 0) : NULL;
    if (!mem || !gst_is_vulkan_image_memory(mem)) {
        capLogOnce(1, [NSString stringWithFormat:@"buffer mem is not vulkan image memory (mem=%p)", mem]);
        gst_sample_unref(sample);
        return nil;
    }

    GstVulkanImageMemory *vkmem = (GstVulkanImageMemory *)mem;
    // Call MoltenVK's exporter directly: the symbol is in libGStreamer.a, but it's not
    // in the device dispatch table (VK_MVK_moltenvk isn't enabled), so the proc-address
    // lookup returns NULL. (Deprecated API, but the simplest VkImage -> MTLTexture path.)
    id<MTLTexture> tex = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    vkGetMTLTextureMVK(vkmem->image, &tex);
#pragma clang diagnostic pop
    if (!tex) {
        capLogOnce(3, @"vkGetMTLTextureMVK returned a nil MTLTexture");
        gst_sample_unref(sample);
        return nil;
    }

    NSUInteger w = tex.width, h = tex.height;
    if (!s_loggedTexInfo) {
        s_loggedTexInfo = YES;
        NSLog(@"[GST-VK] capture: MTLTexture %lux%lu pixelFormat=%lu usage=0x%lx storageMode=%lu",
              (unsigned long)w, (unsigned long)h, (unsigned long)tex.pixelFormat,
              (unsigned long)tex.usage, (unsigned long)tex.storageMode);
    }

    UIImage *result = nil;
    NSUInteger bytesPerRow = w * 4;
    NSUInteger total = bytesPerRow * h;
    id<MTLDevice> dev = tex.device;

    if (s_blitQueue == nil)
        s_blitQueue = [dev newCommandQueue];
    if (s_blitBuffer == nil || s_blitBufferLen < total) {
        s_blitBuffer = [dev newBufferWithLength:total options:MTLResourceStorageModeShared];
        s_blitBufferLen = total;
    }

    id<MTLCommandBuffer> cmd = [s_blitQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:s_blitBuffer
        destinationOffset:0
   destinationBytesPerRow:bytesPerRow
 destinationBytesPerImage:total];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.error && !s_loggedBlitErr) {
        s_loggedBlitErr = YES;
        NSLog(@"[GST-VK] capture: blit command error: %@", cmd.error);
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(s_blitBuffer.contents, w, h, 8, bytesPerRow, cs,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst); // BGRA
    if (ctx) {
        CGImageRef cgImg = CGBitmapContextCreateImage(ctx);
        if (cgImg) {
            result = [UIImage imageWithCGImage:cgImg];
            CGImageRelease(cgImg);
        }
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);

    capLogOnce(4, [NSString stringWithFormat:@"OK -> feeding %lux%lu frames to ImageCache", (unsigned long)w, (unsigned long)h]);
    gst_sample_unref(sample);
    return result;
}

- (void)threadCopyImageFunc {
    NSLog(@"Starting image capture thread");
    while (atomic_load(&runCopyImageThread)) {
        @autoreleasepool {
            UIImage *image = [self captureVulkanFrame];
            if (image) {
                [[ImageCache getInstance] setImage:image];
            }
        }
        
        long currentTimeMs = (long)([[NSDate date] timeIntervalSince1970] * 1000);
        long timeDiffMs = currentTimeMs - self->lastCaptureTimeMs;
        long sleepPeriodMs = self->capturePeriodMs - timeDiffMs;

        self->lastCaptureTimeMs = currentTimeMs;
        
        if (sleepPeriodMs > 0) {
            [NSThread sleepForTimeInterval:sleepPeriodMs / 1000.0];
        }
    }
}

// When the player has inited
void onInit() {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping onPlayerInit event");
                return;
            }
            currentInstance->_view.onPlayerInit(@{});
        });
}

void onStateChanged(GstState old_state, GstState new_state) {
    
    NSNumber *oldStateNumber = @(old_state);
    NSNumber *newStateNumber = @(new_state);

    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping state change event");
                return;
            }
            NSLog(@"mydebug : new_state -> %s (%@ -> %@)", gst_element_state_get_name(new_state), oldStateNumber, newStateNumber);
            currentInstance->_view.onStateChanged(@{ @"old_state": oldStateNumber, @"new_state": newStateNumber });
        });
}

void onVolumeChanged(RctGstAudioLevel* audioLevel) {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping volume change event");
                return;
            }
            currentInstance->_view.onVolumeChanged(@{
                                     @"decay": @(audioLevel->decay),
                                     @"rms": @(audioLevel->rms),
                                     @"peak": @(audioLevel->peak),
                                     });
        });
}

void onUriChanged(gchar* newUri) {
    g_free(new_uri);
    new_uri = g_strdup(newUri);
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping URI change event");
                return;
            }
        
            currentInstance->_view.onUriChanged(@{ @"new_uri": [NSString stringWithUTF8String:new_uri] });
        });
}

void onEOS() {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping EOS event");
                return;
            }
            currentInstance->_view.onEOS(@{});
        });
}

void onElementError(gchar *_source, gchar *_message, gchar *_debug_info) {
    g_free(source);
    g_free(message);
    g_free(debug_info);
    source = g_strdup(_source);
    message = g_strdup(_message);
    debug_info = g_strdup(_debug_info);
    
    NSLog(@"onElementError: source: %s, message: %s, debug_info: %s", source, message, debug_info);
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping element error event");
                return;
            }
            currentInstance->_view.onElementError(@{
                                    @"source": [NSString stringWithUTF8String:source],
                                    @"message": [NSString stringWithUTF8String:message],
                                    @"debug_info": [NSString stringWithUTF8String:debug_info]
                                    });
        });
}

void onRecordingFinished() {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping recording finished event");
                return;
            }
            if (currentInstance->_view.onRecordingFinished)
                currentInstance->_view.onRecordingFinished(@{});
        });
}

void onEventSaved(gchar *file_path) {
    if (events_queue == NULL)
        return;
    // Copy the path now — the caller frees it as soon as this returns.
    NSString *path = file_path ? [NSString stringWithUTF8String:file_path] : @"";
    dispatch_async(events_queue, ^{
        if (currentInstance == nil || currentInstance->_view == nil) {
            NSLog(@"currentInstance or _view is nil, skipping event saved event");
            return;
        }
        if (currentInstance->_view.onEventSaved)
            currentInstance->_view.onEventSaved(@{@"videoFilename": path});
    });
}

- (void)viewDidLoad {
    [super viewDidLoad];

    // Preparing surface
    [self createDrawableSurface];
    
    // Preparing configuration
    configuration = rct_gst_get_configuration();
    configuration->initialDrawableSurface = [self->drawableSurface getHandle];
    
    configuration->onInit = onInit;
    configuration->onStateChanged = onStateChanged;
    configuration->onVolumeChanged = onVolumeChanged;
    configuration->onUriChanged = onUriChanged;
    configuration->onEOS = onEOS;
    configuration->onElementError = onElementError;
    configuration->onRecordingFinished = onRecordingFinished;
    configuration->onEventSaved = onEventSaved;

    // Preparing pipeline
    rct_gst_init(configuration);
    
    [self startImageCaptureThread];
    
    // Run pipeline
    if (background_queue != NULL)
        dispatch_async(background_queue, ^{
            rct_gst_run_loop();
        });
}

// Memory management
- (void)terminate
{
    [self stopImageCapture];

    [[ImageCache getInstance] getImage:YES];

    if (currentInstance == self) {
        currentInstance = nil;

        rct_gst_terminate();
        g_free(new_uri);
        g_free(source);
        g_free(message);
        g_free(debug_info);
        new_uri = NULL;
        source = NULL;
        message = NULL;
        debug_info = NULL;
        [self destroyDrawableSurface];
    }
}

- (void)dealloc
{
    [self terminate];
}

- (void)startImageCaptureThread {
    if (!atomic_load(&captureFrames)) {
        return;
    }
    NSLog(@"Initializing and starting image capture thread");
    if (imageCaptureQueue == NULL) {
        imageCaptureQueue = dispatch_queue_create("RctGstImageCaptureQueue", 0);
    }
    
    if (!atomic_load(&runCopyImageThread)) {
        atomic_store(&runCopyImageThread, true);

        // Signalled when the loop leaves threadCopyImageFunc, so stopImageCapture
        // can wait for it instead of just asking it to stop.
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        imageCaptureDone = done;

        dispatch_async(imageCaptureQueue, ^{
            [self threadCopyImageFunc];
            dispatch_semaphore_signal(done);
        });
    }
}

- (void)stopImageCapture {
    NSLog(@"Stopping image capture thread");
    atomic_store(&runCopyImageThread, false);

    // Wait for the loop to actually exit, don't just ask it to. It reads the
    // backend's video sink through rct_gst_pull_last_sample, and that backend
    // state is file-global -- shared by every player instance -- so returning
    // while a capture is still in flight lets rct_gst_terminate (or a new
    // instance's rct_gst_init) drop the sink underneath it.
    //
    // Bounded, so a wedged Vulkan readback can't hang the caller; the backend
    // also holds a lock around the sink to keep that case safe.
    dispatch_semaphore_t done = imageCaptureDone;
    imageCaptureDone = nil;
    if (done == nil)
        return;

    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
                                                    (int64_t)(2 * NSEC_PER_SEC))) != 0)
        NSLog(@"Image capture thread did not stop within 2s");
}

- (void)setCaptureFrames:(BOOL)enable {
    atomic_store(&captureFrames, enable);
    if (enable) {
        [self startImageCaptureThread];
    } else {
        [self stopImageCapture];
    }
}

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];

    NSUInteger generation = atomic_fetch_add(&teardownGeneration, 1) + 1;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (generation != atomic_load(&self->teardownGeneration))
            return;                          // superseded by a later transition
        if (self->_view.window != nil)
            return;                          // bounced back; never really left

        [self performTeardown];
    });
}

// The former body of -viewWillDisappear, unchanged apart from now being gated.
- (void)performTeardown
{
    if (currentInstance != self)
        return;

    [self stopImageCapture];

    NSArray<UIView *> *orphans = [self->drawableSurface.subviews copy];

    [RCTGstPlayerController enqueuePipelineWork:^{
        rct_gst_set_pipeline_state(GST_STATE_NULL);
        [self removeGstSubviews:orphans];
    }];
}

- (void)removeGstSubviews:(NSArray<UIView *> *)views
{
    if (views.count == 0)
        return;
    void (^sweep)(void) = ^{
        for (UIView *sub in views) {
            [sub removeFromSuperview];
        }
    };
    if ([NSThread isMainThread]) {
        sweep();
    } else {
        dispatch_async(dispatch_get_main_queue(), sweep);
    }
}

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    [[ImageCache getInstance] getImage:YES];
}

@end
