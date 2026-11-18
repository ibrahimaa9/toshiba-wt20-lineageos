/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.wt20.hud;

import android.app.AppOpsManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ComponentCallbacks;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Icon;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.Process;
import android.provider.Settings;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

/**
 * The overlay: a foreground service that owns one small window and refreshes it from a
 * sampling thread.
 *
 * Window types, by z-order (WindowManagerPolicy: apps < TYPE_APPLICATION_OVERLAY < keyboard
 * < status bar < notification shade < navigation bar < TYPE_SYSTEM_ERROR (internal)
 * < ... < TYPE_SECURE_SYSTEM_OVERLAY):
 *  - locked, the normal state while playing: TYPE_SECURE_SYSTEM_OVERLAY, the top layer an app
 *    process can add, over the status bar, the shade, the navigation bar, the keyboard and the
 *    lock screen. It needs INTERNAL_SYSTEM_WINDOW (signature; this app is platform-signed).
 *    WindowManager (DisplayPolicy) always makes it NOT_TOUCHABLE, and it is a trusted overlay
 *    for input: touches under it reach the game without the untrusted-touch occlusion check.
 *  - unlocked, to drag it: that type takes no touches, so the window is added again as
 *    TYPE_SYSTEM_ERROR, which with INTERNAL_SYSTEM_WINDOW is above the status bar, the shade
 *    and the navigation bar and is touchable.
 *  - if WindowManager refuses either (BadTokenException or SecurityException: the permission
 *    was not granted), TYPE_APPLICATION_OVERLAY with SYSTEM_ALERT_WINDOW (signature|appop; the
 *    app-op is switched on if needed): above apps and games, below the status bar and the
 *    shade. Locked, its window alpha is capped at InputManager's maximum obscuring opacity
 *    (0.8), or InputDispatcher would block the touches that pass through it to the game.
 * An added window cannot change its type, so locking or unlocking removes and adds it again.
 * FLAG_NOT_FOCUSABLE always: the game and the keyboard keep the focus.
 */
public class HudService extends Service
        implements SharedPreferences.OnSharedPreferenceChangeListener {
    static final String TAG = "WT20Hud";

    private static final String ACTION_STOP = "org.lineageos.wt20.hud.action.STOP";
    private static final String CHANNEL = "hud";
    private static final int NOTIFICATION_ID = 1;

    private final Handler mMain = new Handler(Looper.getMainLooper());
    private SharedPreferences mPrefs;
    private boolean mStarted;
    private HandlerThread mThread;
    private Handler mSampling;
    private Sampler mSampler;
    private volatile boolean mScreenOn = true;

    // The window (main thread only)
    private Context mWindowContext;
    private WindowManager mWm;
    private TextView mView;
    private WindowManager.LayoutParams mLp;
    private boolean mDragging;

    /** Shows the overlay if the owner left it on (boot, update, tile); false if refused */
    static boolean start(Context context) {
        final Context app = context.getApplicationContext();
        try {
            app.startForegroundService(new Intent(app, HudService.class));
            return true;
        } catch (IllegalStateException | SecurityException e) {
            // ForegroundServiceStartNotAllowedException: the privapp allowlist entry
            // (START_FOREGROUND_SERVICES_FROM_BACKGROUND) is missing
            Log.w(TAG, "cannot start the overlay service", e);
            return false;
        }
    }

    /** Switches the overlay on or off and remembers it; false if Android refused to start it */
    static boolean setEnabled(Context context, boolean on) {
        final SharedPreferences prefs = Prefs.get(context);
        prefs.edit().putBoolean(Prefs.KEY_ENABLED, on).apply();
        if (!on) {
            context.getApplicationContext().stopService(
                    new Intent(context.getApplicationContext(), HudService.class));
            return true;
        }
        if (start(context)) {
            return true;
        }
        prefs.edit().putBoolean(Prefs.KEY_ENABLED, false).apply();
        return false;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        mPrefs = Prefs.get(this);
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            // "Hide" in the notification
            mPrefs.edit().putBoolean(Prefs.KEY_ENABLED, false).apply();
            stopSelf();
            return START_NOT_STICKY;
        }
        // Every startForegroundService() must be answered by startForeground()
        startForeground(NOTIFICATION_ID, buildNotification(),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        if (!mPrefs.getBoolean(Prefs.KEY_ENABLED, false)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (!mStarted) {
            setUp();
        }
        return START_STICKY;
    }

    private void setUp() {
        mStarted = true;
        mPrefs.registerOnSharedPreferenceChangeListener(this);
        final IntentFilter screen = new IntentFilter(Intent.ACTION_SCREEN_ON);
        screen.addAction(Intent.ACTION_SCREEN_OFF);
        registerReceiver(mScreenReceiver, screen, Context.RECEIVER_NOT_EXPORTED);
        mScreenOn = getSystemService(PowerManager.class).isInteractive();
        mThread = new HandlerThread("WT20HudSampler");
        mThread.start();
        mSampling = new Handler(mThread.getLooper());
        mSampler = new Sampler(this);
        attach();
        schedule();
    }

    @Override
    public void onDestroy() {
        if (mStarted) {
            mPrefs.unregisterOnSharedPreferenceChangeListener(this);
            unregisterReceiver(mScreenReceiver);
            mSampling.removeCallbacksAndMessages(null);
            mThread.quitSafely();
            detach();
            mPrefs.edit().putString(Prefs.KEY_WINDOW, "").apply();
            mStarted = false;
        }
        mMain.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    // --- Sampling ------------------------------------------------------------------------------

    private final Runnable mTick = new Runnable() {
        @Override
        public void run() {
            final boolean[] metrics = Prefs.metrics(mPrefs);
            final CharSequence text = HudFormatter.format(mSampler.sample(metrics), metrics);
            mMain.post(() -> {
                if (mView != null) {
                    mView.setText(text);
                }
            });
            if (mScreenOn) {
                mSampling.postDelayed(this, Prefs.interval(mPrefs));
            }
        }
    };

    /** (Re)starts the sampling now, or stops it while the screen is off */
    private void schedule() {
        mSampling.removeCallbacks(mTick);
        if (mScreenOn) {
            mSampling.post(mTick);
        }
    }

    private final BroadcastReceiver mScreenReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            // Nothing to see with the screen off (and the tablet stays awake on AC, apply16c)
            mScreenOn = Intent.ACTION_SCREEN_ON.equals(intent.getAction());
            schedule();
        }
    };

    @Override
    public void onSharedPreferenceChanged(SharedPreferences p, String key) {
        if (key == null) {
            return;
        }
        switch (key) {
            case Prefs.KEY_ENABLED:
                if (!p.getBoolean(key, false)) {
                    stopSelf();
                }
                break;
            case Prefs.KEY_LOCKED:
                attach();
                break;
            case Prefs.KEY_SIZE:
            case Prefs.KEY_OPACITY:
                if (mView != null) {
                    style(mView);
                }
                break;
            case Prefs.KEY_INTERVAL:
                schedule();
                break;
            case Prefs.KEY_X:
            case Prefs.KEY_Y:
                if (!mDragging) {
                    placeSaved(); // "reset position" removes both
                }
                break;
            default:
                if (Prefs.isMetricKey(key)) {
                    schedule(); // show the change at once
                }
                break; // KEY_WINDOW: written here
        }
    }

    // --- The window ----------------------------------------------------------------------------

    /**
     * (Re)adds the window with the strongest type WindowManager accepts. TYPE_SYSTEM_ERROR is
     * deprecated for apps, which is about apps without INTERNAL_SYSTEM_WINDOW.
     */
    @SuppressWarnings("deprecation")
    private void attach() {
        detach();
        final boolean locked = mPrefs.getBoolean(Prefs.KEY_LOCKED, true);
        final int strongest = locked ? WindowManager.LayoutParams.TYPE_SECURE_SYSTEM_OVERLAY
                : WindowManager.LayoutParams.TYPE_SYSTEM_ERROR;
        final int fallback = WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY;
        if (add(strongest, locked, false) || add(fallback, locked, true)
                || add(fallback, locked, false)) {
            return;
        }
        Log.e(TAG, "WindowManager refused every window type");
        mPrefs.edit().putString(Prefs.KEY_WINDOW, Prefs.WINDOW_NONE).apply();
    }

    /**
     * Adds the window with this type; false if WindowManager refused it.
     * systemOverlay: TYPE_APPLICATION_OVERLAY marked as a system app's overlay
     * (SYSTEM_APPLICATION_OVERLAY), which games hiding overlays (HIDE_OVERLAY_WINDOWS) keep.
     */
    @SuppressWarnings("deprecation")
    private boolean add(int type, boolean locked, boolean systemOverlay) {
        try {
            if (type == WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY) {
                allowOverlays();
            }
            final Display display = getSystemService(DisplayManager.class)
                    .getDisplay(Display.DEFAULT_DISPLAY);
            // A window context of the window's own type: WindowManager rejects a mismatch
            final Context wc = createWindowContext(display, type, null);
            final WindowManager wm = wc.getSystemService(WindowManager.class);
            final TextView view = createView(wc, locked);
            final WindowManager.LayoutParams lp = layoutParams(type, locked, systemOverlay);
            wm.addView(view, lp);
            wc.registerComponentCallbacks(mWindowCallbacks);
            mWindowContext = wc;
            mWm = wm;
            mView = view;
            mLp = lp;
            final String name = type == WindowManager.LayoutParams.TYPE_SECURE_SYSTEM_OVERLAY
                    ? Prefs.WINDOW_SECURE
                    : type == WindowManager.LayoutParams.TYPE_SYSTEM_ERROR
                    ? Prefs.WINDOW_SYSTEM : Prefs.WINDOW_OVERLAY;
            mPrefs.edit().putString(Prefs.KEY_WINDOW, name).apply();
            Log.i(TAG, "window added: type " + type + (locked ? ", locked" : ", movable"));
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "window type " + type + (systemOverlay ? " (system overlay)" : "")
                    + " refused: " + e);
            return false;
        }
    }

    private void detach() {
        if (mView != null) {
            try {
                mWm.removeViewImmediate(mView);
            } catch (IllegalArgumentException e) {
                // already gone
            }
        }
        if (mWindowContext != null) {
            mWindowContext.unregisterComponentCallbacks(mWindowCallbacks);
        }
        mWindowContext = null;
        mWm = null;
        mView = null;
        mLp = null;
        mDragging = false;
    }

    private WindowManager.LayoutParams layoutParams(int type, boolean locked,
            boolean systemOverlay) {
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;
        if (locked) {
            flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        }
        final WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT,
                type, flags, PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.LEFT; // x / y from the top left, also in RTL
        lp.setTitle(TAG);
        lp.setFitInsetsTypes(0); // may sit on the status bar
        lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        if (type == WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY) {
            if (systemOverlay) {
                lp.setSystemApplicationOverlay(true);
            }
            if (locked) {
                lp.alpha = Math.min(1f,
                        getSystemService(InputManager.class).getMaximumObscuringOpacityForTouch());
            }
        }
        final int[] pos = savedPosition();
        lp.x = pos[0];
        lp.y = pos[1];
        return lp;
    }

    /** The fallback window type needs the SYSTEM_ALERT_WINDOW app-op: switch it on */
    private void allowOverlays() {
        if (Settings.canDrawOverlays(this)) {
            return;
        }
        try {
            getSystemService(AppOpsManager.class).setMode(AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW,
                    Process.myUid(), getPackageName(), AppOpsManager.MODE_ALLOWED);
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot allow overlays: " + e);
        }
    }

    private TextView createView(Context wc, boolean locked) {
        final TextView v = new TextView(wc);
        v.setTypeface(Typeface.MONOSPACE);
        v.setTextColor(HudFormatter.PLAIN);
        v.setShadowLayer(2f, 0f, 0f, Color.BLACK);
        v.setIncludeFontPadding(false);
        // Labels and numbers read left to right, also with the Arabic UI
        v.setLayoutDirection(View.LAYOUT_DIRECTION_LTR);
        v.setTextDirection(View.TEXT_DIRECTION_LTR);
        v.setGravity(Gravity.LEFT);
        v.setText("WT20 HUD");
        style(v);
        if (!locked) {
            v.setOnTouchListener(mDrag);
        }
        // A new size (other metrics, text size) may push it off the screen
        v.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight,
                oldBottom) -> {
            if (right - left != oldRight - oldLeft || bottom - top != oldBottom - oldTop) {
                mMain.post(this::placeSaved);
            }
        });
        return v;
    }

    /** Text size and the rounded, semi-transparent background */
    private void style(TextView v) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, Prefs.TEXT_SP[Prefs.textSize(mPrefs)]);
        v.setPadding(dp(6), dp(4), dp(6), dp(4));
        final GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(Prefs.opacity(mPrefs) * 255 / 100, 0, 0, 0));
        bg.setCornerRadius(dp(8));
        v.setBackground(bg);
    }

    // --- Position ------------------------------------------------------------------------------

    private int[] savedPosition() {
        return new int[] {mPrefs.getInt(Prefs.KEY_X, dp(12)), mPrefs.getInt(Prefs.KEY_Y, dp(48))};
    }

    /**
     * Back to the saved position, clamped to the screen. Clamping is not saved: after a
     * rotation and back the window returns where the owner put it.
     */
    private void placeSaved() {
        if (!mDragging) {
            final int[] pos = savedPosition();
            place(pos[0], pos[1]);
        }
    }

    /** Moves the window to x / y (px, top left), clamped to the screen */
    private void place(int x, int y) {
        if (mView == null) {
            return;
        }
        final Rect screen = mWm.getCurrentWindowMetrics().getBounds();
        final int cx = Math.max(0, Math.min(x, screen.width() - mView.getWidth()));
        final int cy = Math.max(0, Math.min(y, screen.height() - mView.getHeight()));
        if (cx != mLp.x || cy != mLp.y) {
            mLp.x = cx;
            mLp.y = cy;
            try {
                mWm.updateViewLayout(mView, mLp);
            } catch (IllegalArgumentException e) {
                // removed meanwhile
            }
        }
    }

    /** Rotation: the window context gets the new display size */
    private final ComponentCallbacks mWindowCallbacks = new ComponentCallbacks() {
        @Override
        public void onConfigurationChanged(Configuration newConfig) {
            mMain.post(HudService.this::placeSaved);
        }

        @Override
        @SuppressWarnings("deprecation") // still abstract in ComponentCallbacks
        public void onLowMemory() {
        }
    };

    /** Unlocked only: drag anywhere, the position is saved when the finger lifts */
    private final View.OnTouchListener mDrag = new View.OnTouchListener() {
        private float mDownX;
        private float mDownY;
        private int mStartX;
        private int mStartY;

        @Override
        public boolean onTouch(View v, MotionEvent e) {
            if (mLp == null) {
                return false;
            }
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    mDragging = true;
                    mDownX = e.getRawX();
                    mDownY = e.getRawY();
                    mStartX = mLp.x;
                    mStartY = mLp.y;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    place(mStartX + Math.round(e.getRawX() - mDownX),
                            mStartY + Math.round(e.getRawY() - mDownY));
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    mDragging = false;
                    mPrefs.edit().putInt(Prefs.KEY_X, mLp.x).putInt(Prefs.KEY_Y, mLp.y).apply();
                    return true;
                default:
                    return false;
            }
        }
    };

    // --- Notification --------------------------------------------------------------------------

    private Notification buildNotification() {
        final NotificationChannel channel = new NotificationChannel(CHANNEL,
                getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW);
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);

        final PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                PendingIntent.FLAG_IMMUTABLE);
        final PendingIntent hide = PendingIntent.getService(this, 1,
                new Intent(this, HudService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_hud)
                .setContentTitle(getString(R.string.notification_title))
                .setContentText(getString(R.string.notification_text))
                .setContentIntent(open)
                .addAction(new Notification.Action.Builder(
                        Icon.createWithResource(this, R.drawable.ic_hud),
                        getString(R.string.hide), hide).build())
                .setOngoing(true)
                .setShowWhen(false)
                .setCategory(Notification.CATEGORY_SERVICE)
                .build();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
