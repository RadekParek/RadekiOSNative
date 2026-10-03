package org.radekiosnative.recompiler;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
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
    private static final int BG=Color.rgb(10,13,21),SURFACE=Color.rgb(20,26,38);
    private static final int TEXT=Color.rgb(239,242,249),DIM=Color.rgb(161,173,193),ACCENT=Color.rgb(111,143,255);
    private final List<JSONObject> games=new ArrayList<>();
    private LinearLayout cards;
    private LinearLayout latestLogCard;
    private ScrollView homeScroll;
    private Library library;
    private String autoPromptedLogId = "";
    private int dp(int n){return Math.round(getResources().getDisplayMetrics().density*n);}
    private TextView label(String text,int size,int color){TextView v=new TextView(this);v.setText(text);v.setTextSize(size);v.setTextColor(color);return v;}
    @Override protected void onCreate(Bundle state){
        super.onCreate(state);library=new Library(this);
        ScrollView scroll=new ScrollView(this);homeScroll=scroll;scroll.setFillViewport(true);scroll.setBackgroundColor(BG);
        LinearLayout root=new LinearLayout(this);root.setPadding(dp(20),dp(26),dp(20),dp(28));root.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(root);

        LinearLayout appBar=new LinearLayout(this);appBar.setOrientation(LinearLayout.HORIZONTAL);
        appBar.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout brand=new LinearLayout(this);brand.setOrientation(LinearLayout.VERTICAL);
        TextView name=label("RadekiOSNative",25,TEXT);name.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        brand.addView(name);
        brand.addView(label("iOS game compatibility · native runtime",13,DIM));
        appBar.addView(brand,new LinearLayout.LayoutParams(0,-2,1));
        Button settings=new Button(this);settings.setText("SETTINGS");settings.setTextColor(Color.WHITE);
        settings.setContentDescription("Runtime and logging settings");
        settings.setBackgroundTintList(ColorStateList.valueOf(ACCENT));
        settings.setOnClickListener(v->showSettings());
        appBar.addView(settings,new LinearLayout.LayoutParams(-2,dp(44)));
        root.addView(appBar);

        LinearLayout hero=new LinearLayout(this);hero.setOrientation(LinearLayout.VERTICAL);
        hero.setPadding(dp(18),dp(18),dp(18),dp(16));hero.setBackground(rounded(SURFACE,20,Color.rgb(42,53,76)));
        TextView tag=label(runtimeEditionTitle(),12,ACCENT);tag.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        hero.addView(tag);
        TextView heroTitle=label("Your game library",22,TEXT);heroTitle.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        heroTitle.setPadding(0,dp(6),0,0);hero.addView(heroTitle);
        TextView heroCopy=label("Import an IPA, review compatibility notes, then launch with a saved diagnostic log.",13,DIM);
        heroCopy.setPadding(0,dp(4),0,dp(12));hero.addView(heroCopy);
        LinearLayout primaryActions=new LinearLayout(this);primaryActions.setOrientation(LinearLayout.HORIZONTAL);
        Button add=new Button(this);add.setText("＋  IMPORT IPA");add.setTextColor(Color.WHITE);
        add.setBackgroundTintList(ColorStateList.valueOf(ACCENT));
        add.setOnClickListener(v->{Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");startActivityForResult(i,PICK);});
        primaryActions.addView(add,new LinearLayout.LayoutParams(0,dp(48),1));
        Button self=new Button(this);self.setText("RUN SELF-TEST");self.setTextColor(ACCENT);
        self.setOnClickListener(v->launch(null,"Self-test",isArm32Edition()?"ARMv7 self-test":"ARM64 self-test"));
        LinearLayout.LayoutParams selfParams=new LinearLayout.LayoutParams(0,dp(48),1);selfParams.leftMargin=dp(8);
        primaryActions.addView(self,selfParams);hero.addView(primaryActions);
        root.addView(hero,new LinearLayout.LayoutParams(-1,-2));

        latestLogCard=new LinearLayout(this);latestLogCard.setOrientation(LinearLayout.VERTICAL);
        latestLogCard.setPadding(dp(16),dp(14),dp(16),dp(12));
        latestLogCard.setBackground(rounded(SURFACE,18,Color.rgb(36,46,65)));
        LinearLayout.LayoutParams logParams=new LinearLayout.LayoutParams(-1,-2);logParams.topMargin=dp(14);
        root.addView(latestLogCard,logParams);

        LinearLayout header=new LinearLayout(this);header.setPadding(0,dp(26),0,dp(8));header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title=label("IMPORTED GAMES",12,DIM);title.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        header.addView(title,new LinearLayout.LayoutParams(0,dp(44),1));
        Button history=new Button(this);history.setText("RUN HISTORY");history.setTextColor(ACCENT);
        history.setOnClickListener(v->showRunHistory());header.addView(history);
        root.addView(header);
        cards=new LinearLayout(this);cards.setOrientation(LinearLayout.VERTICAL);root.addView(cards);

        setContentView(scroll);refresh();
    }

    private GradientDrawable rounded(int color,int radiusDp,int strokeColor){
        GradientDrawable drawable=new GradientDrawable();drawable.setColor(color);
        drawable.setCornerRadius(dp(radiusDp));if(strokeColor!=0)drawable.setStroke(dp(1),strokeColor);
        return drawable;
    }

    private boolean isArm32Edition(){
        return BuildConfig.FLAVOR != null && BuildConfig.FLAVOR.contains("arm32");
    }
    private String runtimeEditionTitle(){
        return isArm32Edition()
                ? "ARM32 EDITION  ·  ARMv7 / AArch32" : "ARM64 EDITION  ·  arm64-v8a";
    }

    @Override protected void onResume(){
        super.onResume();
        if(latestLogCard==null)return;
        updateLatestRunCard();
        File latest=RunHistory.latest(this);
        if(latest!=null&&!latest.getName().equals(autoPromptedLogId)&&RunHistory.shouldShowLatest(this)){
            autoPromptedLogId=latest.getName();
            latestLogCard.postDelayed(()->{
                if(!isFinishing()){
                    RunHistory.markViewed(this,latest);
                    RunLogViewer.show(this,latest);
                    updateLatestRunCard();
                }
            },350);
        }
    }

    private void updateLatestRunCard(){
        if(latestLogCard==null)return;
        latestLogCard.removeAllViews();
        File latest=RunHistory.latest(this);
        TextView heading=label("LATEST RUN",12,ACCENT);heading.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        latestLogCard.addView(heading);
        if(latest==null){
            latestLogCard.addView(label("No launches yet. A run log is created automatically for every game.",13,DIM));
            return;
        }
        String state=RunHistory.status(latest);
        TextView summary=label(RunHistory.title(latest)+"  ·  "+state,16,TEXT);
        summary.setTypeface(Typeface.DEFAULT,Typeface.BOLD);summary.setPadding(0,dp(5),0,dp(4));
        latestLogCard.addView(summary);
        TextView path=label(latest.getName(),11,DIM);path.setSingleLine(true);path.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        latestLogCard.addView(path);
        LinearLayout actions=new LinearLayout(this);actions.setGravity(Gravity.END);
        Button view=new Button(this);view.setText("OPEN LOG");view.setTextColor(ACCENT);
        view.setOnClickListener(v->{RunHistory.markViewed(this,latest);RunLogViewer.show(this,latest);});
        actions.addView(view);latestLogCard.addView(actions);
    }

    private void showRunHistory(){
        List<File> logs=RunHistory.recent(this,20);
        if(logs.isEmpty()){
            new AlertDialog.Builder(this).setTitle("Run history")
                    .setMessage("No run logs are saved yet. Every launch will create one here.")
                    .setPositiveButton("DONE",null).show();return;
        }
        String[] labels=new String[logs.size()];
        for(int i=0;i<logs.size();i++)labels[i]=RunHistory.title(logs.get(i))+"  ·  "+RunHistory.status(logs.get(i));
        new AlertDialog.Builder(this).setTitle("Recent runs").setItems(labels,(dialog,index)->{
            File selected=logs.get(index);RunHistory.markViewed(this,selected);RunLogViewer.show(this,selected);
        }).setNegativeButton("CLOSE",null).show();
    }
    private void showSettings(){
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(false);
        LinearLayout content=new LinearLayout(this);content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(4),dp(4),dp(4),dp(4));scroll.addView(content);

        boolean arm32Edition=isArm32Edition();
        addSettingsHeading(content,"ACTIVE RUNTIME",arm32Edition?"ARM32 edition · armeabi-v7a native process":"ARM64 edition · arm64-v8a native process");
        addSettingsToggle(content,"Compatibility fallbacks",
            "Uses limited startup shims and safe defaults for missing imports. This is not a full UIKit, Foundation, or Objective-C runtime, so some games can still stop before their boot screen.",
            AppSettings.useArm64CompatibilityFallbacks(this),
            (button,enabled)->AppSettings.setArm64CompatibilityFallbacks(this,enabled));
        addSettingsToggle(content,"Trace missing API calls",
            "Samples unresolved import calls (first hit and then every 4,096 calls). Off by default; enable it when investigating a launch failure.",
            AppSettings.traceArm64MissingApis(this),
            (button,enabled)->AppSettings.setArm64TraceMissingApis(this,enabled));
        addSettingsToggle(content,"Real-time logging system (RTLS)",
            "Writes sampled startup/API events, EGL and Surface diagnostics, and a 5-second render-loop heartbeat into each run log while it runs. Off by default. Basic run summaries and errors are always saved.",
            AppSettings.useRealtimeLogging(this),
            (button,enabled)->AppSettings.setRealtimeLogging(this,enabled));

        addSettingsHeading(content,"ARM32 / ARM64 GUEST SUPPORT","Choose the APK that matches the guest CPU");
        if(arm32Edition){
            addSettingsInfo(content,"ARMv7 execution edition is active",
                "This variant runs in a 32-bit ARM process and can execute ARMv7/AArch32 guest instructions. ARM64-only games require the ARM64 edition. Framework and graphics compatibility remains partial.");
        }else{
            addSettingsInfo(content,"ARM32 edition available",
                "ARM32 code cannot execute inside a 64-bit ARM process. Use the separately built ARM32 edition on devices that support 32-bit apps for ARMv7-only games. ARM64 games continue to use this edition.");
        }

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
        card.setPadding(dp(12),dp(12),dp(12),dp(12));card.setBackground(rounded(SURFACE,14,Color.rgb(36,46,65)));
        card.addView(label(title,15,TEXT));
        TextView detail=label(description,12,DIM);detail.setPadding(0,dp(4),0,0);card.addView(detail);
        parent.addView(card,new LinearLayout.LayoutParams(-1,-2));
    }

    private Bitmap decodeIcon(String path,int targetPixels){
        if(path==null||path.isEmpty())return null;
        try{
            BitmapFactory.Options bounds=new BitmapFactory.Options();bounds.inJustDecodeBounds=true;
            BitmapFactory.decodeFile(path,bounds);
            if(bounds.outWidth<=0||bounds.outHeight<=0)return null;
            int sample=1;
            while(bounds.outWidth/(sample*2)>=targetPixels&&bounds.outHeight/(sample*2)>=targetPixels)sample*=2;
            BitmapFactory.Options options=new BitmapFactory.Options();options.inSampleSize=sample;
            return BitmapFactory.decodeFile(path,options);
        }catch(OutOfMemoryError ignored){return null;}
    }

    private void refresh(){
        games.clear();games.addAll(library.entries());cards.removeAllViews();updateLatestRunCard();
        if(games.isEmpty()){
            TextView empty=label("Your library is empty. Import an IPA to inspect its architecture and runtime blockers.",14,DIM);
            empty.setPadding(dp(4),dp(8),dp(4),dp(14));cards.addView(empty);
        }
        for(JSONObject g:games){
            LinearLayout card=new LinearLayout(this);card.setOrientation(LinearLayout.VERTICAL);card.setPadding(dp(14),dp(14),dp(14),dp(14));
            card.setBackground(rounded(SURFACE,18,Color.rgb(36,46,65)));
            LinearLayout row=new LinearLayout(this);row.setOrientation(LinearLayout.HORIZONTAL);
            ImageView icon=new ImageView(this);icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
            Bitmap bitmap=decodeIcon(g.optString("iconPath"),dp(72));
            if(bitmap!=null)icon.setImageBitmap(bitmap);else icon.setBackground(rounded(BG,12,0)); // no fabricated app icon
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
                .setPositiveButton("Attempt",(d,w)->launch(g.optString("executablePath"),g.optString("name"),g.optString("engines","not detected")))
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
                String bundledArch=arch==null||arch.length()==0?"Unknown":arch.optString(0);
                String architecture=analysis.optString("arch",bundledArch);
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
    private void launch(String path,String title,String architecture){
        Intent intent=new Intent(this,GameActivity.class);
        if(path!=null)intent.putExtra(GameActivity.EXTRA_PATH,path);
        String processAbi=isArm32Edition()?"ARMv7/AArch32 native process":"ARM64/AArch64 native process";
        String analysisAbi=architecture==null||architecture.isEmpty()?"":" · bundle analysis: "+architecture;
        intent.putExtra(GameActivity.EXTRA_GAME,title)
            .putExtra(GameActivity.EXTRA_ARCH,processAbi+analysisAbi)
            .putExtra(GameActivity.EXTRA_COMPAT,AppSettings.useArm64CompatibilityFallbacks(this))
            .putExtra(GameActivity.EXTRA_TRACE,AppSettings.traceArm64MissingApis(this));
        startActivity(intent);
    }
}
