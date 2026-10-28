package com.kalyzee.rctgstplayer.utils;

import android.content.Context;
import android.view.TextureView;

/**
 * Created by asapone on 02/01/2018.
 */

// A TextureView, not a SurfaceView: its picture is drawn with the rest of the
// views, so the view's transform (a React Native scaleX: -1, say) applies to the
// video too. A SurfaceView's picture is composited apart and ignores it.
//
// Its SurfaceTexture is RGBA_8888 and the view opaque by default, so the
// swapchain gets a 32-bit window and the blend is skipped without asking.
public class EaglUIView extends TextureView {

    public EaglUIView(Context context, TextureView.SurfaceTextureListener surfaceTextureListener) {
        super(context);

        this.setSurfaceTextureListener(surfaceTextureListener);
    }
}
