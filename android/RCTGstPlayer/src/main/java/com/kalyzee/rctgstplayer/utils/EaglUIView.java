package com.kalyzee.rctgstplayer.utils;

import android.content.Context;
import android.graphics.PixelFormat;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * Created by asapone on 02/01/2018.
 */

public class EaglUIView extends SurfaceView {

    public Surface getHandle() {
        return this.getHolder().getSurface();
    }

    public EaglUIView(Context context, SurfaceHolder.Callback sufaceHolderManager) {
        super(context);

        // vulkansink presents 8-bit-per-channel images, and SurfaceView asks for
        // an RGB_565 window by default. Request RGBX_8888 instead: 32-bit so the
        // swapchain isn't handed a 16-bit window to band the video into, and
        // opaque (unlike RGBA_8888) so the compositor keeps skipping the blend.
        this.getHolder().setFormat(PixelFormat.RGBX_8888);

        this.getHolder().addCallback(sufaceHolderManager);
    }
}
