/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * The owner's settings, shared by the activity, the tile and the overlay service (one process,
 * so the service follows changes through a preference listener).
 */
final class Prefs {
    static final String KEY_ENABLED = "enabled";
    static final String KEY_LOCKED = "locked";
    static final String KEY_SIZE = "text_size";
    static final String KEY_OPACITY = "opacity";
    static final String KEY_INTERVAL = "interval_ms";
    static final String KEY_X = "x";
    static final String KEY_Y = "y";
    /** Written by the service: the window type it got (WINDOW_*, "" while off) */
    static final String KEY_WINDOW = "window";

    static final String WINDOW_NONE = "none";
    static final String WINDOW_SECURE = "secure";
    static final String WINDOW_SYSTEM = "system";
    static final String WINDOW_OVERLAY = "overlay";

    // Metrics, in the order of the settings screen
    static final int M_FPS = 0;
    static final int M_CPU = 1;
    static final int M_CPU_CORES = 2;
    static final int M_CPU_FREQ = 3;
    static final int M_CPU_TEMP = 4;
    static final int M_BOARD_TEMP = 5;
    static final int M_GPU_FREQ = 6;
    static final int M_GPU_LOAD = 7;
    static final int M_RAM = 8;
    static final int M_SWAP = 9;
    static final int M_BATTERY = 10;
    static final int M_BATTERY_POWER = 11;
    static final int M_CLOCK = 12;
    static final int M_COUNT = 13;

    private static final String METRIC_PREFIX = "m_";
    private static final String[] METRIC_KEYS = {
        "fps", "cpu", "cpu_cores", "cpu_freq", "cpu_temp", "board_temp", "gpu_freq",
        "gpu_load", "ram", "swap", "battery", "battery_power", "clock",
    };
    private static final boolean[] METRIC_DEFAULTS = {
        true, true, false, false, true, false, true,
        true, true, false, true, false, false,
    };

    /** Text size S / M / L, sp */
    static final float[] TEXT_SP = {10f, 12f, 15f};
    private static final int DEFAULT_SIZE = 1;
    /** Background opacity, % */
    static final int[] OPACITY = {30, 50, 70, 90};
    private static final int DEFAULT_OPACITY = 50;
    static final int[] INTERVAL_MS = {500, 1000, 2000};
    private static final int DEFAULT_INTERVAL_MS = 1000;

    private Prefs() {}

    static SharedPreferences get(Context context) {
        return context.getApplicationContext().getSharedPreferences("hud", Context.MODE_PRIVATE);
    }

    static String metricKey(int metric) {
        return METRIC_PREFIX + METRIC_KEYS[metric];
    }

    static boolean isMetricKey(String key) {
        return key.startsWith(METRIC_PREFIX);
    }

    static boolean metric(SharedPreferences p, int metric) {
        return p.getBoolean(metricKey(metric), METRIC_DEFAULTS[metric]);
    }

    static boolean[] metrics(SharedPreferences p) {
        final boolean[] out = new boolean[M_COUNT];
        for (int i = 0; i < M_COUNT; i++) {
            out[i] = metric(p, i);
        }
        return out;
    }

    /** Index into TEXT_SP */
    static int textSize(SharedPreferences p) {
        final int i = p.getInt(KEY_SIZE, DEFAULT_SIZE);
        return i >= 0 && i < TEXT_SP.length ? i : DEFAULT_SIZE;
    }

    static int opacity(SharedPreferences p) {
        final int v = p.getInt(KEY_OPACITY, DEFAULT_OPACITY);
        return indexOf(OPACITY, v) >= 0 ? v : DEFAULT_OPACITY;
    }

    static int interval(SharedPreferences p) {
        final int v = p.getInt(KEY_INTERVAL, DEFAULT_INTERVAL_MS);
        return indexOf(INTERVAL_MS, v) >= 0 ? v : DEFAULT_INTERVAL_MS;
    }

    static int indexOf(int[] values, int value) {
        for (int i = 0; i < values.length; i++) {
            if (values[i] == value) {
                return i;
            }
        }
        return -1;
    }
}
