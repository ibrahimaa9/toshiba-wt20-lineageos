#!/bin/bash
# Runs as root on the NAS host: picks the Google apps out of the mounted Android 16 Play emulator
# image (/mnt/emu) into an "extracted" tree for gen-gapps.py, next to the MindTheGapps extras.
set -e
E=/mnt/emu
T=/mnt/wt20build/rootfs/tmp/host/gapps-final/system
rm -rf "$T"; mkdir -p "$T"
app() {  # app <partition> <kind> <dir in image> [name]
    local n=${4:-$3}
    mkdir -p "$T/$1/$2/$n"
    cp "$E/$1/$2/$3/"*.apk "$T/$1/$2/$n/$n.apk"
}
for a in PrebuiltGmsCore Phonesky Velvet PartnerSetupPrebuilt GoogleOneTimeInitializer ConfigUpdater WellbeingPrebuilt; do app product priv-app $a; done
app product priv-app GoogleRestorePrebuilt-v717308 GoogleRestorePrebuilt
for a in GoogleTTS LatinIMEGooglePrebuilt CalendarGooglePrebuilt GoogleContacts Maps Photos YouTube PrebuiltGmail Drive; do app product app $a; done
for a in GoogleServicesFramework GoogleFeedback; do app system_ext priv-app $a; done
mkdir -p "$T/product/etc/permissions" "$T/product/etc/sysconfig" "$T/product/etc/default-permissions" "$T/product/etc/security/fsverity" "$T/product/overlay" "$T/system_ext/etc/permissions"
cp "$E/product/etc/permissions/privapp-permissions-google-p.xml" "$E/product/etc/permissions/split-permissions-google.xml" "$T/product/etc/permissions/"
cp "$E/product/etc/sysconfig/google.xml" "$E/product/etc/sysconfig/google_build.xml" "$E/product/etc/sysconfig/google-hiddenapi-package-whitelist.xml" "$E/product/etc/sysconfig/wellbeing.xml" "$T/product/etc/sysconfig/"
cp "$E/product/etc/default-permissions/default-permissions.xml" "$T/product/etc/default-permissions/default-permissions-google.xml"
cp "$E/product/etc/security/fsverity/"*.der "$T/product/etc/security/fsverity/"
# No GoogleConfigOverlay / PixelConfigOverlayCommon: Pixel-only settings (config_servicesExtensionPackage=com.google.android.ext.services crashes system_server); MindTheGapps' Gms*Overlay come with the extras instead
cp "$E/system_ext/etc/permissions/privapp-permissions-google-se.xml" "$T/system_ext/etc/permissions/"
# MindTheGapps extras (uploaded to /tmp/mtg-extra with the same layout)
cp -r /tmp/mtg-extra/system/. "$T/"
chown -R 1000:1000 "$(dirname "$T")"
du -sh "$T"; find "$T" -type f | sed "s|$T/||" | sort
