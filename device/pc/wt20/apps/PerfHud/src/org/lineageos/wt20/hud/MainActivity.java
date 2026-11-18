/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.Manifest;
import android.app.Activity;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.function.IntConsumer;

/** The HUD's settings (programmatic views; the layout follows the locale, RTL in Arabic). */
public class MainActivity extends Activity
        implements SharedPreferences.OnSharedPreferenceChangeListener {
    /** In the order of Prefs.M_* */
    private static final int[] METRIC_LABELS = {
        R.string.metric_fps, R.string.metric_cpu, R.string.metric_cpu_cores,
        R.string.metric_cpu_freq, R.string.metric_cpu_temp, R.string.metric_board_temp,
        R.string.metric_gpu_freq, R.string.metric_gpu_load, R.string.metric_ram,
        R.string.metric_swap, R.string.metric_battery, R.string.metric_battery_power,
        R.string.metric_clock,
    };

    private SharedPreferences mPrefs;
    private Switch mOn;
    private Switch mLock;
    private TextView mWindow;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mPrefs = Prefs.get(this);

        final ScrollView scroll = new ScrollView(this);
        scroll.setClipToPadding(false);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        final int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        root.addView(text(getString(R.string.intro),
                android.R.style.TextAppearance_DeviceDefault_Medium));

        mOn = toggle(root, R.string.show_hud, mPrefs.getBoolean(Prefs.KEY_ENABLED, false));
        mOn.setOnCheckedChangeListener((b, checked) -> {
            if (checked == mPrefs.getBoolean(Prefs.KEY_ENABLED, false)) {
                return; // sync() from the tile or the notification
            }
            if (!HudService.setEnabled(this, checked)) {
                Toast.makeText(this, R.string.start_failed, Toast.LENGTH_LONG).show();
                b.setChecked(false);
            }
        });
        mWindow = text("", android.R.style.TextAppearance_DeviceDefault_Small);
        root.addView(mWindow);

        header(root, R.string.section_metrics);
        for (int i = 0; i < Prefs.M_COUNT; i++) {
            final String key = Prefs.metricKey(i);
            final CheckBox box = new CheckBox(this);
            box.setText(METRIC_LABELS[i]);
            box.setChecked(Prefs.metric(mPrefs, i));
            box.setOnCheckedChangeListener(
                    (b, checked) -> mPrefs.edit().putBoolean(key, checked).apply());
            root.addView(box);
        }
        note(root, R.string.fps_note);

        header(root, R.string.section_position);
        mLock = toggle(root, R.string.lock, mPrefs.getBoolean(Prefs.KEY_LOCKED, true));
        mLock.setOnCheckedChangeListener(
                (b, checked) -> mPrefs.edit().putBoolean(Prefs.KEY_LOCKED, checked).apply());
        note(root, R.string.lock_note);
        final Button reset = new Button(this);
        reset.setText(R.string.reset_position);
        reset.setOnClickListener(
                v -> mPrefs.edit().remove(Prefs.KEY_X).remove(Prefs.KEY_Y).apply());
        root.addView(reset, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        header(root, R.string.section_size);
        choices(root, new String[] {getString(R.string.size_small),
                getString(R.string.size_medium), getString(R.string.size_large)},
                Prefs.textSize(mPrefs), i -> mPrefs.edit().putInt(Prefs.KEY_SIZE, i).apply());

        header(root, R.string.section_opacity);
        final String[] opacity = new String[Prefs.OPACITY.length];
        for (int i = 0; i < opacity.length; i++) {
            opacity[i] = getString(R.string.percent, Prefs.OPACITY[i]);
        }
        choices(root, opacity, Prefs.indexOf(Prefs.OPACITY, Prefs.opacity(mPrefs)),
                i -> mPrefs.edit().putInt(Prefs.KEY_OPACITY, Prefs.OPACITY[i]).apply());

        header(root, R.string.section_interval);
        choices(root, new String[] {getString(R.string.interval_half),
                getString(R.string.interval_one), getString(R.string.interval_two)},
                Prefs.indexOf(Prefs.INTERVAL_MS, Prefs.interval(mPrefs)),
                i -> mPrefs.edit().putInt(Prefs.KEY_INTERVAL, Prefs.INTERVAL_MS[i]).apply());

        // Edge-to-edge (target SDK 36): keep the content clear of the system bars
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            final Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return WindowInsets.CONSUMED;
        });
        setContentView(scroll);

        // The service's notification (its "hide" button); the HUD itself works without it
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 0);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        mPrefs.registerOnSharedPreferenceChangeListener(this);
        sync();
    }

    @Override
    protected void onPause() {
        mPrefs.unregisterOnSharedPreferenceChangeListener(this);
        super.onPause();
    }

    @Override
    public void onSharedPreferenceChanged(SharedPreferences p, String key) {
        sync(); // the tile, the notification or the service changed something
    }

    private void sync() {
        final boolean on = mPrefs.getBoolean(Prefs.KEY_ENABLED, false);
        if (mOn.isChecked() != on) {
            mOn.setChecked(on);
        }
        final boolean locked = mPrefs.getBoolean(Prefs.KEY_LOCKED, true);
        if (mLock.isChecked() != locked) {
            mLock.setChecked(locked);
        }
        final String window = on ? mPrefs.getString(Prefs.KEY_WINDOW, "") : "";
        final int res;
        switch (window) {
            case Prefs.WINDOW_SECURE:
                res = R.string.window_secure;
                break;
            case Prefs.WINDOW_SYSTEM:
                res = R.string.window_system;
                break;
            case Prefs.WINDOW_OVERLAY:
                res = R.string.window_overlay;
                break;
            case Prefs.WINDOW_NONE:
                res = R.string.window_none;
                break;
            default:
                res = 0;
                break;
        }
        mWindow.setVisibility(res == 0 ? View.GONE : View.VISIBLE);
        if (res != 0) {
            mWindow.setText(res);
        }
    }

    private Switch toggle(LinearLayout parent, int label, boolean checked) {
        final Switch s = new Switch(this);
        s.setText(label);
        s.setTextAppearance(android.R.style.TextAppearance_DeviceDefault_Medium);
        s.setChecked(checked);
        s.setPadding(0, dp(12), 0, dp(12));
        parent.addView(s, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return s;
    }

    private void choices(LinearLayout parent, String[] labels, int checked, IntConsumer pick) {
        final RadioGroup group = new RadioGroup(this);
        group.setOrientation(RadioGroup.HORIZONTAL);
        for (int i = 0; i < labels.length; i++) {
            final RadioButton b = new RadioButton(this);
            b.setId(View.generateViewId());
            b.setText(labels[i]);
            final int index = i;
            b.setOnClickListener(v -> pick.accept(index));
            final RadioGroup.LayoutParams lp = new RadioGroup.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMarginEnd(dp(16));
            group.addView(b, lp);
            if (i == checked) {
                group.check(b.getId());
            }
        }
        parent.addView(group);
    }

    private void header(LinearLayout parent, int title) {
        final TextView t = text(getString(title),
                android.R.style.TextAppearance_DeviceDefault_Medium);
        t.setTypeface(t.getTypeface(), Typeface.BOLD);
        t.setPadding(0, dp(20), 0, dp(4));
        parent.addView(t);
    }

    private void note(LinearLayout parent, int res) {
        final TextView t = text(getString(res), android.R.style.TextAppearance_DeviceDefault_Small);
        t.setPadding(0, dp(4), 0, 0);
        parent.addView(t);
    }

    private TextView text(String s, int appearance) {
        final TextView t = new TextView(this);
        t.setTextAppearance(appearance);
        t.setText(s);
        return t;
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics()));
    }
}
