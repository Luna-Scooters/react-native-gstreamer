//
//  RCTGstPlayerController.m
//  RCTGstPlayer
//
//  Created by Alann on 20/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#import "RCTGstPlayerController.h"
#import <stdatomic.h>

@interface RCTGstPlayerController ()
{
    RctGstConfiguration *configuration;
    EaglUIView *drawableSurface;

    _Atomic(NSUInteger) teardownGeneration;
}

- (void)performTeardown;

@end

@implementation RCTGstPlayerController

// For access in pure C callbacks
static RCTGstPlayerController *currentInstance = nil;

dispatch_queue_t background_queue = NULL;
dispatch_queue_t events_queue;
static dispatch_queue_t pipeline_state_queue = NULL;

+ (void)initialize
{
    if (self == [RCTGstPlayerController class]) {
        pipeline_state_queue = dispatch_queue_create("RctGstPipelineStateQueue", 0);
        background_queue = dispatch_queue_create("RctGstBackgroundQueue", 0);
        events_queue = dispatch_get_main_queue();
    }
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
    self = [super init];
    if (self) {
        _view = [[RctGstParentView alloc] init];
        currentInstance = self;
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
    NSString *uri = [NSString stringWithUTF8String:newUri];
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping URI change event");
                return;
            }
        
            currentInstance->_view.onUriChanged(@{ @"new_uri": uri });
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
    NSString *source = [NSString stringWithUTF8String:_source];
    NSString *message = [NSString stringWithUTF8String:_message];
    NSString *debug_info = [NSString stringWithUTF8String:_debug_info];

    NSLog(@"onElementError: source: %@, message: %@, debug_info: %@", source, message, debug_info);
    if (events_queue != NULL)
        dispatch_async(events_queue, ^{
            if (currentInstance == nil || currentInstance->_view == nil) {
                NSLog(@"currentInstance or _view is nil, skipping element error event");
                return;
            }
            currentInstance->_view.onElementError(@{
                                    @"source": source,
                                    @"message": message,
                                    @"debug_info": debug_info
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
    
    // Run pipeline
    if (background_queue != NULL)
        dispatch_async(background_queue, ^{
            rct_gst_run_loop();
        });
}

// Memory management
- (void)terminate
{
    if (currentInstance == self) {
        currentInstance = nil;

        rct_gst_terminate();
        [self destroyDrawableSurface];
    }
}

- (void)dealloc
{
    [self terminate];
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

@end
