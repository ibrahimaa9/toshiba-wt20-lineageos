/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.gamemode;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.GameManager;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.text.Collator;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** The game list and the per-game dialog (programmatic views, right-to-left Arabic UI). */
public class MainActivity extends Activity {
    private static final class Entry {
        final String pkg;
        final String label;
        final Drawable icon;
        /** CATEGORY_GAME: GameManagerService handles mode and FPS; otherwise scale only */
        final boolean systemGame;
        /** Added by hand (removable) */
        final boolean manual;

        Entry(String pkg, String label, Drawable icon, boolean systemGame, boolean manual) {
            this.pkg = pkg;
            this.label = label;
            this.icon = icon;
            this.systemGame = systemGame;
            this.manual = manual;
        }
    }

    private final ExecutorService mBg = Executors.newSingleThreadExecutor();
    private GameModeStore mStore;
    private GameModeController mController;
    private LinearLayout mList;
    private TextView mEmpty;
    private volatile Set<String> mShown = new HashSet<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mStore = new GameModeStore(this);
        mController = new GameModeController(this);

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

        mEmpty = text(getString(R.string.no_games),
                android.R.style.TextAppearance_DeviceDefault_Small);
        mEmpty.setPadding(0, dp(16), 0, 0);
        mEmpty.setVisibility(View.GONE);
        root.addView(mEmpty);

        mList = new LinearLayout(this);
        mList.setOrientation(LinearLayout.VERTICAL);
        mList.setPadding(0, dp(8), 0, dp(8));
        root.addView(mList);

        final Button add = new Button(this);
        add.setText(R.string.add_app);
        add.setOnClickListener(v -> showAddDialog());
        root.addView(add, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // Edge-to-edge (target SDK 36): keep the content clear of the system bars
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            final Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return WindowInsets.CONSUMED;
        });

        setContentView(scroll);
        getWindow().getDecorView().setLayoutDirection(View.LAYOUT_DIRECTION_RTL);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // A game update or a reboot drops non-standard modes in GameManagerService
        mBg.execute(() -> mController.reconcileAll(mStore));
        refresh();
    }

    @Override
    protected void onDestroy() {
        mBg.shutdown();
        super.onDestroy();
    }

    private void refresh() {
        mBg.execute(() -> {
            final List<Entry> entries = loadEntries();
            runOnUiThread(() -> {
                if (!isDestroyed()) {
                    show(entries);
                }
            });
        });
    }

    private List<Entry> loadEntries() {
        final PackageManager pm = getPackageManager();
        final Set<String> manual = mStore.manualPackages();
        final List<Entry> out = new ArrayList<>();
        for (ApplicationInfo ai : pm.getInstalledApplications(
                PackageManager.ApplicationInfoFlags.of(0))) {
            if (getPackageName().equals(ai.packageName)) {
                continue;
            }
            final boolean category = GameModeController.isGameCategory(ai);
            @SuppressWarnings("deprecation")
            final boolean game = category || (ai.flags & ApplicationInfo.FLAG_IS_GAME) != 0;
            final boolean byHand = manual.contains(ai.packageName);
            if (!game && !byHand) {
                continue;
            }
            if (!byHand && pm.getLaunchIntentForPackage(ai.packageName) == null) {
                continue;
            }
            out.add(new Entry(ai.packageName, ai.loadLabel(pm).toString(), ai.loadIcon(pm),
                    category, byHand && !game));
        }
        final Collator collator = Collator.getInstance();
        Collections.sort(out, (a, b) -> collator.compare(a.label, b.label));
        return out;
    }

    private void show(List<Entry> entries) {
        final Set<String> shown = new HashSet<>();
        mList.removeAllViews();
        for (Entry e : entries) {
            shown.add(e.pkg);
            mList.addView(row(e));
        }
        mShown = shown;
        mEmpty.setVisibility(entries.isEmpty() ? View.VISIBLE : View.GONE);
    }

    private View row(Entry e) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(64));
        row.setPadding(dp(8), dp(8), dp(8), dp(8));
        final TypedValue tv = new TypedValue();
        if (getTheme().resolveAttribute(android.R.attr.selectableItemBackground, tv, true)) {
            row.setBackgroundResource(tv.resourceId);
        }

        final ImageView icon = new ImageView(this);
        icon.setImageDrawable(e.icon);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        row.addView(icon, new LinearLayout.LayoutParams(dp(40), dp(40)));

        final LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMarginStart(dp(16));
        row.addView(texts, lp);
        texts.addView(text(e.label, android.R.style.TextAppearance_DeviceDefault_Medium));
        texts.addView(text(summary(e), android.R.style.TextAppearance_DeviceDefault_Small));

        row.setOnClickListener(v -> showGameDialog(e));
        if (e.manual) {
            row.setOnLongClickListener(v -> {
                confirmRemove(e);
                return true;
            });
        }
        return row;
    }

    private String summary(Entry e) {
        final Choice c = mStore.get(e.pkg);
        final List<String> parts = new ArrayList<>();
        if (c.isDefault()) {
            parts.add(getString(R.string.summary_default));
        } else {
            if (c.scalePct != 100) {
                parts.add(getString(R.string.summary_scale, c.scalePct));
            }
            if (c.fps != 0) {
                parts.add(getString(R.string.summary_fps, c.fps));
            }
            if (c.mode == GameManager.GAME_MODE_PERFORMANCE) {
                parts.add(getString(R.string.mode_performance));
            } else if (c.mode == GameManager.GAME_MODE_BATTERY) {
                parts.add(getString(R.string.mode_battery));
            }
        }
        if (!e.systemGame) {
            parts.add(getString(R.string.summary_only_scale));
        }
        if (e.manual) {
            parts.add(getString(R.string.manual_tag));
        }
        return String.join(" · ", parts);
    }

    private void showGameDialog(Entry e) {
        final Choice current = mStore.get(e.pkg);
        final LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(8), dp(24), dp(8));

        final String[] scaleLabels = new String[Choice.SCALES.length];
        for (int i = 0; i < Choice.SCALES.length; i++) {
            scaleLabels[i] = Choice.SCALES[i] == 100 ? getString(R.string.scale_default)
                    : getString(R.string.scale_value, Choice.SCALES[i]);
        }
        final RadioGroup scales = group(box, R.string.section_scale, scaleLabels,
                Choice.indexOf(Choice.SCALES, current.scalePct));

        RadioGroup fps = null;
        RadioGroup modes = null;
        if (e.systemGame) {
            final String[] fpsLabels = new String[Choice.FPS.length];
            for (int i = 0; i < Choice.FPS.length; i++) {
                fpsLabels[i] = Choice.FPS[i] == 0 ? getString(R.string.fps_none)
                        : getString(R.string.fps_value, Choice.FPS[i]);
            }
            fps = group(box, R.string.section_fps, fpsLabels,
                    Choice.indexOf(Choice.FPS, current.fps));
            note(box, R.string.fps_note);

            final String[] modeLabels = {
                getString(R.string.mode_standard_default),
                getString(R.string.mode_performance),
                getString(R.string.mode_battery),
            };
            modes = group(box, R.string.section_mode, modeLabels,
                    Choice.indexOf(Choice.MODES, current.mode));
        } else {
            note(box, R.string.only_scale_note);
        }
        note(box, R.string.restart_note);

        final ScrollView sv = new ScrollView(this);
        sv.addView(box);
        final RadioGroup fpsGroup = fps;
        final RadioGroup modeGroup = modes;
        final AlertDialog d = new AlertDialog.Builder(this)
                .setTitle(e.label)
                .setView(sv)
                .setPositiveButton(R.string.apply, (di, w) -> save(e, new Choice(
                        modeGroup == null ? GameManager.GAME_MODE_STANDARD
                                : Choice.MODES[checked(modeGroup)],
                        Choice.SCALES[checked(scales)],
                        fpsGroup == null ? 0 : Choice.FPS[checked(fpsGroup)])))
                .setNeutralButton(R.string.reset, (di, w) -> save(e, Choice.DEFAULT))
                .setNegativeButton(R.string.cancel, null)
                .create();
        d.show();
        d.getWindow().getDecorView().setLayoutDirection(View.LAYOUT_DIRECTION_RTL);
    }

    private void save(Entry e, Choice choice) {
        final Choice previous = mStore.get(e.pkg);
        if (previous.sameAs(choice)) {
            return; // nothing changes unless the owner picks something new
        }
        mBg.execute(() -> {
            final int result = mController.apply(e.pkg, choice);
            mStore.put(e.pkg, choice);
            runOnUiThread(() -> {
                final int msg;
                if (result == GameModeController.RESULT_FAILED) {
                    msg = R.string.failed;
                } else if (result == GameModeController.RESULT_NO_FPS) {
                    msg = R.string.fps_failed;
                } else if (previous.scalePct != choice.scalePct) {
                    msg = R.string.saved_restart;
                } else {
                    msg = R.string.saved;
                }
                Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
                refresh();
            });
        });
    }

    private void showAddDialog() {
        final Set<String> listed = mShown;
        mBg.execute(() -> {
            final PackageManager pm = getPackageManager();
            final Intent launcher = new Intent(Intent.ACTION_MAIN)
                    .addCategory(Intent.CATEGORY_LAUNCHER);
            final List<String[]> apps = new ArrayList<>(); // {label, package}
            final Set<String> seen = new HashSet<>(listed);
            seen.add(getPackageName());
            for (ResolveInfo ri : pm.queryIntentActivities(launcher,
                    PackageManager.ResolveInfoFlags.of(0))) {
                final String pkg = ri.activityInfo.packageName;
                if (seen.add(pkg)) {
                    apps.add(new String[] {ri.loadLabel(pm).toString(), pkg});
                }
            }
            final Collator collator = Collator.getInstance();
            Collections.sort(apps, (a, b) -> collator.compare(a[0], b[0]));
            runOnUiThread(() -> {
                if (isDestroyed()) {
                    return;
                }
                if (apps.isEmpty()) {
                    Toast.makeText(this, R.string.no_more_apps, Toast.LENGTH_SHORT).show();
                    return;
                }
                final String[] labels = new String[apps.size()];
                for (int i = 0; i < labels.length; i++) {
                    labels[i] = apps.get(i)[0];
                }
                final AlertDialog d = new AlertDialog.Builder(this)
                        .setTitle(R.string.add_app_title)
                        .setItems(labels, (di, which) -> {
                            final String pkg = apps.get(which)[1];
                            mBg.execute(() -> {
                                mStore.addManual(pkg);
                                runOnUiThread(this::refresh);
                            });
                        })
                        .setNegativeButton(R.string.cancel, null)
                        .create();
                d.show();
                d.getWindow().getDecorView().setLayoutDirection(View.LAYOUT_DIRECTION_RTL);
            });
        });
    }

    private void confirmRemove(Entry e) {
        final AlertDialog d = new AlertDialog.Builder(this)
                .setMessage(getString(R.string.remove_confirm, e.label))
                .setPositiveButton(R.string.remove, (di, w) -> mBg.execute(() -> {
                    if (!mStore.get(e.pkg).isDefault()) {
                        mController.apply(e.pkg, Choice.DEFAULT);
                        mStore.put(e.pkg, Choice.DEFAULT);
                    }
                    mStore.removeManual(e.pkg);
                    runOnUiThread(this::refresh);
                }))
                .setNegativeButton(R.string.cancel, null)
                .create();
        d.show();
        d.getWindow().getDecorView().setLayoutDirection(View.LAYOUT_DIRECTION_RTL);
    }

    private RadioGroup group(LinearLayout parent, int title, String[] labels, int checked) {
        final TextView header = text(getString(title),
                android.R.style.TextAppearance_DeviceDefault_Medium);
        header.setTypeface(header.getTypeface(), Typeface.BOLD);
        header.setPadding(0, dp(16), 0, dp(4));
        parent.addView(header);

        final RadioGroup group = new RadioGroup(this);
        group.setOrientation(RadioGroup.VERTICAL);
        int checkedId = View.NO_ID;
        for (int i = 0; i < labels.length; i++) {
            final RadioButton b = new RadioButton(this);
            b.setId(View.generateViewId());
            b.setText(labels[i]);
            group.addView(b);
            if (i == checked) {
                checkedId = b.getId();
            }
        }
        group.check(checkedId);
        parent.addView(group);
        return group;
    }

    private static int checked(RadioGroup group) {
        final View v = group.findViewById(group.getCheckedRadioButtonId());
        return v == null ? 0 : Math.max(0, group.indexOfChild(v));
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
