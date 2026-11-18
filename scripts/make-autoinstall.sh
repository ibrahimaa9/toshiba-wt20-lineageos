#!/bin/bash
# Runs as the build user INSIDE the chroot: copies the installer image to ~/images and makes a second copy
# for the Toshiba WT20 tablet, which has no keyboard. On that machine only (SMBIOS product name), GRUB
# starts an unattended install after a countdown: the disk is partitioned and erased with the
# androidboot.install_disk/install_erase presets, then a small add-on ramdisk (wt20-auto.cpio, appended to
# the recovery initrd) installs the OTA zip from the stick the way recovery's "Apply update" does and
# reboots. Once LineageOS is on a disk, the stick boots from that disk instead, so leaving it plugged in
# cannot reinstall. Other PCs get the normal menu.
# Usage: make-autoinstall.sh [disk name, default sda]
set -euo pipefail
DISK=${1:-sda}
OUT=~/los/out/target/product/pc_x86_64
SRC=$(ls -t $OUT/lineage-23.2-*-pc_x86_64.img | head -1)
ZIP=${SRC%.img}.zip
ZIP_NAME=$(basename "$ZIP")
mkdir -p ~/images
cp "$SRC" ~/images/
DST=~/images/$(basename "${SRC%.img}")-wt20-auto.img
cp "$SRC" "$DST"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- payload location inside the zip, what recovery's SetUpAbUpdateCommands computes at runtime
read -r PAYLOAD_OFFSET < <(python3 - "$ZIP" <<'EOF'
import struct, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
i = z.getinfo('payload.bin')
assert i.compress_type == 0
with open(sys.argv[1], 'rb') as f:
    f.seek(i.header_offset)
    n, e = struct.unpack('<HH', f.read(30)[26:30])
print(i.header_offset + 30 + n + e)
EOF
)
HEADERS=$(unzip -p "$ZIP" payload_properties.txt)

# --- add-on ramdisk: init.recovery.virt.rc without "enable recovery" (no menu), plus the install service
mkdir -p "$WORK/cpio/system/bin"
python3 - "$OUT/recovery/root/init.recovery.virt.rc" "$WORK/cpio/init.recovery.virt.rc" <<'EOF'
import re, sys
src, dst = sys.argv[1:]
text = open(src).read()
block = 'on property:ro.boot.install=1 && property:vendor.preinstall_check.success=1\n    enable recovery\n'
assert block in text, 'enable recovery block not found'
text = text.replace(block, '''# wt20-auto.cpio: with a preset install disk, install the zip from the stick and reboot instead of
# showing the recovery menu (the tablet has no keyboard)
service pc_auto_install /system/bin/sh /system/bin/pc_auto_install.sh
    user root
    group root
    seclabel u:r:recovery:s0
    oneshot
    disabled

on property:ro.boot.install=1 && property:ro.boot.install_disk=* && property:vendor.preinstall_check.success=1
    start pc_auto_install
''')
open(dst, 'w').write(text)
EOF

cat > "$WORK/cpio/system/bin/pc_auto_install.sh" <<EOF
# Runs as recovery (wt20-auto.cpio) once pc_create_partition_table.sh is done: installs the OTA zip
# from the installer stick like recovery's "Apply update" (install.cpp SetUpAbUpdateCommands) and reboots.
# Level 0 shows on screen whatever the loglevel is.
say() { echo "<0>wt20-auto: \$*" > /dev/kmsg; }
fail() { say "ERROR: \$*"; say "Installation stopped. Hold the power button to turn the tablet off."; exit 1; }

ZIP_NAME="$ZIP_NAME"
PAYLOAD_OFFSET=$PAYLOAD_OFFSET
HEADERS="$HEADERS"

say "disk partitioned, installing LineageOS from the USB stick..."

# The installer stick: a FAT32 superfloppy labelled "install" (as in pc_save_logs.sh)
disk=
for sys in /sys/class/block/sd*; do
    name=\${sys##*/}
    [ -e \$sys/partition ] && continue
    if [ "\$(dd if=/dev/block/\$name bs=1 skip=82 count=5 2>/dev/null)" = "FAT32" ] &&
            [ "\$(dd if=/dev/block/\$name bs=1 skip=71 count=7 2>/dev/null)" = "install" ]; then
        disk=\$name
        break
    fi
done
[ -n "\$disk" ] || fail "installer USB stick not found"
MNT=/tmp/wt20_stick
mkdir -p \$MNT
grep -q " \$MNT " /proc/mounts || mount -t vfat -o rw,noatime /dev/block/\$disk \$MNT ||
    fail "cannot mount the USB stick (\$disk)"
[ -f "\$MNT/\$ZIP_NAME" ] || fail "\$ZIP_NAME not found on the USB stick"

# Logs go to the stick (wt20-logs/<n>/), to be read on another PC
LOGDIR=\$MNT/wt20-logs/\$(date +%Y%m%d-%H%M%S)
save_logs() {
    mkdir -p \$LOGDIR
    cp /tmp/wt20_update.log \$LOGDIR/update_engine.log 2>/dev/null
    dmesg > \$LOGDIR/dmesg.txt 2>&1
    cp /tmp/recovery.log \$LOGDIR/recovery.log 2>/dev/null
    sync
}

say "writing the system from \$disk to the internal disk, this takes a few minutes, do not turn off"
if /system/bin/update_engine_sideload --payload=file://\$MNT/\$ZIP_NAME --offset=\$PAYLOAD_OFFSET \\
        --headers="\$HEADERS" > /tmp/wt20_update.log 2>&1; then
    say "LineageOS installed. Restarting in 10 seconds; you can remove the USB stick."
    save_logs
    umount \$MNT
    sleep 10
    setprop sys.powerctl reboot
else
    grep -i "error\|fail\|ErrorCode" /tmp/wt20_update.log | grep -v "death re\|unlinking" | tail -n 10 | cut -c1-300 |
        while read -r line; do say "\$line"; done
    save_logs
    say "logs saved on the USB stick in \${LOGDIR#\$MNT/}"
    umount \$MNT
    fail "update_engine_sideload failed"
fi
EOF
chmod 644 "$WORK/cpio/init.recovery.virt.rc"
chmod 755 "$WORK/cpio/system/bin/pc_auto_install.sh"
# LZ4 legacy frame like the Android ramdisks before it: the kernel's lz4 decompressor keeps going
# through consecutive legacy archives, but treats anything else after one (an uncompressed or
# gzip'd cpio) as corrupt data and stops unpacking
( cd "$WORK/cpio" && find . | sort | cpio -o -H newc --owner=0:0 --quiet | lz4 -l -9 -q > "$WORK/wt20-auto.cpio" )

# --- GRUB menu: recognise the tablet, add the unattended entry, boot from disk once installed
mtype -i "$DST" ::/boot/grub/grub.cfg > "$WORK/grub.cfg"
python3 - "$WORK/grub.cfg" "$DISK" <<'EOF'
import re, sys
path, disk = sys.argv[1:]
text = open(path).read()
# the original install function, with the add-on cpio before bootconfig.bin (which must stay last)
m = re.search(r'function boot_android_install \{\n(.*?)\n\}\n', text, re.S)
assert m, 'boot_android_install not found'
body = m.group(1)
assert '(vendor_boot)/bootconfig.bin' in body
auto_body = body.replace('(vendor_boot)/bootconfig.bin', '/wt20-auto.cpio (vendor_boot)/bootconfig.bin')
block = '''# Unattended install for the Toshiba WT20 tablet (no keyboard), added by make-autoinstall.sh.
# The entry exists only on that machine, so this stick cannot erase another PC without questions.
function boot_android_install_auto {
%(auto_body)s
}

insmod smbios
insmod regexp
set system_product=
smbios --type 1 --get-string 5 --set system_product
if regexp 'WT20' "$system_product"; then
	# LineageOS is installed when its EFI partition was flashed, not just when the disk was
	# partitioned (a failed install leaves PERSIST behind)
	set wt20_installed=
	if [ -n "$partition_persist" ]; then
		if [ -f (${disk_boot},gpt1)/boot/grub/.is_esp_part_on_android_boot_device ]; then
			set wt20_installed=1
		fi
	fi
	if [ -n "$wt20_installed" ]; then
		# The stick was left plugged in after the install: boot the disk
		set default=bootfromdisk
		set timeout=5
	else
		set default=wt20-auto
		set timeout=20
		menuentry "Install automatically on the internal disk (%(disk)s is ERASED, no questions)" --id wt20-auto --class monitor-arrow-down {
			boot_android_install_auto androidboot.install_disk=%(disk)s androidboot.install_erase=1 loglevel=1
		}
	fi
fi

''' % {'auto_body': auto_body, 'disk': disk}
marker = 'menuentry "Install '
at = text.index(marker)
text = text[:at] + block + text[at:]
old = 'menuentry "Boot from disk" --class harddisk {'
assert old in text
text = text.replace(old, 'menuentry "Boot from disk" --id bootfromdisk --class harddisk {')
open(path, 'w').write(text)
EOF

mcopy -o -i "$DST" "$WORK/grub.cfg" ::/boot/grub/grub.cfg
mcopy -o -i "$DST" "$WORK/wt20-auto.cpio" ::/wt20-auto.cpio
echo "--- patched part of grub.cfg:"
mtype -i "$DST" ::/boot/grub/grub.cfg | sed -n '/Unattended install/,/^menuentry "Boot from disk"/p'
echo "--- add-on ramdisk:"
lz4 -d -q -c "$WORK/wt20-auto.cpio" | cpio -t --quiet
mdir -i "$DST" ::/
cd ~/images && sha256sum "$(basename "$SRC")" "$(basename "$DST")" | tee -a SHA256SUMS
