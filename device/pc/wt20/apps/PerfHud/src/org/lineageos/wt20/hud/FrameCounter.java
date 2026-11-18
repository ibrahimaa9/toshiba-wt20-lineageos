/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.app.ActivityManager;
import android.content.Context;
import android.os.SystemClock;
import android.util.Log;
import android.view.WindowManager;
import android.window.TaskFpsCallback;

import java.util.List;

/**
 * FPS of the app in front: WindowManager's task FPS counter (SurfaceFlinger counts the frames the
 * task's layers present and reports about once a second). This is the game's own frame rate, as
 * in MSI Afterburner, not the panel's: a still app (nothing new to draw) reads 0.
 *
 * registerTaskFpsCallback is a @SystemApi behind ACCESS_FPS_COUNTER, and the task in front comes
 * from getRunningTasks with REAL_GET_TASKS; both are signature|privileged, held by this
 * platform-signed app. (SurfaceFlinger's old page-flip debug code 1013 is unused on Android 16.)
 * Only called from the sampling thread; the callback runs on a binder thread.
 */
final class FrameCounter {
    private static final String TAG = HudService.TAG;
    /** A task that reported nothing for this long is not drawing */
    private static final long STALE_MS = 2500;

    private final WindowManager mWindowManager;
    private final ActivityManager mActivityManager;
    private final TaskFpsCallback mCallback = new TaskFpsCallback() {
        @Override
        public void onFpsReported(float fps) {
            mFps = fps;
            mReportedAt = SystemClock.elapsedRealtime();
        }
    };

    private int mTaskId = -1;
    private boolean mRegistered;
    private boolean mWarned;
    private volatile float mFps = -1;
    private volatile long mReportedAt;

    FrameCounter(Context context) {
        mWindowManager = context.getSystemService(WindowManager.class);
        mActivityManager = context.getSystemService(ActivityManager.class);
    }

    /** Frames per second of the task in front, or Sample.NA */
    int fps(long nowMs) {
        final int task = foregroundTask();
        if (task != mTaskId) {
            unregister();
            mTaskId = task;
            mFps = -1;
            if (task >= 0) {
                register(task, nowMs);
            }
        }
        if (!mRegistered) {
            return Sample.NA;
        }
        if (mFps < 0 || nowMs - mReportedAt > STALE_MS) {
            return 0;
        }
        return Math.round(mFps);
    }

    void release() {
        unregister();
        mTaskId = -1;
    }

    private int foregroundTask() {
        try {
            final List<ActivityManager.RunningTaskInfo> tasks = mActivityManager.getRunningTasks(1);
            return tasks == null || tasks.isEmpty() ? -1 : tasks.get(0).taskId;
        } catch (RuntimeException e) {
            warn("no running task: " + e);
            return -1;
        }
    }

    private void register(int task, long nowMs) {
        try {
            mWindowManager.registerTaskFpsCallback(task, Runnable::run, mCallback);
            mRegistered = true;
            mReportedAt = nowMs;
        } catch (RuntimeException e) {
            // IllegalArgumentException: the task went away; SecurityException: permission
            warn("task " + task + " FPS counter refused: " + e);
            mRegistered = false;
        }
    }

    private void unregister() {
        if (mRegistered) {
            try {
                mWindowManager.unregisterTaskFpsCallback(mCallback);
            } catch (RuntimeException e) {
                // the task is gone already
            }
            mRegistered = false;
        }
    }

    private void warn(String why) {
        if (!mWarned) {
            Log.w(TAG, why);
            mWarned = true;
        }
    }
}
