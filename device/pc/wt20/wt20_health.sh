#!/system/bin/sh
# WT20 health log: collects the system's errors (crashes, ANRs, SELinux denials, HAL/kernel
# errors, memory and thermal state) into one report so they can be reviewed from the
# owner's PC. Started by init.wt20.rc once the system is up (then every hour, as root in
# the su domain) or by hand: sh /system/bin/wt20_health.sh [out dir]
#
# Reports: /data/misc/wt20-health/<boot>-<time>.txt (last 30 kept) and, when the pc_logs
# partition is mounted, a copy in /mnt/vendor/pc_logs/health/ (survives a reinstall).
# Pull them with tools/health-pull.sh on the PC.
if [ "$1" = loop ]; then
    # init service: a report 2 minutes after boot, then one every hour
    sleep 120
    while true; do
        sh "$0"
        sleep 3600
    done
fi
OUT=${1:-/data/misc/wt20-health}
PCLOGS=/mnt/vendor/pc_logs/health
mkdir -p $OUT
BOOT=$(getprop ro.boottime.init | cut -c1-6)
F=$OUT/$(date +%Y%m%d-%H%M%S)-boot$BOOT.txt
say() { echo "<6>wt20_health: $*" > /dev/kmsg; }

{
echo "# WT20 health report $(date) uptime=$(cut -d' ' -f1 /proc/uptime)s"
echo "# build $(getprop ro.lineage.version) kernel $(uname -r) sku=$(getprop ro.boot.product.vendor.sku)"
echo
echo "## summary"
printf 'crashes (FATAL EXCEPTION): %s\n' "$(logcat -d -b crash 2>/dev/null | grep -c 'FATAL EXCEPTION')"
printf 'native crashes (tombstones): %s\n' "$(ls /data/tombstones 2>/dev/null | grep -c tombstone_)"
printf 'ANRs: %s\n' "$(ls /data/anr 2>/dev/null | wc -l)"
printf 'selinux denials (enforced): %s\n' "$(dmesg | grep -c 'avc:  denied.*permissive=0')"
printf 'kernel errors/warnings: %s\n' "$(dmesg -l emerg,alert,crit,err 2>/dev/null | wc -l)"
printf 'service restarts: %s\n' "$(dmesg | grep -c "init: Service '.*' (pid [0-9]*) exited with status")"
echo

echo "## memory"
free -m | head -2
cat /proc/swaps
echo "## thermal"
for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/temp)"; done
echo "## cpu freq"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo

echo "## hardware state"
echo "bluetooth hci: $(ls /sys/class/bluetooth 2>/dev/null | tr '\n' ' ')"
echo "cameras: $(dumpsys media.camera 2>/dev/null | grep 'Number of camera devices' | head -1)"
echo "sensors active: $(dumpsys sensorservice 2>/dev/null | grep -A1 'Active sensors' | tail -1)"
echo "wifi: $(cmd wifi status 2>/dev/null | head -1)"
echo "camera provider: $(getprop init.svc.vendor.camera.provider-ext) sensors hal: $(getprop init.svc.vendor.sensors-hal-multihal)"
echo

echo "## app crashes (logcat crash buffer, last 300 lines)"
logcat -d -b crash 2>/dev/null | tail -n 300
echo
echo "## ANR traces present"; ls -la /data/anr 2>/dev/null
echo "## tombstones"; ls -la /data/tombstones 2>/dev/null | tail -n 10
for t in $(ls -t /data/tombstones/tombstone_* 2>/dev/null | grep -v '\.pb$' | head -3); do
    echo "### $t"; grep -E '^(Cmdline|signal|Abort message|  #0[0-4] )' $t | head -8
done
echo

echo "## selinux denials (unique, enforced)"
dmesg | grep 'avc:  denied' | grep 'permissive=0' | sed -E 's/.*avc: +denied +//; s/ pid=[0-9]+//; s/ ino=[0-9]+//; s/audit\([^)]*\)//; s/ comm="[^"]*"//' | sort | uniq -c | sort -rn | head -n 40
echo
echo "## kernel errors"
dmesg -l emerg,alert,crit,err 2>/dev/null | grep -v 'avc:' | tail -n 60
echo
echo "## service exits"
dmesg | grep -E "init: Service '.*' (exited|received signal)" | tail -n 30
echo
echo "## HAL / hardware errors (logcat, errors only, filtered)"
logcat -d '*:E' 2>/dev/null | grep -iE 'Sensors|ExtCam|camera|Bluetooth|bluetooth|audio|AudioFlinger|SurfaceFlinger|hwc|gralloc|wifi|wpa|iwl|thermal|zram|lowmemory|lmkd' | grep -vE 'CameraController|Davey' | tail -n 150
echo
echo "## lmkd kills"
logcat -d -b all 2>/dev/null | grep -E 'lowmemorykiller|lmkd.*kill' | tail -n 20
} > $F 2>&1

ls -t $OUT/*.txt 2>/dev/null | tail -n +31 | xargs rm -f 2>/dev/null
if [ -d /mnt/vendor/pc_logs ] && mkdir -p $PCLOGS 2>/dev/null; then
    cp $F $PCLOGS/ 2>/dev/null && ls -t $PCLOGS/*.txt 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null
fi
say "report written: $F ($(grep -c '' $F) lines)"
