/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.gamemode;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The owner's choices (only games he changed are stored) and the apps he added by hand.
 * This is the source of truth: GameManagerService forgets non-standard modes on boot.
 */
final class GameModeStore {
    private static final String PREFS = "choices";
    private static final String KEY_MANUAL = "manual";
    private static final String CHOICE_PREFIX = "c:";

    private final SharedPreferences mPrefs;

    GameModeStore(Context context) {
        mPrefs = context.getApplicationContext()
                .getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    Choice get(String pkg) {
        return Choice.parse(mPrefs.getString(CHOICE_PREFIX + pkg, null));
    }

    void put(String pkg, Choice choice) {
        final SharedPreferences.Editor e = mPrefs.edit();
        if (choice.isDefault()) {
            e.remove(CHOICE_PREFIX + pkg);
        } else {
            e.putString(CHOICE_PREFIX + pkg, choice.toString());
        }
        e.commit();
    }

    List<String> customizedPackages() {
        final List<String> out = new ArrayList<>();
        for (String key : mPrefs.getAll().keySet()) {
            if (key.startsWith(CHOICE_PREFIX)) {
                out.add(key.substring(CHOICE_PREFIX.length()));
            }
        }
        return out;
    }

    Set<String> manualPackages() {
        return new HashSet<>(mPrefs.getStringSet(KEY_MANUAL, new HashSet<>()));
    }

    void addManual(String pkg) {
        final Set<String> s = manualPackages();
        s.add(pkg);
        mPrefs.edit().putStringSet(KEY_MANUAL, s).commit();
    }

    void removeManual(String pkg) {
        final Set<String> s = manualPackages();
        s.remove(pkg);
        mPrefs.edit().putStringSet(KEY_MANUAL, s).commit();
    }
}
