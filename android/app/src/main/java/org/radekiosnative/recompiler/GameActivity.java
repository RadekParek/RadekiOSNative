package org.radekiosnative.recompiler;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

// Full-screen SurfaceView host for the guest EGL/OpenGL ES frame loop. A small diagnostics panel
// appears only when a launch has not produced a visible EGL swap, so a stuck black screen is not
// an empty, uninformative activity. Every run writes to private storage before Surface startup.
public class GameActivity extends Activity {
    public static final String EXTRA_PATH = "path";
    public static final String EXTRA_GAME = "game";
    public static final String EXTRA_COMPAT = "compat";
    public static final String EXTRA_TRACE = "trace";
    public static final String EXTRA_ARCH = "architecture";

    private static final long BLACK_SCREEN_GRACE_MS = 7000;
    private static final int BG = Color.rgb(10, 14, 23);
    private static final int TEXT = Color.rgb(239, 242, 250);
    private static final int DIM = Color.rgb(174, 184, 200);
    private static final int ACCENT = Color.rgb(111, 143, 255);

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private SurfaceView surfaceView;
    private Thread guestThread;
    private volatile boolean running = false;
    private long launchStartedAt;
    private String gameName = "Guest";
    private boolean realtimeLogging;
    private File runLogFile;
    private LinearLayout diagnosticsPanel;
    private TextView diagnosticsText;
    private final Runnable diagnosticsPoll = new Runnable() {
        @Override public void run() {
            updateBlackScreenDiagnostics();
            if (!isFinishing() && !isDestroyed()) uiHandler.postDelayed(this, 1200);
        }
    };

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

        final String path = getIntent().getStringExtra(EXTRA_PATH);
        gameName = getIntent().getStringExtra(EXTRA_GAME);
        if (gameName == null || gameName.trim().isEmpty()) gameName = "Guest";
        final boolean compatibilityFallbacks = getIntent().getBooleanExtra(EXTRA_COMPAT, true);
        final boolean traceMissingApis = getIntent().getBooleanExtra(EXTRA_TRACE, false);
        realtimeLogging = AppSettings.useRealtimeLogging(this);
        final String architecture = getIntent().getStringExtra(EXTRA_ARCH);
        launchStartedAt = System.currentTimeMillis();

        try {
            runLogFile = RunHistory.begin(this, gameName,
                    architecture == null ? (path == null ? (BuildConfig.FLAVOR != null && BuildConfig.FLAVOR.contains("arm32") ? "ARMv7/AArch32 self-test" : "ARM64/AArch64 self-test") : "not detected") : architecture,
                    realtimeLogging, compatibilityFallbacks, traceMissingApis);
            Native.beginRunLog(runLogFile.getAbsolutePath(), realtimeLogging);
            RunHistory.append(runLogFile, "[java] Launch activity created; waiting for Android Surface.\n");
        } catch (Throwable e) {
            android.util.Log.e("RadekiOSNative", "Could not initialize the run log", e);
            RunHistory.append(runLogFile, "[java] Could not initialize native run logging: " + e + "\n");
        }

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new SurfaceView(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(-1, -1));
        createDiagnosticsPanel(root);
        setContentView(root);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                int width = holder.getSurfaceFrame().width();
                int height = holder.getSurfaceFrame().height();
                android.util.Log.i("RadekiOSNative", "Surface created: " + width + "x" + height);
                RunHistory.append(runLogFile, "[java] Android Surface created: " + width + "x" + height + "\n");
                try {
                    Native.bindSurface(holder.getSurface(), width, height);
                } catch (Throwable e) {
                    RunHistory.append(runLogFile, "[java] Surface binding failed: " + e + "\n");
                    android.util.Log.e("RadekiOSNative", "Could not bind Android Surface", e);
                }
                startGuest(path, compatibilityFallbacks, traceMissingApis);
            }

            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                try {
                    Native.surfaceChanged(holder.getSurface(), width, height);
                } catch (Throwable e) {
                    RunHistory.append(runLogFile, "[java] Surface resize failed: " + e + "\n");
                }
            }

            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                RunHistory.append(runLogFile, "[java] Android Surface destroyed.\n");
                try { Native.surfaceDestroyed(); } catch (Throwable ignored) {}
                waitForGuest();
            }
        });

        surfaceView.setOnTouchListener((v, ev) -> {
            int action = ev.getActionMasked();
            int mapped;
            switch (action) {
                case MotionEvent.ACTION_DOWN: mapped = 0; break;
                case MotionEvent.ACTION_UP:   mapped = 1; break;
                case MotionEvent.ACTION_MOVE: mapped = 2; break;
                case MotionEvent.ACTION_CANCEL: mapped = 3; break;
                default: return true;
            }
            // The currently bridged game input path is primary-pointer/single-touch.
            int index = ev.getActionIndex();
            Native.touchEvent(mapped, ev.getX(index), ev.getY(index), ev.getEventTime());
            return true;
        });
        uiHandler.postDelayed(diagnosticsPoll, 1000);
    }

    private void createDiagnosticsPanel(FrameLayout root) {
        diagnosticsPanel = new LinearLayout(this);
        diagnosticsPanel.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        diagnosticsPanel.setPadding(pad, pad, pad, pad);
        GradientDrawable card = new GradientDrawable();
        card.setColor(Color.rgb(20, 27, 41));
        card.setCornerRadius(dp(18));
        card.setStroke(dp(1), Color.rgb(70, 88, 124));
        diagnosticsPanel.setBackground(card);
        diagnosticsPanel.setElevation(dp(8));
        diagnosticsPanel.setVisibility(View.GONE);

        TextView heading = new TextView(this);
        heading.setText("BLACK-SCREEN CHECK");
        heading.setTextColor(ACCENT);
        heading.setTextSize(12);
        heading.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        diagnosticsPanel.addView(heading);

        diagnosticsText = new TextView(this);
        diagnosticsText.setTextColor(TEXT);
        diagnosticsText.setTextSize(14);
        diagnosticsText.setPadding(0, dp(8), 0, dp(8));
        diagnosticsPanel.addView(diagnosticsText);

        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        Button viewLog = new Button(this);
        viewLog.setText("VIEW RUN LOG");
        viewLog.setTextColor(ACCENT);
        viewLog.setOnClickListener(v -> RunLogViewer.show(this, runLogFile));
        actions.addView(viewLog);
        Button exit = new Button(this);
        exit.setText("EXIT");
        exit.setOnClickListener(v -> {
            try { Native.requestExit(0); } catch (Throwable ignored) {}
            finish();
        });
        actions.addView(exit);
        diagnosticsPanel.addView(actions);

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        params.setMargins(dp(14), dp(14), dp(14), 0);
        root.addView(diagnosticsPanel, params);
    }

    private void updateBlackScreenDiagnostics() {
        if (!running || System.currentTimeMillis() - launchStartedAt < BLACK_SCREEN_GRACE_MS) return;
        try {
            JSONObject status = new JSONObject(Native.liveGraphicsStatus());
            boolean nativeWindow = status.optBoolean("nativeWindowBound");
            boolean eglReady = status.optBoolean("eglReady");
            boolean windowSurface = status.optBoolean("windowSurface");
            boolean loop = status.optBoolean("eventLoopActive");
            long swaps = status.optLong("successfulSwaps");
            if (nativeWindow && eglReady && windowSurface && loop && swaps > 0) {
                diagnosticsPanel.setVisibility(View.GONE);
                return;
            }

            String issue;
            if (!nativeWindow) issue = "Android has not attached the game display surface yet.";
            else if (!eglReady) issue = "No usable EGL context/surface was created; graphics startup may have failed.";
            else if (!windowSurface) issue = "EGL is rendering off-screen (pbuffer), not to the Android display.";
            else if (!loop) issue = "The game has not reached its UIApplicationMain event loop.";
            else if (swaps == 0) issue = "The app loop is alive, but it has not presented a frame.";
            else issue = "The graphics bridge is active but the display has no confirmed frame.";

            String details = "Run log is saved automatically. RTLS is " +
                    (realtimeLogging ? "ON" : "OFF") + ". Use Back or Exit to stop the run.";
            String last = status.optString("lastGraphicsIssue", "");
            if (!last.isEmpty()) details += "\nLast graphics issue: " + last;
            if (!status.optBoolean("displayLinkRegistered") && loop)
                details += "\nNo CADisplayLink callback is registered.";
            diagnosticsText.setText(issue + "\n\n" + details);
            diagnosticsPanel.setVisibility(View.VISIBLE);
        } catch (Throwable e) {
            diagnosticsText.setText("The game is still running without a visible frame. A run log is being saved.\n\n" +
                    "RTLS is " + (realtimeLogging ? "ON" : "OFF") + ".");
            diagnosticsPanel.setVisibility(View.VISIBLE);
        }
    }

    private void startGuest(String path, boolean compatibilityFallbacks, boolean traceMissingApis) {
        if (running) return;
        running = true;
        launchStartedAt = System.currentTimeMillis();
        guestThread = new Thread(() -> {
            JSONObject result;
            try {
                String json = path == null ? Native.selfTest()
                                           : Native.run(path, compatibilityFallbacks, traceMissingApis);
                result = new JSONObject(json);
            } catch (Throwable t) {
                result = new JSONObject();
                try { result.put("error", t.toString()); } catch (Exception ignored) {}
                RunHistory.append(runLogFile, "[java] Guest invocation threw: " + t + "\n");
            }
            try {
                RunHistory.finish(runLogFile, result);
                if (result.optBoolean("crashed") || !result.optString("error").isEmpty())
                    CrashLog.write(GameActivity.this, gameName, result);
                File resultFile = new File(getFilesDir(), "result.json");
                try (FileOutputStream out = new FileOutputStream(resultFile)) {
                    out.write(result.toString().getBytes(StandardCharsets.UTF_8));
                    out.getFD().sync();
                }
            } catch (Exception e) {
                android.util.Log.e("RadekiOSNative", "Could not persist guest result", e);
                RunHistory.append(runLogFile, "[java] Could not persist final result: " + e + "\n");
            } finally {
                try { Native.endRunLog(); } catch (Throwable ignored) {}
                running = false;
                uiHandler.removeCallbacks(diagnosticsPoll);
                runOnUiThread(this::finish);
            }
        }, "RadekiGuest");
        guestThread.start();
    }

    private void waitForGuest() {
        try { Native.requestExit(0); } catch (Throwable ignored) {}
        Thread thread = guestThread;
        if (thread != null && thread != Thread.currentThread()) {
            try { thread.join(1800); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
        }
    }

    private int dp(int value) {
        return Math.round(getResources().getDisplayMetrics().density * value);
    }

    @Override protected void onPause() {
        super.onPause();
    }

    @Override protected void onDestroy() {
        uiHandler.removeCallbacks(diagnosticsPoll);
        try { Native.requestExit(0); } catch (Throwable ignored) {}
        waitForGuest();
        if (running) {
            RunHistory.append(runLogFile, "[java] Activity closed while guest execution was still active; " +
                    "a missing final result marks this run as interrupted.\n");
        }
        try { Native.endRunLog(); } catch (Throwable ignored) {}
        super.onDestroy();
        // This Activity and native guest live in :guest; never leave a stuck guest process behind.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override public void onBackPressed() {
        try { Native.requestExit(0); } catch (Throwable ignored) {}
        super.onBackPressed();
    }
}
