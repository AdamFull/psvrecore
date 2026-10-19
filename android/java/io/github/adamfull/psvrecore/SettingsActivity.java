package io.github.adamfull.psvrecore;

import android.app.Activity;
import android.os.Bundle;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * The settings, as the frontend's settings file has them (settings.ini in the application's folder): built from the
 * schema the native side gives (each key's kind, choices or range, and whether it applies while the game runs), each
 * change written at once (the native side checks it; a refused one is shown and not kept).
 *
 * Opened from the launcher, or over the game (the "playing" extra: the game is suspended meanwhile, and settings that
 * apply only when it starts are marked so). The settings that mean nothing on a phone are left out.
 */
public class SettingsActivity extends Activity {
    static {
        System.loadLibrary("main");
    }

    private static native String nativeSchema(String file);
    private static native String nativeSet(String file, String section, String key, String value);

    private String file;
    private boolean playing;

    private static String title(String s) {
        String t = s.replace('_', ' ');
        if (t.endsWith(" ms")) t = t.substring(0, t.length() - 3) + " (ms)";
        return t.isEmpty() ? t : Character.toUpperCase(t.charAt(0)) + t.substring(1);
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private boolean set(String section, String key, String value) {
        String refused = nativeSet(file, section, key, value);
        if (refused != null) {
            Toast.makeText(this, refused, Toast.LENGTH_LONG).show();
            return false;
        }
        return true;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        file = new File(getExternalFilesDir(null), "settings.ini").getPath();
        playing = getIntent().getBooleanExtra("playing", false);

        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(24), dp(16), dp(24), dp(24));
        TextView heading = new TextView(this);
        heading.setText("Settings");
        heading.setTextSize(24);
        list.addView(heading);
        if (playing) {
            TextView note = new TextView(this);
            note.setText("The game is paused. Settings marked \"at the next start\" apply when it starts again.");
            note.setPadding(0, dp(4), 0, dp(8));
            list.addView(note);
        }

        String section = "";
        for (String line : nativeSchema(file).split("\n")) {
            final String[] f = line.split("\t", -1);
            if (f.length < 8 || f[5].equals("1")) continue;  // (malformed, or for desktops only)
            final String sec = f[0], key = f[1], kind = f[2], extra = f[3], help = f[6], value = f[7];
            final boolean live = f[4].equals("1");
            if (!sec.equals(section)) {
                section = sec;
                TextView header = new TextView(this);
                header.setText(title(sec));
                header.setTextSize(18);
                header.setPadding(0, dp(20), 0, dp(4));
                list.addView(header);
            }
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(0, dp(8), 0, dp(8));
            TextView name = new TextView(this);
            name.setText(title(key) + (playing && !live ? "  (at the next start)" : ""));
            name.setTextSize(16);
            row.addView(name);
            TextView about = new TextView(this);
            about.setText(help);
            about.setAlpha(0.7f);
            row.addView(about);
            row.addView(widget(sec, key, kind, extra, value));
            list.addView(row);
        }

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.END);
        buttons.setPadding(0, dp(24), 0, 0);
        if (playing) {
            Button quit = new Button(this);
            quit.setText("Quit the game");
            quit.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    finishAffinity();  // (the game's activity too: it ends the program)
                }
            });
            buttons.addView(quit);
        }
        Button done = new Button(this);
        done.setText(playing ? "Back to the game" : "Done");
        done.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
        buttons.addView(done);
        list.addView(buttons);

        ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true);  // (clear of the status and gesture bars)
        scroll.addView(list);
        setContentView(scroll);
    }

    private View widget(final String section, final String key, String kind, String extra, final String value) {
        if (kind.equals("bool")) {
            final Switch s = new Switch(this);
            s.setChecked(value.equals("true"));
            s.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
                @Override
                public void onCheckedChanged(android.widget.CompoundButton b, boolean on) {
                    if (!set(section, key, on ? "true" : "false")) s.setChecked(!on);
                }
            });
            return s;
        }
        if (kind.equals("choice")) {
            final String[] choices = extra.split(",");
            Spinner spinner = new Spinner(this);
            ArrayAdapter<String> adapter = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, choices);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            spinner.setAdapter(adapter);
            for (int i = 0; i < choices.length; ++i)
                if (choices[i].equals(value)) spinner.setSelection(i, false);
            spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                private String current = value;

                @Override
                public void onItemSelected(AdapterView<?> parent, View v, int position, long id) {
                    if (choices[position].equals(current)) return;
                    if (set(section, key, choices[position])) current = choices[position];
                }

                @Override
                public void onNothingSelected(AdapterView<?> parent) {
                }
            });
            return spinner;
        }
        if (kind.equals("int") || kind.equals("float")) {
            final boolean integer = kind.equals("int");
            final String[] range = extra.split(",");
            final float lo = Float.parseFloat(range[0]), hi = Float.parseFloat(range[1]);
            final float step = integer ? 1 : 0.05f;
            LinearLayout line = new LinearLayout(this);
            line.setOrientation(LinearLayout.HORIZONTAL);
            line.setGravity(Gravity.CENTER_VERTICAL);
            final TextView shown = new TextView(this);
            shown.setMinWidth(dp(56));
            shown.setText(value);
            final SeekBar bar = new SeekBar(this);
            bar.setMax(Math.round((hi - lo) / step));
            float v = lo;
            try {
                v = Float.parseFloat(value);
            } catch (NumberFormatException e) {
                // (the bar at its start)
            }
            bar.setProgress(Math.round((v - lo) / step));
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                private String text(int progress) {
                    float x = lo + progress * step;
                    return integer ? Integer.toString(Math.round(x)) : String.format(java.util.Locale.ROOT, "%.2f", x);
                }

                @Override
                public void onProgressChanged(SeekBar b, int progress, boolean user) {
                    shown.setText(text(progress));
                }

                @Override
                public void onStartTrackingTouch(SeekBar b) {
                }

                @Override
                public void onStopTrackingTouch(SeekBar b) {
                    set(section, key, text(b.getProgress()));
                }
            });
            line.addView(shown);
            line.addView(bar, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            return line;
        }
        // Text (and folders: on a phone the launcher chooses the game's).
        final EditText edit = new EditText(this);
        edit.setSingleLine(true);
        edit.setInputType(InputType.TYPE_CLASS_TEXT);
        edit.setImeOptions(EditorInfo.IME_ACTION_DONE);
        edit.setText(value);
        edit.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int action, KeyEvent event) {
                set(section, key, v.getText().toString());
                return false;
            }
        });
        edit.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override
            public void onFocusChange(View v, boolean focused) {
                if (!focused) set(section, key, edit.getText().toString());
            }
        });
        return edit;
    }
}
