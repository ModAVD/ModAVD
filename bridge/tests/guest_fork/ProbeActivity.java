// Copyright 2026 Ji Mao Cai
package org.modavd.tests.guestfork;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

public final class ProbeActivity extends Activity {
    private static native String runProbe();
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("ModavdGuestForkProbe", "START hostPid=" + android.os.Process.myPid());
        final TextView text = new TextView(this);
        text.setText("Running ARM64 guest fork probe");
        setContentView(text);
        new Thread(new Runnable() {
            @Override public void run() {
                String value;
                try {
                    System.loadLibrary("guest_fork_probe");
                    value = runProbe();
                } catch (Throwable error) {
                    value = "FAIL " + error.toString();
                }
                final String result = value;
                Log.i("ModavdGuestForkProbe", result);
                runOnUiThread(new Runnable() {
                    @Override public void run() { text.setText(result); }
                });
            }
        }, "guest-fork-probe").start();
    }
}
