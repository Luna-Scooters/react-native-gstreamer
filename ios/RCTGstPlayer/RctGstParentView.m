//
//  RctGstParentView.m
//  RCTGstPlayer
//
//  Created by Alann on 21/12/2017.
//  Copyright © 2017 Kalyzee. All rights reserved.
//

#import "RctGstParentView.h"
#import <React/RCTUtils.h>
#import <React/UIView+React.h>

@implementation RctGstParentView

// Skip setting the transform if the new architecture is enabled,
// since it will be handled by the Fabric renderer.
- (void)setReactTransform:(CATransform3D)reactTransform
{
    if (!RCTIsNewArchEnabled())
        [super setReactTransform:reactTransform];
}

@end
