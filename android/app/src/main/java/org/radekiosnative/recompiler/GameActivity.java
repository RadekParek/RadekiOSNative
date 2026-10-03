package org.radekiosnative.recompiler;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

// Full-screen SurfaceView host for the guest EGL/OpenGL ES frame loop. This Activity acquires
// an ANativeWindow from the SurfaceHolder, passes it to Native.bindSurface (which calls
// eglCreateWindowSurface so the guest's presentRenderbuffer -> eglSwapBuffers draws to the
// screen), and forwards Android MotionEvents (ACTION_DOWN/MOVE/UP) as iOS-style UITouch events.
public class GameActivity extends Activity {
    public static final String EXTRA_PATH = "path";
    public static final String EXTRA_GAME = "game";
    public static final String EXTRA_COMPAT = "compat";
    public static final String EXTRA_TRACE = "trace";

    private SurfaceView surfaceView;
    private Thread guestThread;
    private volatile boolean running = false;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON |
                             WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
        FrameLayout root = new FrameLayout(this);
        surfaceView = new SurfaceView(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);

        final String path = getIntent().getStringExtra(EXTRA_PATH);
        final String game = getIntent().getStringExtra(EXTRA_GAME);
        final boolean compat = getIntent().getBooleanExtra(EXTRA_COMPAT, true);
        final boolean trace = getIntent().getBooleanExtra(EXTRA_TRACE, false);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                android.util.Log.i("RadekiOSNative", "Surface created: " +
                        holder.getSurfaceFrame().width() + "x" + holder.getSurfaceFrame().height());
                Native.bindSurface(holder.getSurface(),
                        holder.getSurfaceFrame().width(),
                        holder.getSurfaceFrame().height());
                startGuest(path, game, compat, trace);
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int fmt, int w, int h) {
                Native.surfaceChanged(holder.getSurface(), w, h);
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                Native.surfaceDestroyed();
                waitForGuest();
            }
        });

        surfaceView.setOnTouchListener((v, ev) -> {
            int action = ev.getActionMasked();
            // Map Android action constants directly: 0=DOWN, 1=UP, 2=MOVE, 5/6 cancel/up ignored.
            int mapped;
            switch (action) {
                case MotionEvent.ACTION_DOWN: mapped = 0; break;
                case MotionEvent.ACTION_UP:   mapped = 1; break;
                case MotionEvent.ACTION_MOVE: mapped = 2; break;
                case MotionEvent.ACTION_CANCEL: mapped = 3; break;
                default: return true;
            }
            // Forward the primary pointer; MCPE 0.10.4 uses single-touch primarily.
            int idx = ev.getActionIndex();
            Native.touchEvent(mapped, ev.getX(idx), ev.getY(idx()), ev.getEventTime());
            return true;
        });
    }

    private void startGuest(String path, String game, boolean compat, boolean trace) {
        if (running) return;
        running = true;
        guestThread = new Thread(() -> {
            JSONObject result;
            try {
                String json;
                if (path == null) json = Native.selfTest();
                else json = Native.run(path, compat, trace);
                result = new JSONObject(json);
            } catch (Throwable t) {
                result = new JSONObject();
                try { result.put("error", t.toString()); } catch (Exception ignored) {}
            }
            try {
                if (result.optBoolean("crashed") || !result.optString("error").isEmpty())
                    CrashLog.write(GameActivity.this, game == null ? "Guest" : game, result);
                File file = new File(getFilesDir(), "result.json");
                try (FileOutputStream out = new FileOutputStream(file)) {
                    out.write(result.toString().getBytes(StandardCharsets.UTF_8));
                    out.getFD().sync();
                }
            } catch (Exception e) {
                android.util.Log.e("RadekiOSNative", "Could not persist guest result", e);
            }
            running = false;
            finish();
        }, "RadekiGuest");
        guestThread.start();
    }

    private void waitForGuest() {
        Native.requestExit(0);
        Thread t = guestThread;
        if (t != null) {
            try { t.join(1500); } catch (InterruptedException ignored) {}
        }
    }

    @Override protected void onPause() {
        super.onPause();
    }
    @Override protected void onDestroy() {
        Native.requestExit(0);
        waitForGuest();
        super.onDestroy();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override public void onBackPressed() {
        Native.requestExit(0);
        super.onBackPressed();
    }
}
