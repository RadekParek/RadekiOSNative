package org.radekiosnative.recompiler;

import android.content.Context;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class CrashLog {
    private CrashLog() {}
    public static String summarize(JSONObject result) {
        String reason = result.optString("reason");
        return reason.isEmpty() ? result.optString("error", "guest process ended without a result") : reason;
    }
    public static String format(String game,JSONObject result) {
        return "Game: " + game + "\nReason: " + summarize(result) + "\nSubsystem: " +
                result.optString("subsystem", "unknown") + "\nGuest address: " + result.optString("faultPc", "unknown") +
                "\nLast symbol: " + result.optString("trapSymbol", "unknown") + "\n\n" + result.toString();
    }
    public static File write(Context context,String game,JSONObject result) throws Exception {
        File dir = new File(context.getFilesDir(), "crashes");
        if (!dir.exists() && !dir.mkdirs()) throw new Exception("cannot create crash directory");
        File file = new File(dir,"crash-" + System.currentTimeMillis() + ".txt");
        try (FileOutputStream stream = new FileOutputStream(file)) {
            stream.write(format(game,result).getBytes(StandardCharsets.UTF_8));stream.getFD().sync();
        }
        return file;
    }
    public static File latest(Context context) {
        File[] list = new File(context.getFilesDir(), "crashes").listFiles();
        if (list==null||list.length==0) return null;
        Arrays.sort(list,(a,b)->Long.compare(b.lastModified(),a.lastModified()));return list[0];
    }
}
