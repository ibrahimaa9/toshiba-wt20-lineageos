#!/system/bin/sh
# Started by init.wt20.rc at boot completed, as root (su domain): keeps the tablet out of suspend
# while it is on the charger, so Wi-Fi, downloads and ADB keep running with the screen off (Wi-Fi
# drops on every suspend, and WoWLAN wedges this Intel 7265). On battery it sleeps normally.
# setprop persist.wt20.ac_wake 0 turns it off.
held=0
while true; do
    on=0
    if [ "$(getprop persist.wt20.ac_wake)" != "0" ]; then
        for s in /sys/class/power_supply/*; do
            [ "$(cat $s/type 2>/dev/null)" = Mains ] && [ "$(cat $s/online 2>/dev/null)" = 1 ] && on=1
        done
    fi
    if [ $on = 1 ] && [ $held = 0 ]; then
        echo wt20_ac > /sys/power/wake_lock && held=1
        echo "<6>wt20_acwake: on AC, holding wakelock" > /dev/kmsg
    elif [ $on = 0 ] && [ $held = 1 ]; then
        echo wt20_ac > /sys/power/wake_unlock; held=0
        echo "<6>wt20_acwake: on battery, released wakelock" > /dev/kmsg
    fi
    sleep 15
done
