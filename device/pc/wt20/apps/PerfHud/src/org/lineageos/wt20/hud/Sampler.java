/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.SystemClock;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.Arrays;
import java.util.Locale;

/**
 * Reads the metrics from /proc, sysfs, SurfaceFlinger and the battery broadcast. Every source
 * fails on its own: a missing file or an SELinux denial leaves that value unknown ("--"),
 * nothing throws. Only the enabled metrics are read. Used from the sampling thread only.
 */
final class Sampler {
    private static final String CPU_DIR = "/sys/devices/system/cpu/";
    private static final String THERMAL_ZONE = "/sys/class/thermal/thermal_zone";
    private static final String POWER_SUPPLY = "/sys/class/power_supply";
    // The Intel iGPU is always 0000:00:02.0 (that whole directory is sysfs_gpu, pc_x86_64
    // genfs_contexts); /sys/class/drm is the generic fallback. The card number is not fixed
    // (card1 here, card0 is the boot framebuffer), so the card is the one with gt_act_freq_mhz.
    private static final String[] DRM_CARDS = {
        "/sys/devices/pci0000:00/0000:00:02.0/drm/card", "/sys/class/drm/card",
    };
    /** How often a source that was not found is looked for again */
    private static final long RESCAN_MS = 10_000;
    /** Older deltas (a metric switched off for a while) are dropped, not averaged */
    private static final long MAX_GAP_MS = 5000;
    private static final int MAX_CPUS = 64;
    private static final long NONE = Long.MIN_VALUE;
    private static final DateTimeFormatter CLOCK = DateTimeFormatter.ofPattern("HH:mm", Locale.US);

    private final Context mContext;
    private final BatteryManager mBatteryManager;
    private final ActivityManager mActivityManager;
    private final FrameCounter mFrames;

    // /proc/stat counters of the previous sample: index 0 = all CPUs, 1 + n = cpuN
    private final long[] mLastTotal = new long[MAX_CPUS + 1];
    private final long[] mLastIdle = new long[MAX_CPUS + 1];
    private int mCpuCount;

    private File mPkgTemp;
    private File mBoardTemp;
    private long mNextThermalScan;

    private File mGpuFreq;
    private File mRc6;
    private long mNextGpuScan;
    private long mLastRc6 = -1;
    private long mLastRc6At;

    private File mBattery;
    private long mNextBatteryScan;

    Sampler(Context context) {
        mContext = context.getApplicationContext();
        mBatteryManager = mContext.getSystemService(BatteryManager.class);
        mActivityManager = mContext.getSystemService(ActivityManager.class);
        mFrames = new FrameCounter(mContext);
    }

    Sample sample(boolean[] m) {
        final long now = SystemClock.elapsedRealtime();
        final Sample s = new Sample();
        if (m[Prefs.M_FPS]) {
            s.fps = mFrames.fps(now);
        }
        if (m[Prefs.M_CPU] || m[Prefs.M_CPU_CORES]) {
            readCpuLoad(s);
        }
        if (m[Prefs.M_CPU_FREQ]) {
            readCpuFreq(s);
        }
        if (m[Prefs.M_CPU_TEMP] || m[Prefs.M_BOARD_TEMP]) {
            readTemps(s, now);
        }
        if (m[Prefs.M_GPU_FREQ] || m[Prefs.M_GPU_LOAD]) {
            readGpu(s, now);
        }
        if (m[Prefs.M_RAM] || m[Prefs.M_SWAP]) {
            readMemory(s);
        }
        if (m[Prefs.M_BATTERY] || m[Prefs.M_BATTERY_POWER]) {
            readBattery(s, now, m[Prefs.M_BATTERY_POWER]);
        }
        if (m[Prefs.M_CLOCK]) {
            s.clock = LocalTime.now().format(CLOCK);
        }
        return s;
    }

    // --- CPU -----------------------------------------------------------------------------------

    /** Total and per-CPU load from the /proc/stat jiffies since the previous sample */
    private void readCpuLoad(Sample s) {
        final int[] cores = new int[MAX_CPUS];
        Arrays.fill(cores, Sample.NA);
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/stat"), 2048)) {
            String line;
            // the cpu lines come first: "cpu  user nice system idle iowait irq softirq steal ..."
            while ((line = r.readLine()) != null && line.startsWith("cpu")) {
                final String[] t = line.trim().split("\\s+");
                if (t.length < 6) {
                    continue;
                }
                final int idx;
                if (t[0].equals("cpu")) {
                    idx = 0;
                } else {
                    final int n = Integer.parseInt(t[0].substring(3));
                    if (n >= MAX_CPUS) {
                        continue;
                    }
                    idx = n + 1;
                    mCpuCount = Math.max(mCpuCount, n + 1);
                }
                long total = 0;
                // user .. steal (guest time is already counted in user)
                for (int i = 1; i < Math.min(t.length, 9); i++) {
                    total += Long.parseLong(t[i]);
                }
                final long idle = Long.parseLong(t[4]) + Long.parseLong(t[5]); // idle + iowait
                final long dTotal = total - mLastTotal[idx];
                final long dIdle = idle - mLastIdle[idx];
                final int load = mLastTotal[idx] == 0 || dTotal <= 0 ? Sample.NA
                        : clampPct((int) Math.round(100.0 * (dTotal - dIdle) / dTotal));
                mLastTotal[idx] = total;
                mLastIdle[idx] = idle;
                if (idx == 0) {
                    s.cpuLoad = load;
                } else {
                    cores[idx - 1] = load; // an offline CPU has no line and stays NA
                }
            }
        } catch (IOException | RuntimeException e) {
            return; // unreadable (SELinux) or unparsable: unknown
        }
        s.coreLoad = Arrays.copyOf(cores, cpuCount());
    }

    /** Current clock of every logical CPU (cpufreq scaling_cur_freq, kHz) */
    private void readCpuFreq(Sample s) {
        final int n = cpuCount();
        s.coreMhz = new int[n];
        for (int i = 0; i < n; i++) {
            final long khz = readLong(new File(CPU_DIR + "cpu" + i + "/cpufreq/scaling_cur_freq"));
            s.coreMhz[i] = khz > 0 ? (int) (khz / 1000) : Sample.NA;
        }
    }

    /** Logical CPUs present ("0-3" in /sys/devices/system/cpu/present: 2 cores + HT here) */
    private int cpuCount() {
        if (mCpuCount == 0) {
            final String present = readLine(new File(CPU_DIR + "present"));
            try {
                final String[] parts = present.split("[-,]");
                mCpuCount = Integer.parseInt(parts[parts.length - 1].trim()) + 1;
            } catch (RuntimeException e) {
                mCpuCount = Runtime.getRuntime().availableProcessors();
            }
            mCpuCount = Math.max(1, Math.min(mCpuCount, MAX_CPUS));
        }
        return mCpuCount;
    }

    // --- Temperatures --------------------------------------------------------------------------

    private void readTemps(Sample s, long now) {
        if ((mPkgTemp == null || mBoardTemp == null) && now >= mNextThermalScan) {
            findThermalZones();
            mNextThermalScan = now + RESCAN_MS;
        }
        s.cpuTemp = readTemp(mPkgTemp);
        if (s.cpuTemp == Sample.NA) {
            mPkgTemp = null; // zone numbers move (iwlwifi adds one): look for it again
        }
        s.boardTemp = readTemp(mBoardTemp);
        if (s.boardTemp == Sample.NA) {
            mBoardTemp = null;
        }
    }

    /** The zones are found by their type: x86_pkg_temp (CPU package) and acpitz (board) */
    private void findThermalZones() {
        int missing = 0;
        for (int i = 0; i < 32 && missing < 4; i++) {
            final File zone = new File(THERMAL_ZONE + i);
            final String type = readLine(new File(zone, "type"));
            if (type == null) {
                missing++;
                continue;
            }
            missing = 0;
            if (mPkgTemp == null && type.equals("x86_pkg_temp")) {
                mPkgTemp = new File(zone, "temp");
            } else if (mBoardTemp == null && type.equals("acpitz")) {
                mBoardTemp = new File(zone, "temp");
            }
        }
    }

    private static int readTemp(File f) {
        final long milli = f == null ? NONE : readLong(f);
        return milli == NONE ? Sample.NA : (int) Math.round(milli / 1000.0);
    }

    // --- GPU -----------------------------------------------------------------------------------

    /** i915: actual clock, and busy % = 100 - RC6 (idle) residency / wall time */
    private void readGpu(Sample s, long now) {
        if (mGpuFreq == null && now >= mNextGpuScan) {
            findGpu();
            mNextGpuScan = now + RESCAN_MS;
        }
        if (mGpuFreq == null) {
            return;
        }
        final long mhz = readLong(mGpuFreq);
        if (mhz == NONE) {
            mGpuFreq = null;
            return;
        }
        s.gpuMhz = (int) mhz;
        if (mRc6 == null) {
            return;
        }
        final long rc6 = readLong(mRc6);
        if (rc6 != NONE && mLastRc6 >= 0 && now > mLastRc6At && now - mLastRc6At <= MAX_GAP_MS) {
            final double idle = (rc6 - mLastRc6) * 100.0 / (now - mLastRc6At);
            s.gpuLoad = clampPct((int) Math.round(100.0 - idle));
        }
        mLastRc6 = rc6 == NONE ? -1 : rc6;
        mLastRc6At = now;
    }

    private void findGpu() {
        for (int i = 0; i < 8; i++) {
            for (String base : DRM_CARDS) {
                final File card = new File(base + i);
                final File freq = new File(card, "gt_act_freq_mhz");
                if (readLong(freq) != NONE) {
                    mGpuFreq = freq;
                    mRc6 = null;
                    for (String rc6 : new String[] {
                            "power/rc6_residency_ms", "gt/gt0/rc6_residency_ms"}) {
                        if (readLong(new File(card, rc6)) != NONE) {
                            mRc6 = new File(card, rc6);
                            break;
                        }
                    }
                    mLastRc6 = -1;
                    return;
                }
            }
        }
    }

    // --- Memory --------------------------------------------------------------------------------

    /** Used = MemTotal - MemAvailable; swap used = SwapTotal - SwapFree (kB) */
    private void readMemory(Sample s) {
        long total = -1;
        long avail = -1;
        long swapTotal = -1;
        long swapFree = -1;
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/meminfo"), 2048)) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.startsWith("MemTotal:")) {
                    total = kb(line);
                } else if (line.startsWith("MemAvailable:")) {
                    avail = kb(line);
                } else if (line.startsWith("SwapTotal:")) {
                    swapTotal = kb(line);
                } else if (line.startsWith("SwapFree:")) {
                    swapFree = kb(line);
                }
            }
        } catch (IOException | RuntimeException e) {
            // unreadable: RAM from ActivityManager below, swap unknown
        }
        if (total <= 0 || avail < 0) {
            try {
                final ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
                mActivityManager.getMemoryInfo(mi);
                total = mi.totalMem / 1024;
                avail = mi.availMem / 1024;
            } catch (RuntimeException e) {
                total = -1;
            }
        }
        if (total > 0 && avail >= 0) {
            s.ramTotalKb = total;
            s.ramUsedKb = Math.max(0, total - avail);
        }
        if (swapTotal >= 0 && swapFree >= 0) {
            s.swapUsedKb = Math.max(0, swapTotal - swapFree);
        }
    }

    private static long kb(String line) {
        return Long.parseLong(line.split("\\s+")[1]);
    }

    // --- Battery -------------------------------------------------------------------------------

    private void readBattery(Sample s, long now, boolean power) {
        // The sticky broadcast: no receiver is registered
        final Intent b = mContext.registerReceiver(null,
                new IntentFilter(Intent.ACTION_BATTERY_CHANGED), Context.RECEIVER_NOT_EXPORTED);
        int voltageMv = -1;
        if (b != null) {
            final int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            final int scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (level >= 0 && scale > 0) {
                s.batteryPct = level * 100 / scale;
            }
            s.batteryStatus = b.getIntExtra(BatteryManager.EXTRA_STATUS,
                    BatteryManager.BATTERY_STATUS_UNKNOWN);
            s.plugged = b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
            voltageMv = b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1);
        }
        if (power) {
            s.batteryWatts = batteryWatts(now, s.batteryStatus, voltageMv);
        }
    }

    /** W, + charging / - discharging, or NaN */
    private float batteryWatts(long now, int status, int voltageMv) {
        if (mBattery == null && now >= mNextBatteryScan) {
            mBattery = findBattery();
            mNextBatteryScan = now + RESCAN_MS;
        }
        if (mBattery != null) {
            // ACPI batteries (BAT1 here) report power_now in uW, always positive
            long uw = readLong(new File(mBattery, "power_now"));
            if (uw == NONE) {
                final long ua = readLong(new File(mBattery, "current_now"));
                final long uv = readLong(new File(mBattery, "voltage_now"));
                if (ua != NONE && uv > 0) {
                    uw = Math.abs(ua) * uv / 1_000_000;
                }
            }
            if (uw != NONE) {
                final String st = readLine(new File(mBattery, "status"));
                final boolean discharging = st != null ? st.equals("Discharging")
                        : status == BatteryManager.BATTERY_STATUS_DISCHARGING;
                final float w = Math.abs(uw) / 1e6f;
                return discharging ? -w : w;
            }
        }
        // No readable supply: the health HAL's current (uA, + into the battery; the pc HAL
        // derives it from power_now) times the broadcast's voltage
        if (voltageMv > 0) {
            final long ua = mBatteryManager.getLongProperty(
                    BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
            if (ua != Long.MIN_VALUE) {
                return ua * voltageMv / 1e9f;
            }
        }
        return Float.NaN;
    }

    /** The first power supply typed "Battery", preferring one that reports power or current */
    private static File findBattery() {
        final String[] names = new File(POWER_SUPPLY).list();
        if (names == null) {
            return null; // missing or not readable
        }
        Arrays.sort(names);
        File fallback = null;
        for (String name : names) {
            final File dir = new File(POWER_SUPPLY, name);
            if (!"Battery".equals(readLine(new File(dir, "type")))) {
                continue;
            }
            if (new File(dir, "power_now").exists() || new File(dir, "current_now").exists()) {
                return dir;
            }
            if (fallback == null) {
                fallback = dir;
            }
        }
        return fallback;
    }

    // --- Files ---------------------------------------------------------------------------------

    /** The first line, trimmed, or null when the file is missing or not readable */
    static String readLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f), 256)) {
            final String line = r.readLine();
            return line == null ? null : line.trim();
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    private static long readLong(File f) {
        final String s = readLine(f);
        if (s == null || s.isEmpty()) {
            return NONE;
        }
        try {
            return Long.parseLong(s);
        } catch (NumberFormatException e) {
            return NONE;
        }
    }

    private static int clampPct(int v) {
        return Math.max(0, Math.min(100, v));
    }
}
