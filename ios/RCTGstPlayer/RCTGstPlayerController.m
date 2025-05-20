//
//  RCTGstPlayerController.m
//  RCTGstPlayer
//
//  Created by Alann on 20/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#import "RCTGstPlayerController.h"
#import "ImageCache.h"

@interface RCTGstPlayerController ()
{
    RctGstConfiguration *configuration;
    EaglUIView *drawableSurface;
    
    BOOL runCopyImageThread;
    dispatch_queue_t imageCaptureQueue;
    int lastCaptureTimeMs;
    int captureFps;
    int capturePeriodMs;
    UIGraphicsImageRenderer *imageRenderer;
    CGRect lastCapturedBounds;
}

@end

@implementation RCTGstPlayerController

// For access in pure C callbacks
RctGstParentView *c_view;

gchar *new_uri;
gchar *source, *message, *debug_info;

NSNumber* oldState;
NSNumber* newState;

dispatch_queue_t background_queue = NULL;
dispatch_queue_t events_queue;

// Generate custom view to return to react-native (for events handle)
@dynamic view;
- (void)loadView {
    self.view = c_view;
}

- (instancetype)init
{
    
    background_queue = dispatch_queue_create("RctGstBackgroundQueue", 0);
    events_queue = dispatch_get_main_queue();
    
    self = [super init];
    if (self) {
        c_view = [[RctGstParentView alloc] init];
        

        runCopyImageThread = NO;
        captureFps = 30;
        capturePeriodMs = (1000 / captureFps);
        lastCaptureTimeMs = 0;

        imageRenderer = nil;
        lastCapturedBounds = CGRectMake(0, 0, 0, 0);
        
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
        [self->drawableSurface removeFromSuperview];
    }
}

// Destroy and recreate a window
- (void) recreateView
{
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            [self destroyDrawableSurface];
            [self createDrawableSurface];
        });
}


- (void)threadCopyImageFunc {
    NSLog(@"Starting image capture thread");
    while (runCopyImageThread) {
        @autoreleasepool {
            if (self->drawableSurface) {
                CGRect bounds = self->drawableSurface.bounds;

                if (imageRenderer == nil || !CGRectEqualToRect(bounds, lastCapturedBounds)) {
                    // Create a new image renderer if needed
                    imageRenderer = [[UIGraphicsImageRenderer alloc] initWithSize:bounds.size];
                    lastCapturedBounds = bounds;
                }
                UIImage *image = [imageRenderer imageWithActions:^(UIGraphicsImageRendererContext * _Nonnull rendererContext) {
                    [self->drawableSurface drawViewHierarchyInRect:bounds afterScreenUpdates:NO];
                }];
                
                if (image) {
                    UIImage *cachedImage = [[ImageCache getInstance] getImage:YES];
                    
                    if (cachedImage == nil || ![cachedImage isEqual:image]) {
                        [[ImageCache getInstance] setImage:image];
                    }
                }
            }
            
            // Record the capture time
            self->lastCaptureTimeMs = (int)([[NSDate date] timeIntervalSince1970] * 1000);
            
            // Sleep to maintain capture rate
            int currentTimeMs = (int)([[NSDate date] timeIntervalSince1970] * 1000);
            int timeDiffMs = currentTimeMs - self->lastCaptureTimeMs;
            int sleepPeriodMs = self->capturePeriodMs - timeDiffMs;
            
            if (sleepPeriodMs > 0) {
                [NSThread sleepForTimeInterval:sleepPeriodMs / 1000.0];
            }
        }
    }
}

// When the player has inited
void onInit() {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            c_view.onPlayerInit(@{});
        });
}

void onStateChanged(GstState old_state, GstState new_state) {
    
    oldState = [NSNumber numberWithInt:old_state];
    newState = [NSNumber numberWithInt:new_state];
    
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            NSLog(@"mydebug : new_state -> %s (%d -> %d)", gst_element_state_get_name(new_state), oldState, newState);
            c_view.onStateChanged(@{ @"old_state": oldState, @"new_state": newState });
        });
}

void onVolumeChanged(RctGstAudioLevel* audioLevel) {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
        c_view.onVolumeChanged(@{
                                 @"decay": @(audioLevel->decay),
                                 @"rms": @(audioLevel->rms),
                                 @"peak": @(audioLevel->peak),
                                 });
        });
}

void onUriChanged(gchar* newUri) {
    new_uri = g_strdup(newUri);
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
        c_view.onUriChanged(@{ @"new_uri": [NSString stringWithUTF8String:new_uri] });
        });
}

void onEOS() {
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            c_view.onEOS(@{});
        });
}

void onElementError(gchar *_source, gchar *_message, gchar *_debug_info) {
    source = g_strdup(_source);
    message = g_strdup(_message);
    debug_info = g_strdup(_debug_info);
    
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            c_view.onElementError(@{
                                    @"source": [NSString stringWithUTF8String:source],
                                    @"message": [NSString stringWithUTF8String:message],
                                    @"debug_info": [NSString stringWithUTF8String:debug_info]
                                    });
        });
}

- (void)viewDidLoad {
    [super viewDidLoad];

    // Preparing surface
    [self createDrawableSurface];
    
    // Preparing configuration
    configuration = rct_gst_get_configuration();
    configuration->initialDrawableSurface = self->drawableSurface;
    
    configuration->onInit = onInit;
    configuration->onStateChanged = onStateChanged;
    configuration->onVolumeChanged = onVolumeChanged;
    configuration->onUriChanged = onUriChanged;
    configuration->onEOS = onEOS;
    configuration->onElementError = onElementError;

    // Preparing pipeline
    rct_gst_init(configuration);
    
    imageCaptureQueue = dispatch_queue_create("com.kalyzee.rctgstplayer.imageCaptureQueue", DISPATCH_QUEUE_SERIAL);
    runCopyImageThread = YES;
    dispatch_async(imageCaptureQueue, ^{
        [self threadCopyImageFunc];
    });
    
    // Run pipeline
    if (background_queue != NULL)
        dispatch_async(background_queue, ^{
            rct_gst_run_loop();
        });
}

// Memory management
- (void)dealloc
{
    runCopyImageThread = NO;
    
    [[ImageCache getInstance] getImage:YES];
    
    imageRenderer = nil;
    
    rct_gst_terminate();
    g_free(new_uri);
    g_free(source);
    g_free(message);
    g_free(debug_info);

    if (self->drawableSurface != NULL)
        self->drawableSurface = NULL;
}

@end
