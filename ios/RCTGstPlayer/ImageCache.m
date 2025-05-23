#import "ImageCache.h"

@implementation ImageCache {
    NSCache<NSString *, UIImage *> *_imageCache;
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

- (void)setImage:(UIImage *)image {
    if (image) {
        [_imageCache setObject:image forKey:_cacheKey];
    }
}

- (UIImage *)getImage:(BOOL)evict {
    UIImage *image = [_imageCache objectForKey:_cacheKey];
    
    if (evict && image) {
        [_imageCache removeObjectForKey:_cacheKey];
    }
    
    return image;
}

@end
