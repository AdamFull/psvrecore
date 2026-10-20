package io.github.adamfull.psvrecore;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Where a game built on psvrecore starts on Android: Play (the game, PsvrecoreActivity), Settings
 * (SettingsActivity), and the game's application folder chosen with the system's folder picker and copied into the
 * application's own folder (game/), which Play needs. With an "args" extra (a test's command line) it goes straight on
 * to the game.
 *
 * The files are copied, not read where they are: the application may read only files it made itself (the ones adb
 * or a computer puts in its folder belong to someone else).
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;

    private TextView message;
    private Button play, settings, choose;
    private volatile boolean copying;

    private File gameFolder() {
        return new File(getExternalFilesDir(null), "game");
    }

    // A game folder put in the application's internal storage from a computer (adb's run-as reaches only that):
    // used when the shared folder has no game.
    private File deployedFolder() {
        return new File(getFilesDir(), "game");
    }

    // Whether a folder holds the game's files: an application folder (eboot.bin at its top), or a copy of the
    // console's ux0: folders (app/<title ID>/eboot.bin, with patch/ and addcont/ beside it).
    private static boolean holdsGame(File folder) {
        if (new File(folder, "eboot.bin").isFile()) return true;
        File[] titles = new File(folder, "app").listFiles();
        if (titles != null)
            for (File t : titles)
                if (new File(t, "eboot.bin").isFile()) return true;
        return false;
    }

    private boolean gamePresent() {
        return holdsGame(gameFolder()) || holdsGame(deployedFolder());
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (getIntent().hasExtra("args")) {
            startGame();
            return;
        }
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(Color.BLACK);
        layout.setPadding(64, 64, 64, 64);
        TextView name = new TextView(this);
        name.setTextColor(Color.WHITE);
        name.setTextSize(28);
        name.setGravity(Gravity.CENTER);
        name.setText(getApplicationInfo().loadLabel(getPackageManager()));
        message = new TextView(this);
        message.setTextColor(Color.WHITE);
        message.setTextSize(16);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, 24, 0, 24);
        play = button("Play", new Runnable() {
            @Override
            public void run() {
                startGame();
            }
        });
        settings = button("Settings", new Runnable() {
            @Override
            public void run() {
                startActivity(new Intent(LauncherActivity.this, SettingsActivity.class));
            }
        });
        choose = button("Choose the game's folder", new Runnable() {
            @Override
            public void run() {
                pick();
            }
        });
        layout.addView(name);
        layout.addView(message);
        layout.addView(play);
        layout.addView(settings);
        layout.addView(choose);
        setContentView(layout);
        update();
    }

    private Button button(String text, final Runnable action) {
        Button b = new Button(this);
        b.setText(text);
        b.setMinWidth(560);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                action.run();
            }
        });
        return b;
    }

    // What the menu says and offers, as the game's files are there or not.
    private void update() {
        boolean present = gamePresent();
        play.setEnabled(present && !copying);
        settings.setEnabled(!copying);
        choose.setEnabled(!copying);
        choose.setText(present ? "Choose the game's folder again" : "Choose the game's folder");
        message.setText(present ? "The game's files are in place."
                                : "The game's files are not here yet: choose the game's application folder (the one "
                                  + "that holds eboot.bin and sce_module). It is copied into this app's storage.");
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent, PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri tree = data.getData();
        copying = true;
        update();
        new Thread(new Runnable() {
            @Override
            public void run() {
                importFrom(tree);
            }
        }).start();
    }

    // (on its own thread)
    private void importFrom(Uri tree) {
        String outcome;
        File staging = new File(getExternalFilesDir(null), "game.importing");
        try {
            deleteTree(staging);
            String root = DocumentsContract.getTreeDocumentId(tree);
            long[] counts = new long[2];  // (files, bytes)
            copyTree(getContentResolver(), tree, root, staging, counts);
            if (!new File(staging, "eboot.bin").isFile()) {
                deleteTree(staging);
                outcome = "That folder has no eboot.bin: it is not a game's application folder. Choose another.";
            } else {
                deleteTree(gameFolder());
                if (!staging.renameTo(gameFolder())) throw new IOException("could not move the copy in place");
                outcome = null;
            }
        } catch (IOException | RuntimeException e) {
            deleteTree(staging);
            outcome = "The copy failed: " + e.getMessage();
        }
        final String shown = outcome;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                copying = false;
                update();
                if (shown == null) startGame();
                else message.setText(shown);
            }
        });
    }

    private void copyTree(ContentResolver resolver, Uri tree, String document, File to, long[] counts)
        throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("could not make " + to);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, document);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = resolver.query(children, columns, null, null, null)) {
            if (c == null) throw new IOException("the folder cannot be listed");
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), type = c.getString(2);
                if (name == null || name.contains("/") || name.equals("..")) continue;
                File target = new File(to, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(type)) {
                    copyTree(resolver, tree, id, target, counts);
                    continue;
                }
                try (InputStream in = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                     OutputStream out = new FileOutputStream(target)) {
                    if (in == null) throw new IOException("cannot read " + name);
                    byte[] buffer = new byte[1 << 20];
                    for (int n; (n = in.read(buffer)) > 0;) {
                        out.write(buffer, 0, n);
                        counts[1] += n;
                    }
                }
                counts[0]++;
                final long files = counts[0], megabytes = counts[1] >> 20;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        message.setText("Copying the game's files: " + files + " files, " + megabytes + " MB so far...");
                    }
                });
            }
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null)
            for (File c : children) deleteTree(c);
        f.delete();
    }

    private void startGame() {
        Intent intent = new Intent(this, PsvrecoreActivity.class);
        if (getIntent().getExtras() != null) intent.putExtras(getIntent().getExtras());
        // (the game a computer put in internal storage, when the shared folder has none: named on the command line)
        if (!intent.hasExtra("args") && !holdsGame(gameFolder()) && holdsGame(deployedFolder()))
            intent.putExtra("args", new String[] {"--game", deployedFolder().getAbsolutePath()});
        startActivity(intent);
        finish();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (play != null) update();
    }

    @Override
    public void onBackPressed() {
        if (!copying) super.onBackPressed();
    }
}
