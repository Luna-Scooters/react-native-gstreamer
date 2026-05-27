package com.kalyzee.rctgstplayer;

import android.graphics.Bitmap;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.PixelCopy;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.Toast;
import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.ReactContext;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.uimanager.events.RCTEventEmitter;
import com.kalyzee.rctgstplayer.utils.EaglUIView;
import com.kalyzee.rctgstplayer.utils.ImageCache;
import com.kalyzee.rctgstplayer.utils.RCTGstConfiguration;
import com.kalyzee.rctgstplayer.utils.RCTGstConfigurationCallable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.freedesktop.gstreamer.GStreamer;

/**
 * Created by asapone on 02/01/2018.
 */

public class RCTGstPlayerController
    implements RCTGstConfigurationCallable, SurfaceHolder.Callback {

  private static final String LOG_TAG = "RCTGstPlayer";
  private static final int GST_STATE_PLAYING = 4;

  private boolean isInited = false;
  private boolean autoPlay = false;

  private RCTGstConfiguration configuration;
  private EaglUIView view;
  private ReactContext context;

  private AtomicBoolean runCopyImageThread = new AtomicBoolean(false);
  private CountDownLatch latchCopyImage;
  private Thread thCopyImage;
  private final boolean enableFrameCache = false;

  // Native methods
  private native String nativeRCTGstGetGStreamerInfo();
  private native void nativeRCTGstSetDrawableSurface(Surface drawableSurface);
  private native void nativeRCTGstSetUri(String uri);
  private native void
  nativeRCTGstSetAudioLevelRefreshRate(int audioLevelRefreshRate);
  private native void nativeRCTGstSetDebugging(boolean isDebugging);

  private native void nativeRCTGstSetPipelineState(int state);

  private native void nativeRCTGstInitAndRun(RCTGstConfiguration configuration);

  private boolean isBridgelessContext() {
    return context != null
        && context.getClass().getName().contains("BridgelessReactContext");
  }

  private void emitViewEvent(String eventName, WritableMap event) {
    if (isBridgelessContext() || !context.hasCatalystInstance()) {
      return;
    }

    try {
      context.getJSModule(RCTEventEmitter.class)
          .receiveEvent(view.getId(), eventName, event);
    } catch (RuntimeException e) {
      Log.w(LOG_TAG,
            "Cannot emit view event in current React architecture: " + eventName,
            e);
    }
  }

  // Configuration callbacks
  @Override
  public void onInit() {
    emitViewEvent("onPlayerInit", null);
  }

  @Override
  public void onStateChanged(int old_state, int new_state) {
    WritableMap event = Arguments.createMap();

    event.putInt("old_state", old_state);
    event.putInt("new_state", new_state);

    emitViewEvent("onStateChanged", event);
  }

  @Override
  public void onVolumeChanged(double rms, double peak, double decay) {
    WritableMap event = Arguments.createMap();

    event.putDouble("rms", rms);
    event.putDouble("peak", peak);
    event.putDouble("decay", decay);

    emitViewEvent("onVolumeChanged", event);
  }

  @Override
  public void onUriChanged(String new_uri) {
    WritableMap event = Arguments.createMap();

    event.putString("new_uri", new_uri);

    emitViewEvent("onUriChanged", event);
  }

  @Override
  public void onEOS() {
    emitViewEvent("onEOS", null);
  }

  @Override
  public void onElementError(String source, String message, String debug_info) {
    WritableMap event = Arguments.createMap();

    event.putString("source", source);
    event.putString("message", message);
    event.putString("debug_info", debug_info);

    emitViewEvent("onElementError", event);
  }

  private final int captureFps = 30; /* Capture Frame Rate of 30 FPS */
  private final double capturePeriodMs = (1.0 / (double)captureFps) * 1000.0; /* Convert Capture FPS to Capture Period in ms */
  private long lastCaptureTimeMs = System.currentTimeMillis();
  private void threadCopyImageFunc() {
    while (runCopyImageThread.get()) {
      try {
        Bitmap bitmap = Bitmap.createBitmap(view.getWidth(), view.getHeight(),
                                            Bitmap.Config.ARGB_8888);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
          latchCopyImage = new CountDownLatch(1);
          PixelCopy.request(view, bitmap, ret -> {
            if (ret == PixelCopy.SUCCESS) {
              ImageCache.getInstance().setBitmap(bitmap);
            }
            lastCaptureTimeMs = System.currentTimeMillis();
            latchCopyImage.countDown();
          }, new Handler(Looper.getMainLooper()));
          latchCopyImage.await(500, TimeUnit.MILLISECONDS);

          /* Sleep Thread to keep the capture rate */
          long currentTimeMs = System.currentTimeMillis();
          long timeDiffMs = currentTimeMs - lastCaptureTimeMs;
          long sleepPeriodMs = (long)(capturePeriodMs - timeDiffMs);
          if (sleepPeriodMs > 0) {
            Thread.sleep(sleepPeriodMs);
          }
        }
        Thread.yield();
      } catch (InterruptedException e) {
        throw new RuntimeException(e);
      }
    }
  }

  // Surface callbacks
  @Override
  public void surfaceCreated(SurfaceHolder holder) {
    if (!isInited) {

      // Preparing configuration
      this.configuration.setInitialDrawableSurface(holder.getSurface());

      // Init and run our pipeline
      nativeRCTGstInitAndRun(this.configuration);

      if (autoPlay) {
        nativeRCTGstSetPipelineState(GST_STATE_PLAYING);
      }

      if (enableFrameCache) {
        runCopyImageThread.set(true);
        thCopyImage = new Thread(() -> { threadCopyImageFunc(); });
        thCopyImage.start();
      }

      // Init done
      this.isInited = true;
    }
  }

  @Override
  public void surfaceChanged(SurfaceHolder holder, int format, int width,
                             int height) {
    nativeRCTGstSetDrawableSurface(holder.getSurface());
  }

  @Override
  public void surfaceDestroyed(SurfaceHolder holder) {
    if (enableFrameCache && thCopyImage != null) {
      runCopyImageThread.set(false);
      try {
        thCopyImage.join();
      } catch (InterruptedException e) {
        throw new RuntimeException(e);
      }
    }
  }

  // Constructor
  public RCTGstPlayerController(ReactContext context) {
    this.context = context;

    // Init GStreamer
    try {
      GStreamer.init(context);
    } catch (Exception e) {
      Toast.makeText(context, e.getMessage(), Toast.LENGTH_LONG).show();
    }

    // Display version - For simple debugging purpose
    String version = nativeRCTGstGetGStreamerInfo();
    Log.d(LOG_TAG, version);

    // Create view - surface manager interface is this class
    this.view = new EaglUIView(this.context, this);

    // Create configuration - Callbacks manager interface is this class
    this.configuration = new RCTGstConfiguration(this);
  }

  View getView() { return this.view; }

  // Manager Shared properties
  void setRctGstUri(String uri) {
    nativeRCTGstSetUri(uri);
    if (autoPlay && isInited) {
      nativeRCTGstSetPipelineState(GST_STATE_PLAYING);
    }
  }

  void setRctGstAutoPlay(boolean autoPlay) {
    this.autoPlay = autoPlay;
    if (this.autoPlay && isInited) {
      nativeRCTGstSetPipelineState(GST_STATE_PLAYING);
    }
  }

  void setRctGstAudioLevelRefreshRate(int audioLevelRefreshRate) {
    nativeRCTGstSetAudioLevelRefreshRate(audioLevelRefreshRate);
  }

  void setRctGstDebugging(boolean isDebugging) {
    nativeRCTGstSetDebugging(isDebugging);
  }

  // Manager methods
  void setRctGstState(int state) { nativeRCTGstSetPipelineState(state); }

  // External C Libraries
  static {
    System.loadLibrary("gstreamer_android");
    System.loadLibrary("rctgstplayer");
  }
}
