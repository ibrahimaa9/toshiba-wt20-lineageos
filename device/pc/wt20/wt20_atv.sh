#!/system/bin/sh
# WT20 Android TV product (lineage_pc_x86_64_tv, apply22a): the tablet sits behind a TV, wired
# over HDMI, driven with an air mouse. Installed as /system/bin/wt20_tv.sh, so init.wt20.rc's
# wt20_tv service (ro.wt20.tv=1) starts it at boot completed, as root (su domain).
#
# Unlike the tablet TV variant's script (wt20_tv.sh) it touches neither the home app nor the
# display size: Android TV has its own launcher and is built at 320 dpi.
# Once, after setup: the display kept on while on the charger.
# Always: the tablet's own panel is dark while the TV is connected, and comes back when the
# cable is pulled. setprop persist.wt20.tv.panel_off 0 keeps the panel lit.

STATE=/data/misc/wt20-tv
MARK=$STATE/provisioned-atv

say() { log -t wt20_tv "$*"; echo "<6>wt20_tv: $*" > /dev/kmsg; }

provision() {
    [ -e "$MARK" ] && return 0
    [ "$(settings get secure user_setup_complete)" = "1" ] || return 1
    # 7 = AC | USB | wireless: the display stays on while the tablet is powered
    settings put global stay_on_while_plugged_in 7
    mkdir -p "$STATE" && : > "$MARK"
    say "TV setup done"
}

hdmi_connected() {
    for s in /sys/class/drm/card*-HDMI-A-*/status /sys/class/drm/card*-DP-*/status; do
        [ "$(cat "$s" 2>/dev/null)" = connected ] && return 0
    done
    return 1
}

# bl_power: 0 on, 4 off. It is independent of the brightness the light HAL writes.
set_panel() { # 0 or 4
    for b in /sys/class/backlight/*/bl_power; do
        [ -e "$b" ] && [ "$(cat "$b")" != "$1" ] && echo "$1" > "$b"
    done
}

panel=on
while true; do
    provision
    want=on
    if [ "$(getprop persist.wt20.tv.panel_off)" != "0" ] && hdmi_connected; then
        want=off
    fi
    if [ "$want" = off ]; then
        # Written every round: a screen-off / screen-on cycle unblanks the backlight again
        set_panel 4
    else
        [ "$panel" = off ] && set_panel 0
    fi
    if [ "$want" != "$panel" ]; then
        say "TV $( [ "$want" = off ] && echo connected: panel off || echo disconnected: panel on)"
        panel=$want
    fi
    sleep 3
done
