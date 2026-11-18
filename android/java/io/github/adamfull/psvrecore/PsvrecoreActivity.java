package io.github.adamfull.psvrecore;

import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;

import org.libsdl.app.SDLActivity;

/**
 * The activity a game (or sample) built on psvrecore runs in: SDL's, loading libmain.so, where SDL is linked in
 * statically and the frontend is the program. Its command line is the intent's "args" string array, empty when
 * launched from the home screen (am start ... --esa args --timeout,10).
 *
 * The settings (SettingsActivity) open over it from the on-screen menu button or Back (the native side calls
 * openSettings), the game suspended meanwhile.
 *
 * Paused, it suspends the game and lets the window's surface go at once, on this thread, before the system takes
 * the surface away (SDL's own notice of the pause reaches the game's side only after that); resumed, it lets the
 * game go on.
 */
public class PsvrecoreActivity extends SDLActivity {
    private static native void nativeSetBackground(boolean background);

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        String[] args = intent != null ? intent.getStringArrayExtra("args") : null;
        return args != null ? args : new String[0];
    }

    // From the game's side (any thread).
    public void openSettings() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent intent = new Intent(PsvrecoreActivity.this, SettingsActivity.class);
                intent.putExtra("playing", true);
                startActivity(intent);
            }
        });
    }

    private WifiManager.MulticastLock multicastLock;

    // From the game's side (any thread): held while the game's ad hoc connection is up, so the Wi-Fi chip passes on
    // the broadcasts other players send (it may filter them otherwise). Needs CHANGE_WIFI_MULTICAST_STATE in the
    // game's manifest; without it nothing is held.
    public synchronized void setMulticastLock(boolean held) {
        try {
            if (held && multicastLock == null) {
                WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wifi == null) return;
                multicastLock = wifi.createMulticastLock("psvrecore ad hoc");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            } else if (!held && multicastLock != null) {
                multicastLock.release();
                multicastLock = null;
            }
        } catch (SecurityException e) {
            multicastLock = null;
        }
    }

    @Override
    protected void onDestroy() {
        setMulticastLock(false);
        super.onDestroy();
    }

    @Override
    protected void onPause() {
        setBackground(true);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        setBackground(false);
    }

    private static void setBackground(boolean background) {
        try {
            nativeSetBackground(background);
        } catch (UnsatisfiedLinkError e) {
            // (the library not loaded: nothing runs yet)
        }
    }
}
