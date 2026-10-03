package org.radekiosnative.recompiler;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

// Guest execution is isolated; persist diagnostics before this process is killed.
public class GuestService extends Service {
    @Override public IBinder onBind(Intent i) { return null; }
    @Override public int onStartCommand(Intent intent,int flags,int id) {
        final String path=intent==null?null:intent.getStringExtra("path");
        final String game=intent==null?"Self-test":intent.getStringExtra("game");
        final boolean compatibilityFallbacks=intent==null||intent.getBooleanExtra("arm64CompatibilityFallbacks",true);
        final boolean traceMissingApis=intent!=null&&intent.getBooleanExtra("arm64TraceMissingApis",false);
        new Thread(()->{
            JSONObject result;
            try { result=new JSONObject(path==null?Native.selfTest():Native.run(path,compatibilityFallbacks,traceMissingApis)); }
            catch(Throwable t){result=new JSONObject();try{result.put("error",t.toString());}catch(Exception ignored){}}
            try {
                if(result.optBoolean("crashed")||!result.optString("error").isEmpty())
                    CrashLog.write(this,game==null?"Guest":game,result);
                File file=new File(getFilesDir(),"result.json");
                try(FileOutputStream out=new FileOutputStream(file)){
                    out.write(result.toString().getBytes(StandardCharsets.UTF_8));out.getFD().sync();
                }
            }catch(Exception e){android.util.Log.e("RadekiOSNative","Could not persist guest result",e);}
            android.os.Process.killProcess(android.os.Process.myPid());
        }).start();return START_NOT_STICKY;
    }
}
