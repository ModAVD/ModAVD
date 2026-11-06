// Copyright 2026 Ji Mao Cai
package org.modavd.tests.downloadlistener;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import java.util.concurrent.atomic.AtomicBoolean;

public final class ProbeInstrumentation extends Instrumentation {
    private static native String nativeRun(String filesDirectory);

    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override public void onStart() {
        final AtomicBoolean finished = new AtomicBoolean(false);
        Thread watchdog = new Thread(new Runnable() {
            @Override public void run() {
                try {
                    Thread.sleep(10000);
                } catch (InterruptedException ignored) {
                    return;
                }
                if (!finished.get()) {
                    Log.e("ModavdListenerProbe", "FAIL watchdog timeout after 10 seconds");
                    Process.killProcess(Process.myPid());
                }
            }
        }, "listener-probe-guard");
        watchdog.setDaemon(true);
        watchdog.start();
        Bundle starting = new Bundle();
        starting.putString("stream", "START late-jni listener app probe pid="
                + Process.myPid() + " uid=" + Process.myUid() + "\n");
        sendStatus(1, starting);
        Bundle result = new Bundle();
        int code = Activity.RESULT_CANCELED;
        try {
            System.loadLibrary("download_listener_probe");
            String report = nativeRun(getTargetContext().getFilesDir().getAbsolutePath());
            result.putString("stream", "\n" + report);
            boolean passed = report.startsWith("PASS late-jni listener app probe\n");
            result.putBoolean("passed", passed);
            code = passed ? Activity.RESULT_OK : Activity.RESULT_CANCELED;
        } catch (Throwable error) {
            result.putString("stream", "\nFAIL late-jni listener app probe: " + error + "\n");
        } finally {
            finished.set(true);
        }
        finish(code, result);
    }
}
