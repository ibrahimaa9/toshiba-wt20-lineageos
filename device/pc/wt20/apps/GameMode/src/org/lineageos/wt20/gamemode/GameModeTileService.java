/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.gamemode;

import android.app.PendingIntent;
import android.content.Intent;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings tile: shows how many games are customized and opens the app. */
public class GameModeTileService extends TileService {
    @Override
    public void onStartListening() {
        final GameModeStore store = new GameModeStore(this);
        final Tile tile = getQsTile();
        if (tile != null) {
            final int n = store.customizedPackages().size();
            tile.setState(n > 0 ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
            tile.setSubtitle(n > 0 ? getString(R.string.tile_count, n)
                    : getString(R.string.tile_none));
            tile.updateTile();
        }
        // Cheap check (only the customized games): re-apply what a game update dropped
        final GameModeController controller = new GameModeController(this);
        new Thread(() -> controller.reconcileAll(store), "WT20GameModeTile").start();
    }

    @Override
    public void onClick() {
        final Intent intent = new Intent(this, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        final PendingIntent pi = PendingIntent.getActivity(this, 0, intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        if (isLocked()) {
            unlockAndRun(() -> startActivityAndCollapse(pi));
        } else {
            startActivityAndCollapse(pi);
        }
    }
}
