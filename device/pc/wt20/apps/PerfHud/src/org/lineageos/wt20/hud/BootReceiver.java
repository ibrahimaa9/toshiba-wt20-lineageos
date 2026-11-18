/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Shows the HUD again after a boot or an app update (OTA) if the owner left it on. */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (Prefs.get(context).getBoolean(Prefs.KEY_ENABLED, false)) {
            HudService.start(context);
        }
    }
}
