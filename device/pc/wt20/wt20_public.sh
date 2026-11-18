#!/system/bin/sh
# Started by init.wt20.rc at boot completed, as root (su domain), as /system/bin/wt20_remote.sh:
# the public build's boot tweaks (apply23a). Unlike the owner's wt20_remote.sh it joins no Wi-Fi
# network and does not open ADB: Developer options work as on any Android device.

say() { log -t wt20_remote "$*"; echo "<6>wt20_remote: $*" > /dev/kmsg; }

# Always-on display defaults to on here (aod_default_on overlay); on this tablet it only
# keeps the display dozing with a wake lock, so the kernel never suspends. Off unless the
# user has set it themselves.
if [ -z "$(settings get secure doze_always_on | grep -v null)" ]; then
    settings put secure doze_always_on 0
fi
# Frame rate overrides are enabled for per-game caps (Game Mode); without a minimum the
# compositor may render the whole UI at 30 fps on the single 60 Hz panel mode
if [ -z "$(settings get system min_refresh_rate | grep -v null)" ]; then
    settings put system min_refresh_rate 60
fi
# init.virt.rc points the kernel at the firmware partition only, so firmware in /vendor/firmware
# always went through the ueventd fallback (also at every suspend); init itself may not write this
echo -n /vendor/firmware/,/vendor/firmware_mnt/ > /sys/module/firmware_class/parameters/path
# 3.7 GB of RAM with lz4 zram (~3.5:1): swap cold pages before lmkd kills apps
echo 100 > /proc/sys/vm/swappiness
# pc_bt_detect.sh enables USB wakeup on the Bluetooth adapter (laptops that cut the port's
# power in S3). This Toshiba does the opposite: with wakeup enabled the adapter is reset on
# resume and comes back as a new hci device, and the Bluetooth stack aborts on the next
# command; with it disabled hci0 survives suspend (tested 2026-11-08).
for h in /sys/class/bluetooth/hci*; do
    [ -e "$h/device/../power/wakeup" ] && echo disabled > "$h/device/../power/wakeup"
done
# The Bluetooth adapter's USB port loses power in S3 on this Toshiba; the port change woke
# the xHCI controller immediately, so the tablet never stayed asleep (suspend/resume every
# 2 s) and a power-button press during that churn was lost (5-7 s hold needed). USB may not
# wake the system: the power button, the lid and RTC alarms still do.
if grep -qE '^XHC.*enabled' /proc/acpi/wakeup; then
    echo XHC > /proc/acpi/wakeup
fi
for d in /sys/bus/usb/devices/usb*/power/wakeup /sys/bus/pci/devices/0000:00:14.0/power/wakeup; do
    [ -e "$d" ] && echo disabled > "$d"
done
