//
//  RCTGstPlayerController.h
//  RCTGstPlayer
//
//  Created by Alann on 20/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#import <UIKit/UIKit.h>
#import "gstreamer_backend.h"
#import "RctGstParentView.h"
#import "DrawableSurfaceFactory.h"

@interface RCTGstPlayerController : UIViewController {
    RctGstParentView *_view;
}
// Tears the player down: stops capture, destroys the drawable surface and
// terminates the backend
- (void) terminate;

- (void) stopImageCapture;

// Queues a pipeline state change on the shared serial pipeline queue and returns immediately.
+ (void) enqueuePipelineState:(GstState)state;

// Runs work on the same queue as the state changes
+ (void) enqueuePipelineWork:(dispatch_block_t)work;
- (void) setCaptureFrames:(BOOL)enable;
- (RctGstConfiguration *) getConfiguration;
@end
