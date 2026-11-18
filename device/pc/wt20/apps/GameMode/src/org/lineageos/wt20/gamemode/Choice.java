/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.gamemode;

import android.app.GameManager;

/** One game's settings as the owner picked them. */
final class Choice {
    static final int[] SCALES = {100, 85, 75, 60};
    static final int[] FPS = {0, 60, 45, 30};
    static final int[] MODES = {
        GameManager.GAME_MODE_STANDARD,
        GameManager.GAME_MODE_PERFORMANCE,
        GameManager.GAME_MODE_BATTERY,
    };
    static final Choice DEFAULT = new Choice(GameManager.GAME_MODE_STANDARD, 100, 0);

    final int mode;
    final int scalePct;
    final int fps;

    Choice(int mode, int scalePct, int fps) {
        this.mode = mode;
        this.scalePct = scalePct;
        this.fps = fps;
    }

    boolean isDefault() {
        return mode == GameManager.GAME_MODE_STANDARD && scalePct == 100 && fps == 0;
    }

    float scale() {
        return scalePct / 100f;
    }

    boolean sameAs(Choice o) {
        return mode == o.mode && scalePct == o.scalePct && fps == o.fps;
    }

    static Choice parse(String s) {
        if (s == null) {
            return DEFAULT;
        }
        try {
            final String[] p = s.split(":");
            final Choice c = new Choice(Integer.parseInt(p[0]), Integer.parseInt(p[1]),
                    Integer.parseInt(p[2]));
            return c.isValid() ? c : DEFAULT;
        } catch (RuntimeException e) {
            return DEFAULT;
        }
    }

    private boolean isValid() {
        return contains(MODES, mode) && contains(SCALES, scalePct) && contains(FPS, fps);
    }

    static int indexOf(int[] values, int value) {
        for (int i = 0; i < values.length; i++) {
            if (values[i] == value) {
                return i;
            }
        }
        return 0;
    }

    private static boolean contains(int[] values, int value) {
        for (int v : values) {
            if (v == value) {
                return true;
            }
        }
        return false;
    }

    @Override
    public String toString() {
        return mode + ":" + scalePct + ":" + fps;
    }
}
