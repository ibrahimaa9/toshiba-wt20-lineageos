/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings tile: shows or hides the HUD (long press opens its settings). */
public class HudTileService extends TileService {
    @Override
    public void onStartListening() {
        refresh();
    }

    @Override
    public void onClick() {
        HudService.setEnabled(this, !Prefs.get(this).getBoolean(Prefs.KEY_ENABLED, false));
        refresh();
    }

    private void refresh() {
        final Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        final boolean on = Prefs.get(this).getBoolean(Prefs.KEY_ENABLED, false);
        tile.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setSubtitle(getString(on ? R.string.tile_on : R.string.tile_off));
        tile.updateTile();
    }
}
