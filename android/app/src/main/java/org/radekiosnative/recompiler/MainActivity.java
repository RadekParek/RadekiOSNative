package org.radekiosnative.recompiler;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class MainActivity extends Activity {
    private static final int PICK=1;
    private static final int BG=Color.rgb(5,6,8),SURFACE=Color.rgb(18,22,28);
    private static final int TEXT=Color.rgb(232,234,237),DIM=Color.rgb(154,163,173),ACCENT=Color.rgb(79,140,255);
    private final List<JSONObject> games=new ArrayList<>();
    private LinearLayout cards;
    private Library library;
    private int dp(int n){return Math.round(getResources().getDisplayMetrics().density*n);}
    private TextView label(String text,int size,int color){TextView v=new TextView(this);v.setText(text);v.setTextSize(size);v.setTextColor(color);return v;}
    @Override protected void onCreate(Bundle state){
        super.onCreate(state);library=new Library(this);
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setBackgroundColor(BG);
        LinearLayout root=new LinearLayout(this);root.setPadding(dp(20),dp(38),dp(20),dp(20));root.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(root);
        LinearLayout appBar=new LinearLayout(this);appBar.setOrientation(LinearLayout.HORIZONTAL);
        appBar.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout brand=new LinearLayout(this);brand.setOrientation(LinearLayout.VERTICAL);
        brand.addView(label("RadekiOSNative",25,TEXT));
        brand.addView(label("Native iOS compatibility runtime",13,DIM));
        appBar.addView(brand,new LinearLayout.LayoutParams(0,-2,1));
        Button settings=new Button(this);settings.setText("⚙ SETTINGS");settings.setTextColor(Color.WHITE);
        settings.setContentDescription("Runtime settings");
        settings.setBackgroundTintList(ColorStateList.valueOf(ACCENT));
        settings.setOnClickListener(v->showSettings());
        appBar.addView(settings,new LinearLayout.LayoutParams(-2,dp(44)));
        root.addView(appBar);
        LinearLayout header=new LinearLayout(this);header.setPadding(0,dp(32),0,dp(12));
        TextView title=label("LIBRARY",13,DIM);header.addView(title,new LinearLayout.LayoutParams(0,dp(48),1));
        Button add=new Button(this);add.setText("[ + ADD IPA ]");add.setTextColor(ACCENT);
        add.setOnClickListener(v->{Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");startActivityForResult(i,PICK);});header.addView(add);
        root.addView(header);cards=new LinearLayout(this);cards.setOrientation(LinearLayout.VERTICAL);root.addView(cards);
        Button self=new Button(this);self.setText("Run built-in ARM64 self-test");
        self.setOnClickListener(v->launch(null,"Self-test"));root.addView(self);
        setContentView(scroll);refresh();
    }
    private void showSettings(){
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(false);
        LinearLayout content=new LinearLayout(this);content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(4),dp(4),dp(4),dp(4));scroll.addView(content);

        addSettingsHeading(content,"ARM64 SETTINGS","Active native runtime · arm64-v8a");
        addSettingsToggle(content,"Compatibility fallbacks",
            "Uses limited startup shims and safe defaults for missing imports, not a UIKit implementation. Unsupported view, event-loop, graphics, and Foundation APIs can still stop a game before its boot screen.",
            AppSettings.useArm64CompatibilityFallbacks(this),
            (button,enabled)->AppSettings.setArm64CompatibilityFallbacks(this,enabled));
        addSettingsToggle(content,"Trace missing API calls",
            "Records the first and every 4,096th call to an unresolved import. Off by default to reduce guest overhead; enable it when diagnosing a game.",
            AppSettings.traceArm64MissingApis(this),
            (button,enabled)->AppSettings.setArm64TraceMissingApis(this,enabled));

        addSettingsHeading(content,"ARM32 SETTINGS","Not available in this APK");
        addSettingsInfo(content,"ARMv7 / AArch32 execution is not included",
            "This build packages only the arm64-v8a runtime. ARM32-only bundles may be inspected, but cannot be launched. No ARM32 controls are shown because there is no ARM32 engine to configure.");

        new AlertDialog.Builder(this).setTitle("Runtime settings").setView(scroll)
            .setPositiveButton("DONE",null).show();
    }
    private void addSettingsHeading(LinearLayout parent,String title,String subtitle){
        TextView heading=label(title,13,ACCENT);heading.setPadding(dp(8),dp(14),dp(8),0);
        parent.addView(heading);
        TextView sub=label(subtitle,12,DIM);sub.setPadding(dp(8),dp(2),dp(8),dp(8));
        parent.addView(sub);
    }
    private void addSettingsToggle(LinearLayout parent,String title,String description,boolean enabled,
                                   CompoundButton.OnCheckedChangeListener listener){
        LinearLayout row=new LinearLayout(this);row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(dp(12),dp(10),dp(8),dp(10));
        row.setBackgroundColor(SURFACE);
        LinearLayout copy=new LinearLayout(this);copy.setOrientation(LinearLayout.VERTICAL);
        copy.addView(label(title,15,TEXT));
        TextView detail=label(description,12,DIM);detail.setPadding(0,dp(3),dp(8),0);
        copy.addView(detail);
        row.addView(copy,new LinearLayout.LayoutParams(0,-2,1));
        Switch toggle=new Switch(this);toggle.setChecked(enabled);toggle.setOnCheckedChangeListener(listener);
        row.addView(toggle,new LinearLayout.LayoutParams(-2,-2));
        LinearLayout.LayoutParams params=new LinearLayout.LayoutParams(-1,-2);
        params.bottomMargin=dp(8);parent.addView(row,params);
    }
    private void addSettingsInfo(LinearLayout parent,String title,String description){
        LinearLayout card=new LinearLayout(this);card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(12),dp(12),dp(12),dp(12));card.setBackgroundColor(SURFACE);
        card.addView(label(title,15,TEXT));
        TextView detail=label(description,12,DIM);detail.setPadding(0,dp(4),0,0);card.addView(detail);
        parent.addView(card,new LinearLayout.LayoutParams(-1,-2));
    }
    private void refresh(){

        games.clear();games.addAll(library.entries());cards.removeAllViews();
        if(games.isEmpty())cards.addView(label("No imported IPA bundles yet.",15,DIM));
        for(JSONObject g:games){
            LinearLayout card=new LinearLayout(this);card.setOrientation(LinearLayout.VERTICAL);card.setPadding(dp(14),dp(14),dp(14),dp(14));
            card.setBackgroundColor(SURFACE);
            LinearLayout row=new LinearLayout(this);row.setOrientation(LinearLayout.HORIZONTAL);
            ImageView icon=new ImageView(this);icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
            Bitmap bitmap=BitmapFactory.decodeFile(g.optString("iconPath"));
            if(bitmap!=null)icon.setImageBitmap(bitmap);else icon.setBackgroundColor(BG); // no fabricated app icon
            row.addView(icon,new LinearLayout.LayoutParams(dp(72),dp(72)));
            LinearLayout text=new LinearLayout(this);text.setPadding(dp(12),0,0,0);text.setOrientation(LinearLayout.VERTICAL);
            TextView name=label(g.optString("name","Unnamed bundle"),18,TEXT);name.setSingleLine(true);name.setEllipsize(TextUtils.TruncateAt.END);
            text.addView(name);text.addView(label(g.optString("engines", "Architecture unknown"),13,DIM));
            String state=g.optString("state","ANALYZED");
            text.addView(label("Runtime: "+state,14,state.equals("RUNTIME_READY")?Color.rgb(95,210,130):ACCENT));
            row.addView(text,new LinearLayout.LayoutParams(0,dp(72),1));card.addView(row);
            card.addView(label(g.optString("summary"),12,DIM));
            LinearLayout actions=new LinearLayout(this);actions.setOrientation(LinearLayout.HORIZONTAL);
            Button launch=new Button(this);launch.setText("LAUNCH →");launch.setTextColor(ACCENT);
            launch.setOnClickListener(v->new AlertDialog.Builder(this).setTitle(g.optString("name"))
                .setMessage(g.optString("summary")+"\n\nThis does not mean the game is launchable. Attempt execution anyway?")
                .setPositiveButton("Attempt",(d,w)->launch(g.optString("executablePath"),g.optString("name")))
                .setNegativeButton("Cancel",null).show());
            actions.addView(launch,new LinearLayout.LayoutParams(0,-2,1));
            Button remove=new Button(this);remove.setText("REMOVE");remove.setTextColor(DIM);
            remove.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Remove "+g.optString("name")+"?")
                .setMessage("Delete imported bundle from library?")
                .setPositiveButton("Remove",(d,w)->{
                    try{
                        String id=g.optString("bundleId");
                        String dirName=sanitizeDirName(id);
                        deleteTree(new File(new File(getFilesDir(),"games"),dirName));
                        deleteTree(new File(new File(getFilesDir(),"games"),id));
                        library.remove(id);
                    }catch(Exception ignored){}
                    refresh();
                }).setNegativeButton("Cancel",null).show());
            actions.addView(remove);
            card.addView(actions);
            LinearLayout.LayoutParams params=new LinearLayout.LayoutParams(-1,-2);params.bottomMargin=dp(12);cards.addView(card,params);
        }
    }
    @Override protected void onActivityResult(int req,int res,Intent data){
        super.onActivityResult(req,res,data);
        if(req!=PICK||res!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();Toast.makeText(this,"Importing IPA...",Toast.LENGTH_SHORT).show();
        new Thread(()->{
            String message;
            try{message="Imported "+importIpa(uri);}catch(Exception e){message="Import failed: "+e.getMessage();}
            final String finalMessage=message;runOnUiThread(()->{Toast.makeText(this,finalMessage,Toast.LENGTH_LONG).show();refresh();});
        }).start();
    }
    private static void deleteTree(File file){
        if(file==null||!file.exists())return;
        File[] children=file.listFiles();if(children!=null)for(File c:children)deleteTree(c);file.delete();
    }
    private static String normalizeEntryName(String raw){
        String s=raw.replace('\\','/');
        while(s.startsWith("./")||s.startsWith("/"))s=s.substring(s.startsWith("./")?2:1);
        return s;
    }
    private static String sanitizeDirName(String id){
        if(id==null||id.isEmpty())return "bundle";
        String cleaned=id.replaceAll("[^A-Za-z0-9._-]","_");
        if(cleaned.isEmpty()||cleaned.equals(".")||cleaned.equals(".."))cleaned="bundle";
        return cleaned.length()>180?cleaned.substring(0,180):cleaned;
    }
    private String importIpa(Uri uri)throws Exception{
        File staging=new File(getFilesDir(),"staging-"+System.nanoTime());
        if(!staging.mkdirs())throw new Exception("cannot create staging directory");
        try{
            File archive=new File(staging,"import.ipa");long total=0;
            try(InputStream in=getContentResolver().openInputStream(uri);OutputStream out=new FileOutputStream(archive)){
                if(in==null)throw new Exception("cannot read selected IPA");byte[] buf=new byte[65536];int n;
                while((n=in.read(buf))!=-1){total+=n;if(total>4L*1024*1024*1024)throw new Exception("IPA exceeds size limit");out.write(buf,0,n);}
            }
            File extracted=new File(staging,"extracted");if(!extracted.mkdirs())throw new Exception("cannot stage bundle");
            String appPrefix=null;long extractedBytes=0;int count=0;
            try(ZipFile zip=new ZipFile(archive)){
                // Pass 1: prefer standard Payload/*.app/, fallback to any *.app/ in archive.
                String fallbackPrefix=null;
                for(Enumeration<? extends ZipEntry> entries=zip.entries();entries.hasMoreElements();){
                    String name=normalizeEntryName(entries.nextElement().getName());
                    String lower=name.toLowerCase(Locale.ROOT);
                    int appIdx=lower.indexOf(".app/");
                    if(appIdx<0)continue;
                    String candidate=name.substring(0,appIdx+5);
                    if(lower.startsWith("payload/")){appPrefix=candidate;break;}
                    if(fallbackPrefix==null)fallbackPrefix=candidate;
                }
                if(appPrefix==null)appPrefix=fallbackPrefix;
                if(appPrefix==null)throw new Exception("no Payload/*.app in IPA");
                for(Enumeration<? extends ZipEntry> entries=zip.entries();entries.hasMoreElements();){
                    ZipEntry entry=entries.nextElement();
                    String name=normalizeEntryName(entry.getName());
                    if(!name.startsWith(appPrefix))continue; // only the first .app, no plugins from another bundle
                    String relative=name.substring(appPrefix.length());if(relative.isEmpty()||entry.isDirectory())continue;
                    if(++count>200000)throw new Exception("too many IPA entries");
                    File target=new File(extracted,relative);
                    String base=extracted.getCanonicalPath()+File.separator;
                    if(!target.getCanonicalPath().startsWith(base))throw new Exception("unsafe path in IPA");
                    if(!target.getParentFile().isDirectory()&&!target.getParentFile().mkdirs())throw new Exception("cannot create IPA directory");
                    try(InputStream in=zip.getInputStream(entry);OutputStream out=new FileOutputStream(target)){
                        byte[] buf=new byte[65536];int n;
                        while((n=in.read(buf))!=-1){extractedBytes+=n;if(extractedBytes>8L*1024*1024*1024)throw new Exception("extracted IPA exceeds limit");out.write(buf,0,n);}
                    }
                }
            }
            String trimmed=appPrefix.substring(0,appPrefix.length()-1);
            int slash=trimmed.lastIndexOf('/');
            String appDirName=slash>=0?trimmed.substring(slash+1):trimmed;
            if(!appDirName.toLowerCase(Locale.ROOT).endsWith(".app"))appDirName=appDirName+".app";
            appDirName=sanitizeDirName(appDirName.substring(0,appDirName.length()-4))+".app";
            File app=new File(staging,appDirName);
            if(!extracted.renameTo(app))throw new Exception("cannot stage .app directory");
            JSONObject inspected=new JSONObject(Native.inspectBundle(app.getPath(),new File(staging,"preview.png").getPath()));
            if(!inspected.optBoolean("ok"))throw new Exception(inspected.optString("error","bundle inspection failed"));
            String bundleId=inspected.optString("bundleId");
            String stagedName=inspected.optString("name",appDirName.substring(0,appDirName.length()-4));
            if(bundleId.isEmpty())bundleId="bundle."+sanitizeDirName(stagedName);
            String safeGameDir=sanitizeDirName(bundleId);
            File game=new File(new File(getFilesDir(),"games"),safeGameDir);
            // Destination directory MUST keep the .app suffix so bundle inspection and relative paths treat it as an .app bundle.
            File dest=new File(game,"bundle.app");
            File legacyDest=new File(game,"bundle");
            if(!game.isDirectory()&&!game.mkdirs())throw new Exception("cannot create game directory");
            File old=new File(game,"bundle.previous"),oldIcon=new File(game,"icon.previous.png");
            File icon=new File(game,"icon.png");
            if(old.exists())deleteTree(old);
            if(oldIcon.exists())oldIcon.delete();
            if(legacyDest.exists()&&!dest.exists())legacyDest.renameTo(dest);
            else if(legacyDest.exists())deleteTree(legacyDest);
            boolean backedUp=false,iconBackedUp=false,moved=false;
            try {
                if(dest.exists()){
                    if(!dest.renameTo(old))throw new Exception("cannot preserve previous bundle");
                    backedUp=true;
                }
                if(icon.exists()){
                    if(!icon.renameTo(oldIcon))throw new Exception("cannot preserve previous icon");
                    iconBackedUp=true;
                }
                if(!app.renameTo(dest))throw new Exception("cannot move imported bundle");
                moved=true;
                JSONObject info=new JSONObject(Native.inspectBundle(dest.getPath(),icon.getPath()));
                if(!info.optBoolean("ok"))throw new Exception(info.optString("error","bundle inspection failed"));
                String displayName=info.optString("name");
                if(displayName.isEmpty()||displayName.equals("bundle"))displayName=stagedName;
                JSONObject analysis=new JSONObject(Native.analyze(info.getString("executablePath")));
                JSONArray arch=info.optJSONArray("architectures");
                String architecture=arch==null||arch.length()==0?analysis.optString("arch","Unknown"):arch.optString(0);
                String engines=architecture+(analysis.optBoolean("opengles")?" · OpenGL ES":"")+
                        (analysis.optBoolean("metal")?" · Metal":"");
                String state=analysis.optString("state",analysis.has("error")?"BLOCKED":"ANALYZED");
                String summary=analysis.has("error")?analysis.optString("error"):analysis.optString("summary","Analyzed");
                JSONObject entry=new JSONObject();
                entry.put("bundleId",bundleId).put("version",info.optString("version","0"))
                    .put("name",displayName).put("bundleDir",dest.getPath())
                    .put("executable",info.optString("executable"))
                    .put("executablePath",info.getString("executablePath"))
                    .put("iconPath",info.optString("iconPath"))
                    .put("architectures",info.optJSONArray("architectures"))
                    .put("engines",engines).put("state",state)
                    .put("summary",summary)
                    .put("sourceIpa",uri.toString()).put("lastLaunch",0).put("lastCrash","")
                    .put("imported",System.currentTimeMillis()).put("warnings",info.optJSONArray("warnings"));
                library.upsert(entry);
                if(backedUp)deleteTree(old);
                if(iconBackedUp)oldIcon.delete();
                return displayName;
            } catch(Exception failure) {
                if(moved)deleteTree(dest);
                if(icon.exists())icon.delete();
                if(backedUp && !old.renameTo(dest))
                    throw new Exception("import failed and previous bundle needs recovery at "+old, failure);
                if(iconBackedUp && !oldIcon.renameTo(icon))
                    throw new Exception("import failed and previous icon needs recovery at "+oldIcon, failure);
                throw failure;
            }
        }finally{deleteTree(staging);}
    }
    private void launch(String path,String title){
        Intent intent=new Intent(this,GameActivity.class);
        if(path!=null)intent.putExtra(GameActivity.EXTRA_PATH,path);
        intent.putExtra(GameActivity.EXTRA_GAME,title)
            .putExtra(GameActivity.EXTRA_COMPAT,AppSettings.useArm64CompatibilityFallbacks(this))
            .putExtra(GameActivity.EXTRA_TRACE,AppSettings.traceArm64MissingApis(this));
        startActivity(intent);
    }
}
