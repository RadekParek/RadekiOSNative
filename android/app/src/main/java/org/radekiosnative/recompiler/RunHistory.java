package org.radekiosnative.recompiler;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Process;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/** Durable per-launch log files. Native RTLS appends to the same file while the guest is running. */
public final class RunHistory {
    private static final String PREFS = "run_history";
    private static final String LAST_VIEWED = "last_viewed_run_id";
    private static final int MAX_LOGS = 30;
    private static final int MAX_VIEW_BYTES = 512 * 1024;

    private RunHistory() {}

    public static File begin(Context context, String game, String architecture,
                             boolean realtimeLogging, boolean compatibilityFallbacks,
                             boolean traceMissingApis) throws Exception {
        File directory = directory(context);
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new Exception("cannot create run log directory");
        long now = System.currentTimeMillis();
        String id = "run-" + now + "-" + Process.myPid() + "-" + (System.nanoTime() & 0xfffffL);
        File file = new File(directory, id + ".log");
        String header = "RadekiOSNative run log\n" +
                "Run ID: " + id + "\n" +
                "Started: " + timestamp(now) + "\n" +
                "Game: " + safe(game, "Guest") + "\n" +
                "Architecture: " + safe(architecture, "not detected") + "\n" +
                "Run mode: native guest runtime\n" +
                "RTLS: " + (realtimeLogging ? "enabled" : "disabled") + "\n" +
                "Compatibility fallbacks: " + (compatibilityFallbacks ? "enabled" : "disabled") + "\n" +
                "Missing API tracing: " + (traceMissingApis ? "enabled" : "disabled") + "\n" +
                "Run status: RUNNING\n" +
                "------------------------------------------------------------\n";
        write(file, header, true);
        prune(directory);
        return file;
    }

    public static void append(File file, String text) {
        if (file == null || text == null || text.isEmpty()) return;
        try {
            write(file, text, false);
        } catch (Exception ignored) {
            // A diagnostic failure must not stop the guest or mask its actual result.
        }
    }

    public static void finish(File file, JSONObject result) {
        if (file == null) return;
        String status;
        if (result != null && result.optBoolean("crashed")) status = "CRASHED";
        else if (result != null && !result.optString("error").isEmpty()) status = "FAILED";
        else if (result != null && result.optBoolean("ran")) status = "STOPPED";
        else status = "NOT RUN";
        String summary = result == null ? "No result was returned" : CrashLog.summarize(result);
        StringBuilder report = new StringBuilder()
                .append("\nFinished: ").append(timestamp(System.currentTimeMillis())).append('\n')
                .append("Run status: ").append(status).append('\n')
                .append("Summary: ").append(safe(summary, "No summary")).append('\n')
                .append("Result details:\n");
        if (result != null) {
            try {
                report.append(result.toString(2));
            } catch (Exception e) {
                report.append(result.toString());
            }
        }
        report.append("\n");
        try {
            write(file, report.toString(), true);
        } catch (Exception ignored) {
            // The native log is already on disk; final JSON is best-effort if storage is full.
        }
    }

    public static File latest(Context context) {
        List<File> files = recent(context, 1);
        return files.isEmpty() ? null : files.get(0);
    }

    public static List<File> recent(Context context, int maximum) {
        File[] files = directory(context).listFiles((dir, name) -> name.startsWith("run-") && name.endsWith(".log"));
        if (files == null || files.length == 0 || maximum <= 0) return new ArrayList<>();
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return new ArrayList<>(Arrays.asList(files).subList(0, Math.min(maximum, files.length)));
    }

    public static boolean shouldShowLatest(Context context) {
        File file = latest(context);
        if (file == null) return false;
        String viewed = preferences(context).getString(LAST_VIEWED, "");
        return !file.getName().equals(viewed);
    }

    public static void markViewed(Context context, File file) {
        if (file != null) preferences(context).edit().putString(LAST_VIEWED, file.getName()).apply();
    }

    public static String title(File file) {
        String value = readLineValue(file, "Game: ");
        return value.isEmpty() ? "Run log" : value;
    }

    public static String status(File file) {
        String status = "RUNNING / interrupted";
        if (file == null) return status;
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[8192];
            String pending = "";
            int n;
            while ((n = in.read(buffer)) != -1) {
                String chunk = pending + new String(buffer, 0, n, StandardCharsets.UTF_8);
                String[] lines = chunk.split("\\n", -1);
                for (int i = 0; i + 1 < lines.length; i++)
                    if (lines[i].startsWith("Run status: ")) status = lines[i].substring("Run status: ".length());
                pending = lines[lines.length - 1];
            }
            if (pending.startsWith("Run status: ")) status = pending.substring("Run status: ".length());
        } catch (Exception ignored) {}
        return status;
    }

    public static String read(File file) {
        if (file == null || !file.isFile()) return "No run log is available.";
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long length = input.length();
            long start = Math.max(0, length - MAX_VIEW_BYTES);
            input.seek(start);
            byte[] bytes = new byte[(int) (length - start)];
            input.readFully(bytes);
            String content = new String(bytes, StandardCharsets.UTF_8);
            return (start == 0 ? "" : "[Showing the most recent 512 KB of this log. Earlier lines were omitted.]\n\n") + content;
        } catch (Exception e) {
            return "Could not read this run log: " + e.getMessage();
        }
    }

    private static String readLineValue(File file, String prefix) {
        if (file == null) return "";
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[(int) Math.min(file.length(), 8192)];
            int count = in.read(bytes);
            if (count <= 0) return "";
            for (String line : new String(bytes, 0, count, StandardCharsets.UTF_8).split("\\n"))
                if (line.startsWith(prefix)) return line.substring(prefix.length());
        } catch (Exception ignored) {}
        return "";
    }

    private static File directory(Context context) {
        return new File(context.getApplicationContext().getFilesDir(), "run_logs");
    }

    private static SharedPreferences preferences(Context context) {
        return context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static String timestamp(long milliseconds) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS z", Locale.getDefault()).format(new Date(milliseconds));
    }

    private static String safe(String value, String fallback) {
        if (value == null || value.trim().isEmpty()) return fallback;
        return value.replace('\n', ' ').replace('\r', ' ');
    }

    private static void write(File file, String text, boolean sync) throws Exception {
        try (FileOutputStream out = new FileOutputStream(file, true)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            if (sync) out.getFD().sync();
        }
    }

    private static void prune(File directory) {
        File[] files = directory.listFiles((dir, name) -> name.startsWith("run-") && name.endsWith(".log"));
        if (files == null || files.length <= MAX_LOGS) return;
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        for (int i = MAX_LOGS; i < files.length; i++) files[i].delete();
    }
}
