// LAS Recomp on Android: SDL's activity runs the runtime (libmain.so, runtime/main.c). Before the native code starts,
// the shipped resources (assets/res: the start screen's font, the fallback fonts, the icon) are copied to
// <internal files>/res, where runtime/platform/linux.c (plat_exe_path) and runtime/paths.c find them, as next to the
// desktop executable. The data folder (game files, saves, settings, the log) is <external files>:
// Android/data/com.lasrecomp.android/files. From LASRecompAndroid by mamcjr (MIT).
package com.lasrecomp;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class LASRecompActivity extends SDLActivity {
    private static final String TAG = "LASRecomp";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "shaderc_shared", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        try {
            copyAssets("res", new File(getFilesDir(), "res"));
        } catch (IOException e) {
            Log.e(TAG, "copying the app's resources: " + e);
        }
        super.onCreate(state);
        setWindowStyle(true);  // (SDL's immersive full screen: no status or navigation bar)
    }

    private void copyAssets(String from, File to) throws IOException {
        AssetManager am = getAssets();
        String[] names = am.list(from);
        if (names == null) return;
        to.mkdirs();
        for (String n : names) {
            try (InputStream in = am.open(from + "/" + n); OutputStream o = new FileOutputStream(new File(to, n))) {
                byte[] buf = new byte[1 << 16];
                for (int k; (k = in.read(buf)) > 0; ) o.write(buf, 0, k);
            }
        }
    }
}
