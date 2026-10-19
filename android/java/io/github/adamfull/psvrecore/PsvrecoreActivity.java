package io.github.adamfull.psvrecore;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

/**
 * The activity a game (or sample) built on psvrecore runs in: SDL's, loading libmain.so, where SDL is linked in
 * statically and the frontend is the program. Its command line is the intent's "args" string array, empty when
 * launched from the home screen (am start ... --esa args --timeout,10).
 */
public class PsvrecoreActivity extends SDLActivity {
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
}
