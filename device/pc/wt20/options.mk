#
# Toshiba Portege WT20-B: build options read by virt-common / mainline-common. Included
# (not inherited) at the top of lineage_pc_x86_64.mk: inherited makefiles are imported in
# sorted order, so wt20.mk would be read after device.mk and these would come too late.
#

# Sensors: the IIO HAL (hardware/intel/sensors-iio) on the ST sensor hub's HID sensor
# devices (kernel: wt20.config). Replaces virt-common's virtual cuttlefish sensors.
TARGET_SENSORS_HAL := iio

# Cameras: the two UVC webcams (front 04F2:B470, rear 04F2:B50B) through the external
# camera HAL
TARGET_SUPPORTS_EXTERNAL_CAMERAS := true

# ARM apps (TikTok, most games): Google's ARM64-to-x86_64 translator from the Android emulator
# image (vendor/google/ndk_translation). Android lists arm64-v8a after x86_64, so Google Play offers
# apps that ship only ARM code, and runs their native code translated. 64-bit ARM only. Read by
# wt20.mk (files, properties) and pc_x86_64/BoardConfig.mk (ABI list, SELinux); false = x86_64 only.
WT20_ARM_TRANSLATION := true

# WT20_QUICK_WINS (apply15k): Vulkan 1.1 feature flag (hasvk on Gen8), no always-on display
# offer (backlit LCD; the SoC must sleep), tablet characteristics (tablet strings/resources)
TARGET_VULKAN_FEATURE_1_1 := true
TARGET_VIRT_AOD_DEFAULT_ON := false
PRODUCT_CHARACTERISTICS := tablet

# WT20_VAAPI_DRIVER (apply15x): Intel's i965 VA-API driver (external/intel-vaapi-driver 2.4.1)
# + libva + vainfo, to prove hardware video decode works before a Codec2 HAL uses it
WT20_VAAPI_DRIVER := true
# WT20_IHD_DRIVER (apply21a): Intel's iHD VA-API driver (external/intel-media-driver, Celadon's
# media-driver 24.3.4, BDW on) next to i965; the HAL uses it when
# persist.vendor.wt20.vadriver=iHD (default i965). Needs WT20_VAAPI_DRIVER (libva).
WT20_IHD_DRIVER := true

# WT20_GAME_MODE_APP (apply15o): the "Game Mode" app (device/pc/wt20/apps/GameMode): per-game
# resolution scale, FPS cap and game mode through GameManagerService; false leaves it out
WT20_GAME_MODE_APP := true

# WT20_CHARGING_CONTROL (apply15l): LineageOS charging control on the Toshiba EC's 80% battery
# charge mode (BAT1/charge_control_end_threshold; wt20.mk). false = no charging control HAL;
# the EC keeps whatever mode was last written.
WT20_CHARGING_CONTROL := true

# WT20_POWER_HAL (apply15m): Lineage's perfmgr power HAL with the WT20 powerhint.json
# (device/pc/wt20/power: touch/launch boosts, game CPU/GPU floors, Battery Saver caps; no ADPF
# yet). Read before device/mainline/common's "TARGET_POWER_HAL ?= default-aidl". false = the
# example HAL (no effect on clocks).
WT20_POWER_HAL := true
ifeq ($(WT20_POWER_HAL),true)
TARGET_POWER_HAL := perfmgr-lineage
endif

# WT20_RAPL (apply15m): Intel RAPL powercap driver in the kernel (pc_x86_64/BoardConfig.mk merges
# device/pc/wt20/wt20-rapl.config): package energy counters and power limits, read-only use
WT20_RAPL := true

# WT20_THERMAL_HAL (apply15n): the WT20 thermal HAL (device/pc/wt20/thermal): CPU package
# temperature as CPU and SKIN sensors, LIGHT/MODERATE/SEVERE at 80/88/94 C, for the framework's
# thermal status and the games' thermal headroom. false = no thermal HAL (as before).
WT20_THERMAL_HAL := true

# WT20_WIFI_STA_AP (apply15p, B6): hotspot while connected to Wi-Fi, through the AIDL Wi-Fi HAL
# (generic nl80211 legacy HAL) and a second interface wlan1. The 7265 runs the AP only on the
# station's channel. Off by default: it changes how the only remote link (Wi-Fi) is managed.
WT20_WIFI_STA_AP := false

# WT20_MIRACAST (apply15p, B7): Miracast source (Cast > Enable wireless display) and the
# standard P2P device interface for Wi-Fi Direct (wifi.direct.interface=p2p-dev-wlan0)
WT20_MIRACAST := true

# WT20_LIVEDISPLAY (apply15q, B8): LiveDisplay picture adjustment (hue, saturation, intensity,
# contrast) in software through DisplayTransformManager; false hides it again
WT20_LIVEDISPLAY := true

# WT20_HDMI_AUDIO (apply15s, C2): sound over HDMI / DisplayPort (HD Audio HDMI card) when a
# monitor or TV with speakers is plugged in (picture tested on an LG TV 2026-11-09; ELD valid). With it off the
# HAL refuses HDMI and sound stays on the speaker / jack.
WT20_HDMI_AUDIO := true

# WT20_TOUCH_HAL (apply15v): Settings > Display > High touch sensitivity (device/pc/wt20/touch).
# Off: a declared touch HAL that fails to start makes system_server wait for it at boot, and
# without adb over USB that needs a rollback. Set to true for a build that can be rolled back.
WT20_TOUCH_HAL := false

# WT20_HW_VIDEO_DECODE (apply17a/b/c): H.264 / VP8 / MPEG-2 on the HD 5300 through FFmpeg's VA-API
# hwaccel in a Codec2 HAL (external/ffmpeg, external/ffmpeg_codec2, device/pc/wt20/media); the
# software c2.android decoders stay listed after it as the fallback. Needs WT20_VAAPI_DRIVER.
# Off until tested on a build that can be rolled back. At run time, as root (adb root):
# setprop persist.vendor.wt20.hwdec 0 + reboot turns it off; tools/verify17.sh checks it.
# Off here still has Soong parse external/ffmpeg, external/ffmpeg_codec2 and
# device/pc/wt20/media: remove those three together to revert (apply17c's docstring).
WT20_HW_VIDEO_DECODE := true

# WT20_PERF_HUD (apply18a): the "WT20 HUD" app (device/pc/wt20/apps/PerfHud): an always-on-top
# performance overlay (FPS, CPU, GPU, temperatures, RAM, battery) with a Quick Settings tile,
# and its SELinux reads (pc_x86_64/BoardConfig.mk); false leaves both out
WT20_PERF_HUD := true

# WT20_TV (apply20b): the TV variant, for a WT20 wired to a TV over HDMI and driven with an air
# mouse (dual boot with Windows, apply20a): the TV home screen app, wt20_tv.sh's one-time setup
# and the dark panel while HDMI is connected. True when device/pc/wt20/TV_BUILD exists, so one
# tree builds both systems: touch that file for a TV build, remove it for the tablet's.
WT20_TV ?= $(if $(wildcard device/pc/wt20/TV_BUILD),true,false)

# WT20_PUBLIC (apply23a): the build published for other WT20 owners: no owner Wi-Fi network,
# no ADB over TCP or rooted debugging at boot, no trusted adb key (wt20.mk). Always true in this
# published tree: the private remote-access script (wt20_remote.sh) and adb key are not part of it.
WT20_PUBLIC ?= true

# WT20_ATV (apply22a): set by the Android TV product (device/pc/pc_x86_64_tv) before it includes
# this file. What belongs to the tablet UI goes: our TV launcher app (Google TV has its own home),
# the Game Mode and Perf HUD apps, the Settings overlays (TvSettings is another app), charging
# control (no UI on TV), the cameras.
ifeq ($(WT20_ATV),true)
PRODUCT_CHARACTERISTICS := tv
WT20_TV := false
WT20_GAME_MODE_APP := false
WT20_PERF_HUD := false
WT20_LIVEDISPLAY := false
WT20_MIRACAST := false
WT20_CHARGING_CONTROL := false
TARGET_SUPPORTS_EXTERNAL_CAMERAS := false
endif
