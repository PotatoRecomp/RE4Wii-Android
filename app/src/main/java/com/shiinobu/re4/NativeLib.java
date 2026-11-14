package com.shiinobu.re4;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetFileDescriptor;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * libmain.so ships zstd-compressed in assets/native (tools/b-android.ps1 -Mode stage): about 48 MB in
 * the APK instead of 113 MB as a deflated jniLib. It is unpacked once per install into files/native,
 * with a stamp of the install (version code and update time) so a new APK unpacks its own copy.
 * An APK built without the packed asset loads libmain.so from jniLibs as before.
 */
final class NativeLib {
    private static final String TAG = "RE4NativeLib";
    private static final String ASSET = "native/libmain.so.zst";

    private NativeLib() {}

    private static File dir(Context context) {
        return new File(context.getFilesDir(), "native");
    }

    private static File lib(Context context) {
        return new File(dir(context), "libmain.so");
    }

    private static File stampFile(Context context) {
        return new File(dir(context), "libmain.stamp");
    }

    /** True when this APK carries the packed libmain.so. */
    static boolean packed(Context context) {
        try (AssetFileDescriptor a = context.getAssets().openFd(ASSET)) {
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    static String stamp(Context context) {
        try {
            final PackageInfo p = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return p.getLongVersionCode() + ":" + p.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    /** True when nothing has to be unpacked before the game loads. */
    static boolean ready(Context context) {
        if (!packed(context)) return true;
        try {
            return lib(context).isFile() && stamp(context).equals(
                    new String(Files.readAllBytes(stampFile(context).toPath()), StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
    }

    /** Unpacks libmain.so when needed (a few seconds, once per install). Null when ready, else why not. */
    static synchronized String prepare(Context context) {
        if (ready(context)) return null;
        final File dir = dir(context);
        final File[] old = dir.listFiles();
        if (old != null) for (File f : old) f.delete();
        dir.mkdirs();
        final File tmp = new File(dir, "libmain.so.part");
        final long start = System.nanoTime();
        try (AssetFileDescriptor a = context.getAssets().openFd(ASSET)) {
            final String error = DiscImport.unpackZstd(a.getParcelFileDescriptor().getFd(),
                    a.getStartOffset(), a.getLength(), tmp.getPath());
            if (error != null) {
                tmp.delete();
                return error;
            }
        } catch (IOException e) {
            tmp.delete();
            return e.getMessage();
        }
        if (!tmp.renameTo(lib(context))) return "cannot rename " + tmp;
        try (FileOutputStream out = new FileOutputStream(stampFile(context))) {
            out.write(stamp(context).getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return e.getMessage();
        }
        Log.i(TAG, String.format(java.util.Locale.ROOT, "libmain.so unpacked: %.1f MB in %d ms",
                lib(context).length() / 1048576.0, (System.nanoTime() - start) / 1000000));
        return null;
    }

    /** The libmain.so the game loads: the unpacked one, or jniLibs' when the APK has no packed copy. */
    static String path(Context context) {
        return packed(context) ? lib(context).getPath()
                : context.getApplicationInfo().nativeLibraryDir + "/libmain.so";
    }
}
