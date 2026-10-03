package org.radekiosnative.recompiler;

import android.view.Surface;

public class Native {
    static { System.loadLibrary("radekijni"); }
    public static native String analyze(String path);
    public static native String run(String path, boolean compatibilityFallbacks, boolean traceMissingApis);
    public static native String selfTest();
    public static native String inspectBundle(String appPath, String iconPath);
    // EGL / Surface binding (drives eglCreateWindowSurface + eglSwapBuffers for game rendering).
    public static native void bindSurface(Surface surface, int width, int height);
    public static native void surfaceChanged(Surface surface, int width, int height);
    public static native void surfaceDestroyed();
    // Touch forwarding: action is 0=DOWN, 1=UP, 2=MOVE (matches MotionEvent.getActionMasked).
    public static native void touchEvent(int action, float x, float y, long timeMs);
    // Ask UIApplicationMain's event loop to exit with the given code (used on teardown).
    public static native void requestExit(int code);
}
