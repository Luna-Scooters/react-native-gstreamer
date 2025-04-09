package com.kalyzee.rctgstplayer.utils;

import android.graphics.Bitmap;
import android.util.LruCache;

public class ImageCache {
  private static volatile ImageCache instance;
  private static final Object LOCK = new Object();
  private final String cacheKey = "image";
  private final LruCache<String, Bitmap> cacheBitmap = new LruCache<>(1);

  private ImageCache() {}

  public static ImageCache getInstance() {
    if (instance == null) {
      synchronized (LOCK) { // Ensure thread safety
        if (instance == null) {
          instance = new ImageCache();
        }
      }
    }
    return instance;
  }

  public void setBitmap(Bitmap bitmap) { cacheBitmap.put(cacheKey, bitmap); }

  public Bitmap getBitmap() { return cacheBitmap.get(cacheKey); }
}
