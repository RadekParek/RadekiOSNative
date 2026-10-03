package org.radekiosnative.recompiler;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.widget.Toast;
import org.json.JSONObject;

public final class CrashDialog {
    private CrashDialog() {}
    public static void show(Activity activity,String game,JSONObject result) {
        String details = CrashLog.format(game,result);
        String body = game + "\n" + CrashLog.summarize(result) + "\nSubsystem: " +
                result.optString("subsystem", "unknown") + "\nGuest address: " +
                result.optString("faultPc", "unknown") + "\nLast symbol: " +
                result.optString("trapSymbol", "unknown");
        new AlertDialog.Builder(activity).setTitle("Game crashed").setMessage(body)
            .setPositiveButton("VIEW DETAILS",(d,w)->new AlertDialog.Builder(activity)
                .setTitle("Crash details").setMessage(details).setPositiveButton("CLOSE",null).show())
            .setNeutralButton("COPY LOG",(d,w)->{
                ClipboardManager cb = (ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE);
                cb.setPrimaryClip(ClipData.newPlainText("RadekiOSNative crash",details));
                Toast.makeText(activity,"Crash log copied",Toast.LENGTH_SHORT).show();
            }).setNegativeButton("CLOSE",null).show();
    }
}
