package org.radekiosnative.recompiler;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

// Optional isolated guest runner. It uses the same durable run log as GameActivity and the
// launcher, so background runs remain inspectable after this process exits.
public class GuestService extends Service {
    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        final String path = intent == null ? null : intent.getStringExtra("path");
        final String game = intent == null ? "Self-test" : intent.getStringExtra("game");
        final String architecture = intent == null ? "ARM64 self-test" : intent.getStringExtra("architecture");
        final boolean compatibilityFallbacks = intent == null || intent.getBooleanExtra("arm64CompatibilityFallbacks", true);
        final boolean traceMissingApis = intent != null && intent.getBooleanExtra("arm64TraceMissingApis", false);
        final boolean realtimeLogging = AppSettings.useRealtimeLogging(this);

        new Thread(() -> {
            File runLog = null;
            JSONObject result;
            try {
                runLog = RunHistory.begin(this, game, architecture, realtimeLogging,
                        compatibilityFallbacks, traceMissingApis);
                Native.beginRunLog(runLog.getAbsolutePath(), realtimeLogging);
                result = new JSONObject(path == null ? Native.selfTest()
                        : Native.run(path, compatibilityFallbacks, traceMissingApis));
            } catch (Throwable t) {
                result = new JSONObject();
                try { result.put("error", t.toString()); } catch (Exception ignored) {}
                RunHistory.append(runLog, "[java] Guest invocation threw: " + t + "\n");
            }
            try {
                RunHistory.finish(runLog, result);
                if (result.optBoolean("crashed") || !result.optString("error").isEmpty())
                    CrashLog.write(this, game == null ? "Guest" : game, result);
                File resultFile = new File(getFilesDir(), "result.json");
                try (FileOutputStream out = new FileOutputStream(resultFile)) {
                    out.write(result.toString().getBytes(StandardCharsets.UTF_8));
                    out.getFD().sync();
                }
            } catch (Exception e) {
                android.util.Log.e("RadekiOSNative", "Could not persist guest result", e);
                RunHistory.append(runLog, "[java] Could not persist final result: " + e + "\n");
            } finally {
                try { Native.endRunLog(); } catch (Throwable ignored) {}
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        }, "RadekiGuestService").start();
        return START_NOT_STICKY;
    }
}
