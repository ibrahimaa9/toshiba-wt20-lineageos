#!/system/bin/sh
# WT20_WIFI_STA_AP (apply15p): add wlan1 next to the station wlan0 on the Intel 7265 (iwlwifi
# allows 1 station + 1 AP on one channel). wlan0 comes with the iwlwifi module (loaded at
# init by dlkm_loader); wait for it for up to 30 s.
i=0
while [ $i -lt 30 ]; do
    [ -e /sys/class/net/wlan1 ] && exit 0
    if [ -e /sys/class/net/wlan0 ]; then
        /system/bin/iw dev wlan0 interface add wlan1 type managed && exit 0
        log -t wt20_wifi_ap "iw could not add wlan1"
        exit 1
    fi
    sleep 1
    i=$((i + 1))
done
log -t wt20_wifi_ap "no wlan0 after 30 s: hotspot interface wlan1 not created"
exit 1
