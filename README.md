# LineageOS 23.2 (Android 16) for the Toshiba Portégé WT20

An unofficial LineageOS 23.2 build for the **Toshiba Portégé WT20-B** Windows tablet
(Core M-5Y10c, Intel HD 5300, 4 GB RAM). It is built on
[LineageOS for PC (LOS-PC)](https://github.com/LOS-PC), with the drivers, HALs and fixes this
tablet needs on top.

> **Android TV on a WT20, next to Windows?** See the companion project
> [toshiba-wt20-android-tv](https://github.com/ibrahimaa9/toshiba-wt20-android-tv).

## Status

| Works | Notes |
|---|---|
| Display, touch (Atmel maXTouch) | The GPU needs the "Intel pre-Skylake GPU with workarounds" boot entry. On a WT20 it is picked automatically. |
| Wi-Fi (Intel 7265D), Bluetooth | |
| Auto-rotation, compass, gyroscope, light sensor | ST HID sensor hub, through the IIO sensors HAL. The compass starts calibrated. |
| Both cameras (front and rear) | External camera HAL (UVC) |
| Sound: speakers, headphone jack, internal mic, HDMI audio | |
| Sleep (S3) and battery | Charging limit at 80 % (LineageOS charging control) |
| Hardware video decoding (H.264, VP8, MPEG-2) | FFmpeg 7.1.3 through VA-API in a Codec2 HAL. VP9 and AV1 have no hardware support on Broadwell, so the software decoders for them are disabled and YouTube uses H.264. |
| ARM apps | Google's ARM64 translator (`ndk_translation`) |
| Google apps (Play Store, Play services) | Included in the image. See [Google certification](#google-certification). |
| Extras | "Game Mode" app (per-game resolution and FPS cap), "WT20 HUD" performance overlay, Miracast, LiveDisplay, thermal and power HALs |

**Known limitations**

- No Widevine DRM (ClearKey only), so Netflix and similar apps play at low resolution or not at all.
- The device is not Google-certified. Play Integrity fails, and banking apps that require it will not work.
- The Wi-Fi regulatory country is set to Iraq (`ro.boot.wificountrycode=IQ` in `device/pc/wt20/wt20.mk`). If you build it yourself, change it to your country.
- While asleep on battery, Wi-Fi is off. WoWLAN hangs this Wi-Fi chip. While on the charger, the tablet keeps Wi-Fi alive.
- The build is `userdebug`, signed with test keys.
- **Testing:** this exact public build was boot-tested in a virtual machine. Its features were tested on real WT20 tablets in earlier builds of the same tree.

## Download

| File | What it is |
|---|---|
| `lineage-23.2-<date>-UNOFFICIAL-pc_x86_64.img` | Normal installer, with a menu. Needs a keyboard, or the volume and power keys. |
| `lineage-23.2-<date>-UNOFFICIAL-pc_x86_64-wt20-auto.img` | Unattended installer for a WT20 with no keyboard. **It erases the whole internal disk after a 20-second countdown, Windows included.** |

**Links:** DOWNLOAD_LINKS_HERE. Check the files against `SHA256SUMS` after you download them.

## Install

> **The installer erases the whole internal disk (Windows and all files).** Back up first.
> To keep Windows, use the dual-boot installer from
> [toshiba-wt20-android-tv](https://github.com/ibrahimaa9/toshiba-wt20-android-tv) instead.

1. **Turn off Secure Boot.** In Windows: Settings > Update & Security > Recovery > Advanced startup >
   Restart now > Troubleshoot > Advanced options > UEFI Firmware Settings. Then
   Security > Secure Boot > Disabled. Save and exit.
2. **Write the image to a USB stick** (2 GB or larger, erased) with
   [Rufus](https://rufus.ie) in DD mode, [balenaEtcher](https://etcher.balena.io), or `dd`.
   The tablet has one micro-USB port, so you need an OTG adapter.
3. **Boot from the stick.** In Windows: Advanced startup > Use a device > EFI USB Device.
4. **Install.**
   - With the normal image: choose "Install". Move with the volume keys and confirm with the power button. Then choose the internal disk.
   - With the `wt20-auto` image: do nothing. After 20 seconds it partitions the disk, installs, and restarts (about 10 minutes).
5. Remove the stick when the tablet restarts. The first boot takes a few minutes.

## Google certification

The image includes Google Play, but Google treats an uncertified device as unknown. If Play
services say the device is not certified:

1. Install a device-ID app, or run `adb shell 'sqlite3 /data/data/com.google.android.gsf/databases/gservices.db "select value from main where name=\"android_id\""'` with root.
2. Register that GSF ID at <https://www.google.com/android/uncertified>.
3. Clear Play Store and Play services data, wait a few minutes, and sign in.

This is Google's own registration page for custom ROMs. The build does not spoof another
device's fingerprint, and it never will.

## Build it yourself

You need a Linux machine (Ubuntu 24.04 tested) with about 400 GB free and 32 GB RAM (or zram).
The first build takes 6–8 hours on an 8-core CPU.

```bash
# 1. LOS-PC 23.2, pinned to the revisions the images were built from
mkdir ~/los && cd ~/los
repo init -u https://github.com/LOS-PC/android.git -b lineage-23.2 --git-lfs
cp /path/to/this/repo/manifests/pinned-manifest.xml .repo/manifests/wt20-pinned.xml
repo init -m wt20-pinned.xml
mkdir -p .repo/local_manifests && cp /path/to/this/repo/manifests/wt20.xml .repo/local_manifests/
repo sync -c -j8

# 2. FFmpeg 7.1.3 sources (unpatched) for the video decoding HAL
mkdir -p external/ffmpeg && curl -L https://ffmpeg.org/releases/ffmpeg-7.1.3.tar.xz | tar -xJ --strip-components=1 -C external/ffmpeg

# 3. this repository's changes (the device tree, FFmpeg's build files, and the patches;
#    the patch to lineage_pc_x86_64.mk makes LOS-PC's tablet product inherit device/pc/wt20)
/path/to/this/repo/scripts/apply-patches.sh ~/los
```

**Google apps and the ARM translator are not in this repository.** They are Google's software
and may not be redistributed. The build expects:

- `vendor/gapps`, generated from Google apps you download yourself. Get the Android 16
  x86_64 Google Play emulator image (`system-images;android-36;google_apis_playstore;x86_64`
  in the Android SDK manager) and MindTheGapps-15.0.0-x86_64 (for its SetupWizard and overlays).
  `scripts/emu-collect.sh` picks the apps out of the mounted image; edit its paths for your
  machine. Then `scripts/gen-gapps.py <collected dir> ~/los/vendor/gapps` writes the build files.
- `vendor/google/ndk_translation`, from the same emulator image.

To build without them, remove the `vendor/gapps/gapps.mk` line from
`device/pc/pc_x86_64/lineage_pc_x86_64.mk` and set `WT20_ARM_TRANSLATION := false` in
`device/pc/wt20/options.mk`.

```bash
# 5. build: out/target/product/pc_x86_64/lineage-23.2-*-pc_x86_64.img (+ the OTA zip)
scripts/build.sh            # breakfast pc_x86_64 userdebug && m
scripts/make-autoinstall.sh # optional: the unattended -wt20-auto.img copy
```

## What is in this repository

| Path | Contents |
|---|---|
| `device/pc/wt20/` | The WT20 additions. `wt20.mk` and the `WT20_*` switches in `options.mk`, init scripts, sepolicy, overlays, the thermal, power, touch and charging HALs, the Game Mode and HUD apps, the TV product's overlays. |
| `external/ffmpeg_codec2/` | Codec2 HAL on FFmpeg, based on Michael Goffioul's and KonstaKANG's work, with VA-API decoding added (`C2FFMPEGVaapi.cpp`). |
| `external/ffmpeg/` | Our `Android.bp` and configure output for FFmpeg 7.1.3. The sources come from ffmpeg.org. |
| `patches/` | Changes to 19 LOS-PC / LineageOS / AOSP projects, as `git diff` against the pinned revisions (`series` lists project and patch). They cover the kernel config (sensor hub, Toshiba ACPI, RAPL), hwcomposer planes on Broadwell, minigbm, the audio HAL, the i965/iHD VA-API drivers, the IIO sensors HAL, the GRUB defaults for the WT20, the unattended installer, and the dual-boot partitioning. |
| `manifests/` | `pinned-manifest.xml` (the exact source revisions) and `wt20.xml` (extra projects). |
| `scripts/` | Build, patch, installer-image and Google-apps tools. |

## Licenses and credits

- Our own code is Apache-2.0, like AOSP. Kernel changes (`patches/kernel_pc_pc.patch`) are
  GPL-2.0. Patches to other projects keep those projects' licenses. FFmpeg is LGPL-2.1-or-later.
- The kernel source is [LOS-PC/android_kernel_pc_pc](https://github.com/LOS-PC/android_kernel_pc_pc)
  at the revision in `manifests/pinned-manifest.xml`, plus `patches/kernel_pc_pc.patch`.
- Thanks to [LineageOS](https://lineageos.org), [LOS-PC](https://github.com/LOS-PC), the
  [FFmpeg](https://ffmpeg.org) project, Michael Goffioul and KonstaKANG (ffmpeg_codec2), Intel
  (VA-API drivers) and Project Celadon (media-driver).
- This project is not affiliated with Toshiba, Google or LineageOS. Use it at your own risk.
