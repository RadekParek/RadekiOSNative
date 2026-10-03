package org.radekiosnative.recompiler;

import android.content.Context;
import android.util.AtomicFile;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Persistent library: a corrupt file is preserved as .bad and never silently read as valid. */
public final class Library {
    private final File file;
    private final List<JSONObject> entries = new ArrayList<>();
    public Library(Context context) {
        file = new File(context.getFilesDir(), "library.json");
        if (!file.exists()) return;
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[(int)Math.min(file.length(), 8 * 1024 * 1024)];
            if (in.read(bytes) != bytes.length || file.length() > bytes.length) throw new Exception("truncated library");
            JSONArray list = new JSONArray(new String(bytes, StandardCharsets.UTF_8));
            for (int i = 0; i < list.length(); i++) entries.add(list.getJSONObject(i));
        } catch (Exception e) {
            entries.clear();
            File bad = new File(file.getParentFile(), "library.json.bad");
            if (bad.exists()) bad.delete();
            file.renameTo(bad);
        }
    }
    public synchronized List<JSONObject> entries() { return new ArrayList<>(entries); }
    public synchronized void upsert(JSONObject item) throws Exception {
        String id = item.getString("bundleId"), version = item.optString("version", "0");
        if (id.isEmpty()) throw new Exception("missing bundle ID");
        for (int i=0;i<entries.size();i++) if (id.equals(entries.get(i).optString("bundleId")) &&
                version.equals(entries.get(i).optString("version"))) { entries.set(i,item);save();return; }
        // Bundles live at games/<bundleId>/bundle.app; do not retain stale cards after an upgrade.
        entries.removeIf(old -> id.equals(old.optString("bundleId")));
        entries.add(item);save();
    }
    public synchronized void remove(String bundleId) throws Exception {
        entries.removeIf(old -> bundleId.equals(old.optString("bundleId")));
        save();
    }
    private void save() throws Exception {
        JSONArray list = new JSONArray();for (JSONObject e:entries) list.put(e);
        AtomicFile atomic = new AtomicFile(file);
        FileOutputStream stream = null;
        try {
            stream = atomic.startWrite();
            stream.write(list.toString().getBytes(StandardCharsets.UTF_8));
            atomic.finishWrite(stream);
        } catch (Exception e) {
            if (stream != null) atomic.failWrite(stream);
            throw e;
        }
    }
}
