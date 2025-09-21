#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface ImageCache : NSObject

+ (instancetype)getInstance;

- (void)setImage:(CGImageRef)image;

- (CGImageRef _Nullable)getImage:(BOOL)evict;

@end

NS_ASSUME_NONNULL_END
