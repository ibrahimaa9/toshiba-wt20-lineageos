/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.os.BatteryManager;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;

import java.util.Locale;

/**
 * Turns a {@link Sample} into the overlay's lines: a 3-letter label, then fixed-width values
 * (so the window does not change size every second), coloured by thresholds. Digits are
 * always Latin (Locale.US), also with the Arabic UI.
 */
final class HudFormatter {
    static final int LABEL = 0xFFFFAB40; // orange, as in Afterburner
    static final int PLAIN = 0xFFFFFFFF;
    static final int GOOD = 0xFF69F0AE;
    static final int WARN = 0xFFFFD740;
    static final int BAD = 0xFFFF5252;
    static final int UNKNOWN = 0xFF9E9E9E;

    // Yellow from the first value, red from the second
    private static final int LOAD_WARN = 70;
    private static final int LOAD_BAD = 90;
    private static final int TEMP_WARN = 75;
    private static final int TEMP_BAD = 90;
    private static final int RAM_WARN = 80;
    private static final int RAM_BAD = 90;
    // Low battery: the other way round
    private static final int BATTERY_WARN = 30;
    private static final int BATTERY_BAD = 15;

    private static final double KB_PER_GB = 1024.0 * 1024.0;
    private static final String NA = "--";

    private final SpannableStringBuilder mOut = new SpannableStringBuilder();

    private HudFormatter() {}

    static CharSequence format(Sample s, boolean[] m) {
        final HudFormatter f = new HudFormatter();
        f.build(s, m);
        return f.mOut.length() > 0 ? f.mOut : "WT20 HUD";
    }

    private void build(Sample s, boolean[] m) {
        if (m[Prefs.M_FPS]) {
            label("FPS");
            item(pad(s.fps, 3), s.fps == Sample.NA ? UNKNOWN : PLAIN);
        }
        if (m[Prefs.M_CPU] || m[Prefs.M_CPU_CORES]) {
            label("CPU");
            if (m[Prefs.M_CPU]) {
                item(pad(s.cpuLoad, 3) + "%", level(s.cpuLoad, LOAD_WARN, LOAD_BAD));
            }
            if (m[Prefs.M_CPU_CORES]) {
                if (s.coreLoad == null) {
                    item(NA, UNKNOWN);
                } else {
                    for (int v : s.coreLoad) {
                        item(pad(v, 3), level(v, LOAD_WARN, LOAD_BAD));
                    }
                }
            }
        }
        if (m[Prefs.M_CPU_FREQ]) {
            label("FRQ");
            if (s.coreMhz == null) {
                item(NA, UNKNOWN);
            } else {
                for (int mhz : s.coreMhz) {
                    item(mhz == Sample.NA ? "-.-" : String.format(Locale.US, "%.1f", mhz / 1000f),
                            mhz == Sample.NA ? UNKNOWN : PLAIN);
                }
                add("GHz", LABEL);
            }
        }
        if (m[Prefs.M_CPU_TEMP] || m[Prefs.M_BOARD_TEMP]) {
            label("TMP");
            if (m[Prefs.M_CPU_TEMP]) {
                item("CPU", LABEL);
                temperature(s.cpuTemp);
            }
            if (m[Prefs.M_BOARD_TEMP]) {
                item("BRD", LABEL);
                temperature(s.boardTemp);
            }
        }
        if (m[Prefs.M_GPU_FREQ] || m[Prefs.M_GPU_LOAD]) {
            label("GPU");
            if (m[Prefs.M_GPU_FREQ]) {
                item(pad(s.gpuMhz, 4) + "MHz", s.gpuMhz == Sample.NA ? UNKNOWN : PLAIN);
            }
            if (m[Prefs.M_GPU_LOAD]) {
                item(pad(s.gpuLoad, 3) + "%", level(s.gpuLoad, LOAD_WARN, LOAD_BAD));
            }
        }
        if (m[Prefs.M_RAM] || m[Prefs.M_SWAP]) {
            label("RAM");
            if (m[Prefs.M_RAM]) {
                if (s.ramTotalKb > 0 && s.ramUsedKb >= 0) {
                    final int pct = (int) Math.round(s.ramUsedKb * 100.0 / s.ramTotalKb);
                    final int color = level(pct, RAM_WARN, RAM_BAD);
                    item(String.format(Locale.US, "%.1f/%.1fG", s.ramUsedKb / KB_PER_GB,
                            s.ramTotalKb / KB_PER_GB), color);
                    item(pad(pct, 3) + "%", color);
                } else {
                    item(NA, UNKNOWN);
                }
            }
            if (m[Prefs.M_SWAP]) {
                item("SWP", LABEL);
                if (s.swapUsedKb >= 0) {
                    item(String.format(Locale.US, "%.1fG", s.swapUsedKb / KB_PER_GB), PLAIN);
                } else {
                    item(NA, UNKNOWN);
                }
            }
        }
        if (m[Prefs.M_BATTERY] || m[Prefs.M_BATTERY_POWER]) {
            label("BAT");
            if (m[Prefs.M_BATTERY]) {
                final int pct = s.batteryPct;
                item(pad(pct, 3) + "%", pct == Sample.NA ? UNKNOWN
                        : pct <= BATTERY_BAD ? BAD : pct <= BATTERY_WARN ? WARN : GOOD);
                final String state = chargeState(s);
                if (state != null) {
                    item(state, GOOD);
                }
            }
            if (m[Prefs.M_BATTERY_POWER]) {
                if (Float.isNaN(s.batteryWatts)) {
                    item(NA + "W", UNKNOWN);
                } else {
                    item(String.format(Locale.US, "%+.1fW", s.batteryWatts), PLAIN);
                }
            }
        }
        if (m[Prefs.M_CLOCK]) {
            label("CLK");
            item(s.clock == null ? "--:--" : s.clock, s.clock == null ? UNKNOWN : PLAIN);
        }
    }

    private static String chargeState(Sample s) {
        if (s.batteryStatus == BatteryManager.BATTERY_STATUS_CHARGING) {
            return "CHG";
        }
        if (s.batteryStatus == BatteryManager.BATTERY_STATUS_FULL) {
            return "FULL";
        }
        return s.plugged ? "AC" : null;
    }

    private void temperature(int c) {
        item(pad(c, 2) + "°C", level(c, TEMP_WARN, TEMP_BAD));
    }

    private static int level(int v, int warn, int bad) {
        if (v == Sample.NA) {
            return UNKNOWN;
        }
        return v >= bad ? BAD : v >= warn ? WARN : GOOD;
    }

    /** The number (or "--") right-aligned in width characters */
    private static String pad(int v, int width) {
        final String s = v == Sample.NA ? NA : Integer.toString(v);
        final StringBuilder b = new StringBuilder();
        for (int i = s.length(); i < width; i++) {
            b.append(' ');
        }
        return b.append(s).toString();
    }

    private void label(String name) {
        if (mOut.length() > 0) {
            mOut.append('\n');
        }
        add(name, LABEL);
    }

    private void item(String text, int color) {
        mOut.append(' ');
        add(text, color);
    }

    private void add(String text, int color) {
        final int start = mOut.length();
        mOut.append(text);
        mOut.setSpan(new ForegroundColorSpan(color), start, mOut.length(),
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }
}
