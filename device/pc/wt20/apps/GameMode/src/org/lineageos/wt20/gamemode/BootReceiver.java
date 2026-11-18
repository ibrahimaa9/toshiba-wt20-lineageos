/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.gamemode;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * GameManagerService resets every game without a manifest/device_config mode config to the
 * standard mode when the user starts: the stored choices are applied again after boot.
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        final PendingResult result = goAsync();
        final Context app = context.getApplicationContext();
        new Thread(() -> {
            try {
                new GameModeController(app).reconcileAll(new GameModeStore(app));
            } finally {
                result.finish();
            }
        }, "WT20GameModeBoot").start();
    }
}
