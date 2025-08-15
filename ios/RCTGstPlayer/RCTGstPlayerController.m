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

@interface RCTGstPlayerController ()
{
    RctGstConfiguration *configuration;
    EaglUIView *drawableSurface;
    
    atomic_bool runCopyImageThread;
    dispatch_queue_t imageCaptureQueue;
    long lastCaptureTimeMs;
    long captureFps;
    long capturePeriodMs;
    UIGraphicsImageRenderer *imageRenderer;
}

@end

@implementation RCTGstPlayerController

// For access in pure C callbacks
static RCTGstPlayerController *currentInstance = nil;

gchar *new_uri;
gchar *source, *message, *debug_info;

NSNumber* oldState;
NSNumber* newState;

dispatch_queue_t background_queue = NULL;
dispatch_queue_t events_queue;

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
        

        atomic_store(&runCopyImageThread, false);
        captureFps = 30;
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
        rct_gst_set_drawable_surface(0);
        [self->drawableSurface removeFromSuperview];
        self->drawableSurface = nil;
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
    while (atomic_load(&runCopyImageThread)) {
        @autoreleasepool {
            __block UIImage *image = nil;
            
            dispatch_sync(dispatch_get_main_queue(), ^{
                CGRect bounds = self->drawableSurface.bounds;
                if (bounds.size.width <= 0 || bounds.size.height <= 0) {
                    NSLog(@"Surface with invalid size: %f x %f", bounds.size.width, bounds.size.height);
                    [NSThread sleepForTimeInterval:0.1];
                    return;
                }

                if (imageRenderer == nil || !CGSizeEqualToSize(imageRenderer.format.bounds.size, bounds.size)) {
                    imageRenderer = [[UIGraphicsImageRenderer alloc] initWithSize:bounds.size];
                }
                
                image = [imageRenderer imageWithActions:^(UIGraphicsImageRendererContext * _Nonnull rendererContext) {
                    [self->drawableSurface drawViewHierarchyInRect:bounds afterScreenUpdates:NO];
                }];
            });
            
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
    
    oldState = [NSNumber numberWithInt:old_state];
    newState = [NSNumber numberWithInt:new_state];
    
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping state change event");
                return;
            }
            NSLog(@"mydebug : new_state -> %s (%d -> %d)", gst_element_state_get_name(new_state), oldState, newState);
            currentInstance->_view.onStateChanged(@{ @"old_state": oldState, @"new_state": newState });
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
    
    [self startImageCaptureThread];
    
    // Run pipeline
    if (background_queue != NULL)
        dispatch_async(background_queue, ^{
            rct_gst_run_loop();
        });
}

// Memory management
- (void)dealloc
{
    atomic_store(&runCopyImageThread, false);

    if (currentInstance == self) {
        currentInstance = nil;
    }

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

- (void)startImageCaptureThread {
    NSLog(@"Initializing and starting image capture thread");
    if (imageCaptureQueue == NULL) {
        imageCaptureQueue = dispatch_queue_create("RctGstImageCaptureQueue", 0);
    }
    
    if (!atomic_load(&runCopyImageThread)) {
        atomic_store(&runCopyImageThread, true);
        dispatch_async(imageCaptureQueue, ^{
            [self threadCopyImageFunc];
        });
    }
}

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];
    atomic_store(&runCopyImageThread, false);
}

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    
    
    [[ImageCache getInstance] getImage:YES];
    
    imageRenderer = nil;
}

@end
