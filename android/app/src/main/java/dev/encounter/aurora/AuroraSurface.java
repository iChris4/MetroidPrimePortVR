package dev.encounter.aurora;

import android.content.Context;
import android.os.Build;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

public class AuroraSurface extends SDLSurface {
    private static native void nativeSetSurfaceReady(boolean ready);
    private static native void nativeSetSurfaceChanging(boolean changing);
    private static native boolean nativeWaitSurfaceReleased();

    public AuroraSurface(Context context) {
        super(context);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);
        super.surfaceCreated(holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);
        // The window must be unused once this returns; let the renderer drop its
        // swapchain first (bounded, so a busy main thread can't cause an ANR).
        nativeWaitSurfaceReleased();
        super.surfaceDestroyed(holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // The native window survives surfaceChanged, so keep the renderer's surface:
        // a second one for the same window fails while the old swapchain is connected.
        nativeSetSurfaceChanging(true);
        super.surfaceChanged(holder, format, width, height);
        nativeSetSurfaceReady(mIsSurfaceReady);
        nativeSetSurfaceChanging(false);
        requestHighestFrameRate(holder.getSurface());
    }

    // The activity's preferredDisplayModeId only votes on its own window. A layer
    // without a vote of its own gets the game default rate (60 on HyperOS, since
    // the manifest says isGame), and some HyperOS builds go by the layer that
    // posts frames, so this one asks for the panel's top rate too (issue #159).
    private void requestHighestFrameRate(Surface surface) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || surface == null || !surface.isValid()) {
            return;
        }
        Display display = getDisplay();
        if (display == null) {
            return;
        }
        float best = 0.0f;
        for (Display.Mode mode : display.getSupportedModes()) {
            best = Math.max(best, mode.getRefreshRate());
        }
        if (best > 0.0f) {
            try {
                surface.setFrameRate(best, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            } catch (IllegalArgumentException | IllegalStateException e) {
                // A surface released mid-call; the next surfaceChanged asks again.
            }
        }
    }
}
