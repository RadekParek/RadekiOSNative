package org.radekiosnative.recompiler;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/** Scrollable, copyable view of a saved or currently-running run log. */
public final class RunLogViewer {
    private RunLogViewer() {}

    public static void show(Activity activity, File file) {
        if (file == null) {
            Toast.makeText(activity, "No run log is available yet.", Toast.LENGTH_SHORT).show();
            return;
        }
        String contents = RunHistory.read(file);
        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(false);
        TextView text = new TextView(activity);
        text.setText(contents);
        text.setTextSize(12);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextIsSelectable(true);
        int padding = Math.round(16 * activity.getResources().getDisplayMetrics().density);
        text.setPadding(padding, padding, padding, padding);
        scroll.addView(text, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                         ViewGroup.LayoutParams.WRAP_CONTENT));

        AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle(RunHistory.title(file) + " · " + RunHistory.status(file))
                .setView(scroll)
                .setPositiveButton("COPY LOG", (d, which) -> {
                    ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                    if (clipboard != null) clipboard.setPrimaryClip(ClipData.newPlainText("RadekiOSNative run log", contents));
                    Toast.makeText(activity, "Run log copied", Toast.LENGTH_SHORT).show();
                })
                .setNeutralButton("SHARE", (d, which) -> {
                    Intent share = new Intent(Intent.ACTION_SEND);
                    share.setType("text/plain");
                    share.putExtra(Intent.EXTRA_SUBJECT, RunHistory.title(file) + " run log");
                    share.putExtra(Intent.EXTRA_TEXT, contents);
                    activity.startActivity(Intent.createChooser(share, "Share run log"));
                })
                .setNegativeButton("CLOSE", null)
                .create();
        dialog.show();
        dialog.getWindow().setLayout(
                Math.min((int) (activity.getResources().getDisplayMetrics().widthPixels * 0.94f),
                         activity.getResources().getDisplayMetrics().widthPixels),
                (int) (activity.getResources().getDisplayMetrics().heightPixels * 0.78f));
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
    }
}
