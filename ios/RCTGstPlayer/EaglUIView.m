//
//  EaglUiView.m
//
//  Created by Alann on 13/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#import "EaglUIView.h"
#import "RctGstVulkanViewGuard.h"
#import <QuartzCore/CAMetalLayer.h>

@implementation EaglUIView

+ (Class) layerClass
{
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        self->handle = (guintptr)(id)self;
        NSLog(@"Creating video surface with pointer : %lu", self->handle);
    }
    return self;
}

- (void)dealloc
{
    NSLog(@"Removing video surface with pointer : %lu", self->handle);
}

- (guintptr)getHandle
{
    return self->handle;
}

// vulkansink adds its view here on the main thread, already attached to its window.
- (void)didAddSubview:(UIView *)subview
{
    [super didAddSubview:subview];

    if (![NSStringFromClass([subview class]) isEqualToString:@"GstVulkanUIView"])
        return;

    // Owned by the window's close handler from here on.
    (void)[[RctGstVulkanViewGuard alloc] initWithView:subview];
}

@end
