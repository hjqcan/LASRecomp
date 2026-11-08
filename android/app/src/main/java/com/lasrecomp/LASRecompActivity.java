// LAS Recomp on Android: SDL's activity runs the runtime (libmain.so, runtime/main.c). Before the native code starts,
// the shipped resources (assets/res: the start screen's font, the fallback fonts, the icon) are copied to
// <internal files>/res, where runtime/platform/linux.c (plat_exe_path) and runtime/paths.c find them, as next to the
// desktop executable. The data folder (game files, saves, settings, the log) is <external files>:
// Android/data/com.lasrecomp.android/files.
// The game files folder is picked with Android's own folder picker (SDL has none for folders), which gives a
// content:// folder rather than a path: the setup copies the game files from it through the storage framework
// (importGameFiles), into the data folder, where the runtime reads them as files. From LASRecompAndroid by mamcjr (MIT).
package com.lasrecomp;

import android.content.ContentResolver;
import android.content.Intent;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class LASRecompActivity extends SDLActivity {
    private static final String TAG = "LASRecomp";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "shaderc_shared", "avutil", "avcodec", "avformat", "swscale", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        try {
            copyAssets("res", new File(getFilesDir(), "res"));
            Os.setenv("LARECOMP_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("LARECOMP_GEN_DIR", new File(getFilesDir(), "gen").getAbsolutePath(), true);
            File driver = customDriver();  // (runtime/nvn/vulkan/android_driver.c: none = the system's)
            Os.setenv("LARECOMP_VK_DRIVER", driver != null ? driver.getAbsolutePath() : "", true);
            // the desktop's debug and test switches (docs/debug-hooks.md) from the launch, in debuggable builds only (the
            // activity is exported: in a release build any app could start it with switches set):
            //   adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity --es LARECOMP_LOG_SKIPS 1
            boolean debuggable = (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
            Bundle extras = debuggable && getIntent() != null ? getIntent().getExtras() : null;
            if (extras != null)
                for (String k : extras.keySet())
                    if (k.startsWith("LARECOMP_") && extras.get(k) != null) {
                        Os.setenv(k, String.valueOf(extras.get(k)), true);
                        Log.i(TAG, "launch switch " + k + "=" + extras.get(k));
                    }
        } catch (IOException | ErrnoException e) {
            Log.e(TAG, "setting up the app's files: " + e);
        }
        unpackBuilder();
        self = this;
        super.onCreate(state);
        setWindowStyle(true);  // (SDL's immersive full screen: no status or navigation bar)
    }

    // The game code builder's files (assets/gen.zip: Python's standard library, the bindings, the generator, zig's lib/,
    // the headers) into <internal files>/gen, once per install or update, on a thread of its own: tens of thousands of
    // files. gen/.ready marks it done (runtime/gamebuild.c waits for it).
    private void unpackBuilder() {
        File dir = new File(getFilesDir(), "gen"), ready = new File(dir, ".ready");
        String stamp;
        try {
            stamp = Long.toString(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        } catch (Exception e) {
            stamp = "?";
        }
        try (InputStream in = new java.io.FileInputStream(ready)) {
            if (new String(in.readAllBytes()).equals(stamp)) return;
        } catch (IOException e) {
            // (not unpacked yet, or from another install)
        }
        final String id = stamp;
        new Thread(() -> {
            long t0 = System.nanoTime();
            ready.delete();
            deleteTree(dir);
            byte[] buf = new byte[1 << 16];
            try (ZipInputStream z = new ZipInputStream(new java.io.BufferedInputStream(getAssets().open("gen.zip"), 1 << 16))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    File f = new File(dir, e.getName());
                    if (!f.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator)) continue;
                    if (e.isDirectory()) { f.mkdirs(); continue; }
                    f.getParentFile().mkdirs();
                    try (OutputStream o = new FileOutputStream(f)) {
                        for (int k; (k = z.read(buf)) > 0; ) o.write(buf, 0, k);
                    }
                }
                try (OutputStream o = new FileOutputStream(ready)) { o.write(id.getBytes()); }
                Log.i(TAG, "game code builder unpacked in " + (System.nanoTime() - t0) / 1000000 + " ms");
            } catch (IOException e) {
                Log.e(TAG, "unpacking the game code builder: " + e);
            }
        }, "unpack-builder").start();
    }

    // ---- the Vulkan driver package (Settings > Game files: runtime/ui/launcher.mm). It takes effect at the next start:
    // the driver is loaded before the start screen. From LASRecompAndroid by mamcjr (MIT).
    public void pickDriver() {
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                startActivityForResult(i, PICK_DRIVER);
            } catch (Exception e) {
                Log.e(TAG, "file picker: " + e);
                nativeDriverPicked("");
            }
        });
    }

    // The package into <external files>/drivers, in place of any there; its name, or "" (not a .zip / .so, or the copy
    // failed)
    private String copyDriver(Uri f) {
        String name = "driver.zip";
        try (Cursor c = getContentResolver().query(f, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) name = c.getString(0);
        } catch (Exception ignored) {
        }
        String low = name.toLowerCase();
        if ((!low.endsWith(".zip") && !low.endsWith(".so")) || name.contains("/")) return "";
        File dir = new File(getExternalFilesDir(null), "drivers");
        deleteTree(dir);
        dir.mkdirs();
        try (InputStream in = getContentResolver().openInputStream(f); OutputStream o = new FileOutputStream(new File(dir, name))) {
            if (in == null) return "";
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0; ) o.write(buf, 0, k);
            return name;
        } catch (Exception e) {
            Log.e(TAG, "driver package: " + e);
            return "";
        }
    }

    public void useSystemDriver() {
        deleteTree(new File(getExternalFilesDir(null), "drivers"));
        deleteTree(new File(getFilesDir(), "drivers"));
        nativeDriverPicked("-");
    }

    // ---- a Vulkan driver error that stops the app (runtime/platform/linux.c plat_fatal_notice). The driver is loaded
    // before the start screen, so its Settings rows can't be reached when it fails: this dialog chooses another
    // package, or goes back to the phone's own driver, and starts the app again; else it closes. Blocks the calling
    // (native) thread until then. From LASRecompAndroid by mamcjr after LASRecomp-Android-Plus (both MIT).
    private static final Object dlgLock = new Object();
    private static boolean dlgDone;
    private static String dlgText;
    private static LASRecompActivity self;

    public static boolean driverErrorDialog(String text) {
        LASRecompActivity a = self;
        if (a == null) return false;
        synchronized (dlgLock) { dlgDone = false; dlgText = text; }
        a.runOnUiThread(() -> a.showDriverError(null));
        synchronized (dlgLock) {
            while (!dlgDone) {
                try { dlgLock.wait(); } catch (InterruptedException e) { break; }
            }
        }
        return true;
    }

    private void showDriverError(String extra) {
        String msg = dlgText + (extra != null ? "\n\n" + extra : "");
        android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("LAS Recomp").setMessage(msg).setCancelable(false)
            .setPositiveButton("Choose a driver package", (d, w) -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try {
                    startActivityForResult(i, PICK_DRIVER_RECOVER);
                } catch (Exception e) {
                    Log.e(TAG, "file picker: " + e);
                    showDriverError("The file picker couldn't open.");
                }
            })
            .setNegativeButton("Close", (d, w) -> {
                synchronized (dlgLock) { dlgDone = true; dlgLock.notifyAll(); }
            });
        File[] custom = new File(getExternalFilesDir(null), "drivers").listFiles((dir, n) -> n.endsWith(".so") || n.endsWith(".zip"));
        if (custom != null && custom.length > 0)
            b.setNeutralButton("Use the phone's own driver", (d, w) -> {
                deleteTree(new File(getExternalFilesDir(null), "drivers"));
                deleteTree(new File(getFilesDir(), "drivers"));
                restartApp();
            });
        b.show();
    }

    // Starts the app again (a new process: the driver is chosen at start). Started while the app is in front (an alarm
    // would be a background start, which Android 14+ blocks), then this process ends and the system starts a new one
    // for it.
    public void restartApp() {
        runOnUiThread(() -> {
            Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
            if (i != null) {
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
            }
            System.exit(0);
        });
    }

    // The Vulkan driver: the first driver package in <external files>/drivers - an adrenotools .zip (Turnip builds as
    // other emulators use them) or a bare .so - unpacked into internal storage (a driver can't be loaded from shared
    // storage), again only when it changed. None there: the system's driver. From LASRecompAndroid by mamcjr (MIT).
    private File customDriver() throws IOException {
        File src = new File(getExternalFilesDir(null), "drivers");
        src.mkdirs();
        File[] files = src.listFiles((d, n) -> n.endsWith(".so") || n.endsWith(".zip"));
        if (files == null || files.length == 0) return null;
        java.util.Arrays.sort(files);
        File f = files[0];
        File dir = new File(getFilesDir(), "drivers/" + f.getName().replaceAll("\\.(so|zip)$", ""));
        File stamp = new File(dir, ".source");
        String id = f.length() + ":" + f.lastModified();
        File lib = findLib(dir);
        try (InputStream in = new java.io.FileInputStream(stamp)) {
            if (lib != null && id.equals(new String(in.readAllBytes()))) return lib;
        } catch (IOException e) {
            // (not unpacked yet)
        }
        deleteTree(dir);
        dir.mkdirs();
        byte[] buf = new byte[1 << 16];
        if (f.getName().endsWith(".so")) {
            try (InputStream in = new java.io.FileInputStream(f); OutputStream o = new FileOutputStream(new File(dir, f.getName()))) {
                for (int k; (k = in.read(buf)) > 0; ) o.write(buf, 0, k);
            }
        } else {
            try (ZipInputStream z = new ZipInputStream(new java.io.FileInputStream(f))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    String n = new File(e.getName()).getName();
                    if (e.isDirectory() || !(n.endsWith(".so") || n.equals("meta.json"))) continue;
                    try (OutputStream o = new FileOutputStream(new File(dir, n))) {
                        for (int k; (k = z.read(buf)) > 0; ) o.write(buf, 0, k);
                    }
                }
            }
        }
        lib = findLib(dir);
        if (lib != null) try (OutputStream o = new FileOutputStream(stamp)) { o.write(id.getBytes()); }
        Log.i(TAG, "Vulkan driver: " + f.getName() + " -> " + lib);
        return lib;
    }

    // The driver in a package: meta.json's libraryName (adrenotools packages), else the library named like a Vulkan
    // driver (vulkan.*.so, libvulkan_*.so), else the first: a package can carry helper libraries beside it
    private static File findLib(File dir) {
        File meta = new File(dir, "meta.json");
        if (meta.isFile()) {
            try (InputStream in = new java.io.FileInputStream(meta)) {
                String name = new org.json.JSONObject(new String(in.readAllBytes())).optString("libraryName", "");
                File lib = new File(dir, new File(name).getName());
                if (!name.isEmpty() && lib.isFile()) return lib;
            } catch (Exception e) {
                Log.w(TAG, "driver package meta.json: " + e);
            }
        }
        File[] libs = dir.listFiles((d, n) -> n.endsWith(".so"));
        if (libs == null || libs.length == 0) return null;
        java.util.Arrays.sort(libs);
        for (File l : libs)
            if (l.getName().startsWith("vulkan.") || l.getName().startsWith("libvulkan")) return l;
        return libs[0];
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
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

    // ---- the game files folder (runtime/platform/android_storage.c)
    private static final int PICK_GAME_FOLDER = 0x4c41;  // (SDL's own dialogs use other request codes)
    private static final int PICK_DRIVER = 0x4c42;
    private static final int PICK_DRIVER_RECOVER = 0x4c43;  // (from the driver error dialog: the package, then a restart)
    static native void nativeDriverPicked(String result);  // a package's name, "-" the system's again, "" not a driver

    static native void nativeFolderPicked(String uri);  // null: cancelled
    static native void nativeFilesPicked(String[] paths);  // empty: cancelled, null: no file picker
    static native void nativeImportProgress(long done, long total);
    static native boolean nativeImportCancelled();

    // ---- files for the start screen (Settings > Game font's Import fonts: runtime/ui/launcher.mm). The system's picker
    // gives content:// links, not paths, so each picked .ttf / .bfttf is copied under the cache folder by its own name
    // first; other files are passed by name only (the import skips them).
    private static final int PICK_FILES = 0x4c44;

    public void pickFiles() {
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            try {
                startActivityForResult(i, PICK_FILES);
            } catch (Exception e) {
                Log.e(TAG, "file picker: " + e);
                nativeFilesPicked(null);
            }
        });
    }

    private String[] copyPicked(List<Uri> picks) {
        File dir = new File(getCacheDir(), "picked");
        deleteTree(dir);
        dir.mkdirs();
        List<String> out = new ArrayList<>();
        for (Uri f : picks) {
            String name = null;
            try (Cursor c = getContentResolver().query(f, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0)) name = c.getString(0);
            } catch (Exception ignored) {
            }
            if (name == null || name.isEmpty() || name.contains("/")) continue;
            File dst = new File(dir, name);
            out.add(dst.getPath());
            String low = name.toLowerCase();
            if (!low.endsWith(".ttf") && !low.endsWith(".bfttf")) continue;
            try (InputStream in = getContentResolver().openInputStream(f); OutputStream o = new FileOutputStream(dst)) {
                if (in == null) continue;
                byte[] buf = new byte[1 << 16];
                for (int k; (k = in.read(buf)) > 0; ) o.write(buf, 0, k);
            } catch (Exception e) {  // (the import reports it as not copied)
                Log.e(TAG, "picked file: " + e);
                dst.delete();
            }
        }
        return out.toArray(new String[0]);
    }

    // Called from the start screen's Choose... (any thread)
    public void pickGameFolder() {
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                startActivityForResult(i, PICK_GAME_FOLDER);
            } catch (Exception e) {
                Log.e(TAG, "folder picker: " + e);
                nativeFolderPicked(null);
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        PlayService.start(this);  // (keeps the game alive at the home screen: PlayService.java)
    }

    @Override
    protected void onDestroy() {
        PlayService.stop(this);
        super.onDestroy();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == PICK_DRIVER_RECOVER) {
            Uri f = resultCode == RESULT_OK && data != null ? data.getData() : null;
            String name = f != null ? copyDriver(f) : "";
            if (!name.isEmpty()) restartApp();
            else showDriverError(f != null ? "That file isn't a driver package (.zip or .so)." : null);
            return;
        }
        if (requestCode == PICK_DRIVER) {
            Uri f = resultCode == RESULT_OK && data != null ? data.getData() : null;
            if (f != null) new Thread(() -> nativeDriverPicked(copyDriver(f)), "copy-driver").start();
            return;
        }
        if (requestCode == PICK_FILES) {
            List<Uri> picks = new ArrayList<>();
            if (resultCode == RESULT_OK && data != null) {
                if (data.getClipData() != null)
                    for (int k = 0; k < data.getClipData().getItemCount(); k++) picks.add(data.getClipData().getItemAt(k).getUri());
                else if (data.getData() != null) picks.add(data.getData());
            }
            new Thread(() -> nativeFilesPicked(copyPicked(picks)), "copy-picked").start();
            return;
        }
        if (requestCode != PICK_GAME_FOLDER) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        Uri tree = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (tree != null) {
            try {  // (kept across restarts: a setup can be started later)
                getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException e) {
                Log.w(TAG, "folder permission not kept: " + e);
            }
        }
        nativeFolderPicked(tree != null ? tree.toString() : null);
    }

    private static final class Doc {
        final String id, name, mime;
        final long size;
        Doc(String id, String name, String mime, long size) { this.id = id; this.name = name; this.mime = mime; this.size = size; }
        boolean isDir() { return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime); }
    }

    private List<Doc> children(Uri tree, String id) {
        List<Doc> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new Doc(c.getString(0), c.getString(1), c.getString(2), c.isNull(3) ? 0 : c.getLong(3)));
        }
        out.sort((a, b) -> a.name.compareTo(b.name));
        return out;
    }

    private static Doc child(List<Doc> kids, String name) {
        for (Doc d : kids) if (d.name.equals(name)) return d;
        return null;
    }

    // As runtime/ui/launcher.mm find_game: exefs = the folder with main and main.npdm, romfs = the folder with
    // region_common, at most 4 levels down; game packages instead of files make it unusable.
    private boolean unusable;
    private void findGame(Uri tree, Doc dir, int depth, Doc[] found) {
        if (depth > 4 || (found[0] != null && found[1] != null)) return;
        List<Doc> kids = children(tree, dir.id);
        if (found[0] == null && child(kids, "main") != null && child(kids, "main.npdm") != null) found[0] = dir;
        Doc rc = child(kids, "region_common");
        if (found[1] == null && rc != null && rc.isDir()) found[1] = dir;
        for (Doc k : kids) {
            String n = k.name.toLowerCase();
            if (n.endsWith(".xci") || n.endsWith(".nsp") || n.endsWith(".nca") || n.endsWith(".nsz") || n.endsWith(".xcz"))
                unusable = true;
        }
        for (Doc k : kids)
            if (k.isDir() && !k.name.startsWith(".") && !k.name.equals("region_common")) findGame(tree, k, depth + 1, found);
    }

    private long treeBytes(Uri tree, Doc dir) {
        long n = 0;
        for (Doc k : children(tree, dir.id)) n += k.isDir() ? treeBytes(tree, k) : k.size;
        return n;
    }

    private long done, total;
    private boolean copyTree(Uri tree, Doc dir, File to) throws IOException {
        to.mkdirs();
        ContentResolver cr = getContentResolver();
        byte[] buf = new byte[1 << 20];
        for (Doc k : children(tree, dir.id)) {
            if (nativeImportCancelled()) return false;
            File dst = new File(to, k.name);
            if (k.isDir()) {
                if (!copyTree(tree, k, dst)) return false;
                continue;
            }
            Uri u = DocumentsContract.buildDocumentUriUsingTree(tree, k.id);
            try (InputStream in = cr.openInputStream(u); OutputStream o = new FileOutputStream(dst)) {
                if (in == null) throw new IOException("can't open " + k.name);
                for (int r; (r = in.read(buf)) > 0; ) {
                    o.write(buf, 0, r);
                    done += r;
                }
            }
            nativeImportProgress(done, total);
        }
        return true;
    }

    // The setup's copy (its own thread): exefs and romfs from the picked folder into dst/exefs and dst/romfs.
    // 0 copied, 1 not found, 2 game packages instead of files, 3 copy failed, -1 cancelled (launcher.mm FAIL_*).
    public int importGameFiles(String uri, String dst) {
        try {
            Uri tree = Uri.parse(uri);
            Doc root = new Doc(DocumentsContract.getTreeDocumentId(tree), "", DocumentsContract.Document.MIME_TYPE_DIR, 0);
            Doc[] found = new Doc[2];
            unusable = false;
            findGame(tree, root, 0, found);
            if (found[0] == null || found[1] == null) return unusable ? 2 : 1;
            done = 0;
            total = treeBytes(tree, found[0]) + treeBytes(tree, found[1]);
            nativeImportProgress(0, total);
            if (!copyTree(tree, found[0], new File(dst, "exefs")) || !copyTree(tree, found[1], new File(dst, "romfs")))
                return -1;
            return 0;
        } catch (Exception e) {
            Log.e(TAG, "copying the game files: " + e);
            return 3;
        }
    }
}
