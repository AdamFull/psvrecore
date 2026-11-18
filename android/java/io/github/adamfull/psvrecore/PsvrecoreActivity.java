package io.github.adamfull.psvrecore;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.ImageView;

import java.io.InputStream;

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
 *
 * The launch screen: when the package has one (scripts/android_apk.py: the colour psvrecore_launch and the asset
 * launch.png, made from the game's LiveArea), the picture is shown over SDL's surface from the activity's first
 * frame, as the native side shows it (as large as fits, the bars in that colour), until the native side's first
 * image is on the screen (hideLaunchPicture); the native side then shows it until the game's first frame. The picture
 * is read on a thread of its own: until then, and if it is slow, the colour alone.
 */
public class PsvrecoreActivity extends SDLActivity {
    private static native void nativeSetBackground(boolean background);

    // (the native side's first image presented in this process: no launch picture over it again)
    private static volatile boolean nativeShown;
    private ImageView launchView;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        showLaunchPicture();
    }

    private void showLaunchPicture() {
        int colour = getResources().getIdentifier("psvrecore_launch", "color", getPackageName());
        if (colour == 0 || nativeShown || mLayout == null) return;
        final ImageView view = new ImageView(this);
        view.setScaleType(ImageView.ScaleType.FIT_CENTER);
        view.setBackgroundColor(getResources().getColor(colour, null));
        mLayout.addView(view, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        launchView = view;
        new Thread(new Runnable() {
            @Override
            public void run() {
                final Bitmap picture;
                try (InputStream in = getAssets().open("launch.png")) {
                    picture = BitmapFactory.decodeStream(in);
                } catch (Exception e) {
                    return;
                }
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (launchView == view && picture != null) view.setImageBitmap(picture);
                    }
                });
            }
        }, "psvrecore launch picture").start();
    }

    // From the native side (its presenting thread): its first image is on the screen.
    public void hideLaunchPicture() {
        nativeShown = true;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (launchView == null) return;
                if (mLayout != null) mLayout.removeView(launchView);
                launchView = null;
            }
        });
    }

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
