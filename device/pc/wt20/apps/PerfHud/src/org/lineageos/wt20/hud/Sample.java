/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

/** One reading of the enabled metrics. NA, null, -1 or NaN = unknown, shown as "--". */
final class Sample {
    static final int NA = Integer.MIN_VALUE;

    int fps = NA;
    /** % */
    int cpuLoad = NA;
    /** % per logical CPU */
    int[] coreLoad;
    /** MHz per logical CPU */
    int[] coreMhz;
    /** degrees C */
    int cpuTemp = NA;
    int boardTemp = NA;
    int gpuMhz = NA;
    /** % */
    int gpuLoad = NA;
    long ramUsedKb = -1;
    long ramTotalKb = -1;
    long swapUsedKb = -1;
    /** % */
    int batteryPct = NA;
    /** BatteryManager.BATTERY_STATUS_* */
    int batteryStatus = NA;
    boolean plugged;
    /** W, + into the battery, - out of it */
    float batteryWatts = Float.NaN;
    /** HH:mm */
    String clock;
}
