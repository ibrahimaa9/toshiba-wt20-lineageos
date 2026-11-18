/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.gamemode;

import android.app.GameManager;
import android.app.GameModeConfiguration;
import android.app.GameModeInfo;
import android.app.IGameManagerService;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.os.ServiceManager;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Applies a {@link Choice} to GameManagerService. Call from a background thread. */
final class GameModeController {
    static final String TAG = "WT20GameMode";

    static final int RESULT_OK = 0;
    static final int RESULT_NO_FPS = 1;
    static final int RESULT_FAILED = 2;

    private static final float SCALING_DISABLED = -1f;

    private final PackageManager mPm;
    private final GameManager mGameManager;
    private final IBinder mBinder;
    private final IGameManagerService mService;
    private final int mUserId;

    GameModeController(Context context) {
        final Context app = context.getApplicationContext();
        mPm = app.getPackageManager();
        mGameManager = app.getSystemService(GameManager.class);
        mBinder = ServiceManager.getService(Context.GAME_SERVICE);
        mService = IGameManagerService.Stub.asInterface(mBinder);
        mUserId = Process.myUserHandle().getIdentifier();
    }

    /** GameManagerService's own test (isPackageGame): manifest appCategory or installer hint. */
    static boolean isGameCategory(ApplicationInfo ai) {
        return ai.category == ApplicationInfo.CATEGORY_GAME;
    }

    /** The game mode that carries a choice (STANDARD never gets an FPS override). */
    static int effectiveMode(Choice c) {
        return c.mode == GameManager.GAME_MODE_STANDARD && c.fps != 0
                ? GameManager.GAME_MODE_CUSTOM : c.mode;
    }

    int apply(String pkg, Choice c) {
        if (mService == null || mGameManager == null) {
            Log.w(TAG, "game service missing");
            return RESULT_FAILED;
        }
        final String user = String.valueOf(mUserId);
        try {
            final ApplicationInfo ai = mPm.getApplicationInfo(pkg,
                    PackageManager.ApplicationInfoFlags.of(0));
            if (!isGameCategory(ai)) {
                // Not a game to GameManagerService: only the resolution override applies (the
                // compat-scale provider does not check the category)
                mService.updateResolutionScalingFactor(pkg, GameManager.GAME_MODE_STANDARD,
                        scaling(c), mUserId);
                return RESULT_OK;
            }

            // Start from a clean state: no override left over from an earlier choice
            final boolean reset = shell("reset", "--user", user, pkg);

            if (c.mode == GameManager.GAME_MODE_STANDARD) {
                if (c.fps == 0) {
                    if (c.scalePct != 100 || !reset) {
                        mService.updateResolutionScalingFactor(pkg,
                                GameManager.GAME_MODE_STANDARD, scaling(c), mUserId);
                    }
                    mGameManager.setGameMode(pkg, GameManager.GAME_MODE_STANDARD);
                } else {
                    if (!reset) {
                        // a stale standard-mode scale would return when the mode falls back
                        mService.updateResolutionScalingFactor(pkg,
                                GameManager.GAME_MODE_STANDARD, SCALING_DISABLED, mUserId);
                    }
                    mGameManager.updateCustomGameModeConfiguration(pkg,
                            new GameModeConfiguration.Builder()
                                    .setScalingFactor(c.scale())
                                    .setFpsOverride(c.fps)
                                    .build());
                    mGameManager.setGameMode(pkg, GameManager.GAME_MODE_CUSTOM);
                }
                return RESULT_OK;
            }

            // Performance / Battery: the per-mode override the "cmd game set" command writes
            // (also selects the mode)
            final List<String> args = new ArrayList<>();
            args.add("set");
            args.add("--mode");
            args.add(String.valueOf(c.mode));
            args.add("--user");
            args.add(user);
            args.add("--downscale");
            args.add(c.scalePct == 100 ? "disable" : String.valueOf(c.scale()));
            if (c.fps != 0) {
                args.add("--fps");
                args.add(String.valueOf(c.fps));
            }
            args.add(pkg);
            if (shell(args.toArray(new String[0]))) {
                return RESULT_OK;
            }
            // Fallback through the binder API: mode and scale, no FPS cap
            mService.updateResolutionScalingFactor(pkg, c.mode, scaling(c), mUserId);
            return c.fps == 0 ? RESULT_OK : RESULT_NO_FPS;
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, pkg + " is not installed");
            return RESULT_FAILED;
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "applying " + c + " to " + pkg + " failed", e);
            return RESULT_FAILED;
        }
    }

    /** Applies again every stored choice that GameManagerService no longer has. */
    void reconcileAll(GameModeStore store) {
        if (mService == null || mGameManager == null) {
            return;
        }
        for (String pkg : store.customizedPackages()) {
            final Choice c = store.get(pkg);
            if (c.isDefault()) {
                continue;
            }
            try {
                if (!matches(pkg, c)) {
                    Log.i(TAG, "applying " + c + " to " + pkg + " again");
                    apply(pkg, c);
                }
            } catch (PackageManager.NameNotFoundException e) {
                // uninstalled: the choice stays for a reinstall
            } catch (RemoteException | RuntimeException e) {
                Log.w(TAG, "checking " + pkg + " failed", e);
            }
        }
    }

    private boolean matches(String pkg, Choice c)
            throws PackageManager.NameNotFoundException, RemoteException {
        final ApplicationInfo ai = mPm.getApplicationInfo(pkg,
                PackageManager.ApplicationInfoFlags.of(0));
        if (!isGameCategory(ai)) {
            return sameScale(mService.getResolutionScalingFactor(pkg,
                    GameManager.GAME_MODE_STANDARD, mUserId), c.scalePct);
        }
        final GameModeInfo info = mGameManager.getGameModeInfo(pkg);
        if (info == null) {
            return true;
        }
        final int mode = effectiveMode(c);
        if (info.getActiveGameMode() != mode) {
            return false;
        }
        final GameModeConfiguration cfg = info.getGameModeConfiguration(mode);
        final float scale = cfg == null ? 1f : cfg.getScalingFactor();
        final int fps = cfg == null ? 0 : cfg.getFpsOverride();
        return sameScale(scale, c.scalePct)
                && (mode == GameManager.GAME_MODE_STANDARD || fps == c.fps);
    }

    private static boolean sameScale(float scale, int pct) {
        if (pct == 100) {
            return scale <= 0f || Math.abs(scale - 1f) < 0.01f;
        }
        return Math.abs(scale - pct / 100f) < 0.01f;
    }

    private static float scaling(Choice c) {
        return c.scalePct == 100 ? SCALING_DISABLED : c.scale();
    }

    /**
     * Runs a GameManagerShellCommand ("cmd game ...") in system_server as this app: the
     * command checks MANAGE_GAME_MODE of the caller, not the shell uid. True on exit code 0.
     */
    private boolean shell(String... args) {
        final String line = "cmd game " + String.join(" ", args);
        if (mBinder == null) {
            return false;
        }
        ParcelFileDescriptor in = null;
        ParcelFileDescriptor read = null;
        ParcelFileDescriptor write = null;
        try {
            in = ParcelFileDescriptor.open(new File("/dev/null"),
                    ParcelFileDescriptor.MODE_READ_ONLY);
            final ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
            read = pipe[0];
            write = pipe[1];
            final AtomicInteger code = new AtomicInteger(Integer.MIN_VALUE);
            final CountDownLatch done = new CountDownLatch(1);
            final ResultReceiver result = new ResultReceiver(null) {
                @Override
                protected void onReceiveResult(int resultCode, Bundle resultData) {
                    code.set(resultCode);
                    done.countDown();
                }
            };
            mBinder.shellCommand(in.getFileDescriptor(), write.getFileDescriptor(),
                    write.getFileDescriptor(), args, null, result);
            write.close();
            write = null;
            final String out = readAll(read);
            read = null;
            done.await(2, TimeUnit.SECONDS);
            Log.i(TAG, line + " -> " + code.get() + (out.isEmpty() ? "" : ": " + out.trim()));
            return code.get() == 0;
        } catch (IOException | RemoteException | RuntimeException e) {
            Log.w(TAG, line + " failed", e);
            return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } finally {
            closeQuietly(in);
            closeQuietly(read);
            closeQuietly(write);
        }
    }

    private static String readAll(ParcelFileDescriptor fd) throws IOException {
        try (InputStream is = new ParcelFileDescriptor.AutoCloseInputStream(fd)) {
            final ByteArrayOutputStream bos = new ByteArrayOutputStream();
            final byte[] buf = new byte[1024];
            int n;
            while ((n = is.read(buf)) > 0) {
                bos.write(buf, 0, n);
            }
            return bos.toString(StandardCharsets.UTF_8);
        }
    }

    private static void closeQuietly(ParcelFileDescriptor fd) {
        if (fd != null) {
            try {
                fd.close();
            } catch (IOException ignored) {
            }
        }
    }
}
