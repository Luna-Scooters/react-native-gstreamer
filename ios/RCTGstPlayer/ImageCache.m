#import "ImageCache.h"

@implementation ImageCache {
    NSCache<NSString *, NSValue *> *_imageCache;
    NSString *_cacheKey;
}

+ (instancetype)getInstance {
    static ImageCache *instance = nil;
    static dispatch_once_t onceToken;
    
    dispatch_once(&onceToken, ^{
        instance = [[ImageCache alloc] init];
    });
    
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _imageCache = [[NSCache alloc] init];
        [_imageCache setCountLimit:1];
        _cacheKey = @"image";
    }
    return self;
}

- (void)setImage:(CGImageRef)image {
    if (image) {
        // Clean up any existing image first
        [self evictCurrentImage];
        
        NSValue *imageValue = [NSValue valueWithPointer:image];
        [_imageCache setObject:imageValue forKey:_cacheKey];
        // Retain the image since we're storing it
        CGImageRetain(image);
    }
}

// Helper method to clean up existing image
- (void)evictCurrentImage {
    NSValue *existingImageValue = [_imageCache objectForKey:_cacheKey];
    if (existingImageValue) {
        CGImageRef existingImage = (CGImageRef)[existingImageValue pointerValue];
        if (existingImage) {
            CGImageRelease(existingImage);
        }
        [_imageCache removeObjectForKey:_cacheKey];
    }
}


- (CGImageRef)getImage:(BOOL)evict {
    NSValue *imageValue = [_imageCache objectForKey:_cacheKey];
    CGImageRef image = NULL;
    
    if (imageValue) {
        image = (CGImageRef)[imageValue pointerValue];
        // Only retain if we're not evicting (caller will own the reference)
        if (!evict && image) {
            CGImageRetain(image);
        }
    }
    
    if (evict && imageValue) {
        [_imageCache removeObjectForKey:_cacheKey];
        // Release the cached reference when evicting
        if (image) {
            CGImageRelease(image);
            // Don't return the released image
            image = NULL;
        }
    }
    
    return image;
}

- (void)dealloc {
    // Clean up any cached image before deallocation
    [self evictCurrentImage];
}

@end
