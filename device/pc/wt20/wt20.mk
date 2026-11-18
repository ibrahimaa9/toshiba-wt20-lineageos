#
# Toshiba Portege WT20-B additions on top of pc_x86_64 (build options: options.mk).
# Userdebug only: the remote access service runs in the su domain.
#

# Sensor features the hub provides, on top of tablet_core_hardware.xml's accelerometer and
# compass (pc_desktop.xml no longer marks them unavailable)
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml

# Camera features: the external camera HAL reports the webcams as back and front (config in
# device/mainline/common/optional/external-camera-provider-hal_default-aidl)
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.camera.autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml

# Remote access for a tablet with no keyboard and a host-only USB port (no ADB over a cable):
# joins the owner's Wi-Fi and opens ADB over the network once the system is up. The public build
# (WT20_PUBLIC) installs wt20_public.sh there instead: the boot tweaks only.
PRODUCT_COPY_FILES += \
    device/pc/wt20/idc/Vendor_03eb_Product_8a55.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Vendor_03eb_Product_8a55.idc \
    device/pc/wt20/init.wt20.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/init.wt20.rc \
    device/pc/wt20/$(if $(filter true,$(WT20_PUBLIC)),wt20_public.sh,wt20_remote.sh):$(TARGET_COPY_OUT_SYSTEM)/bin/wt20_remote.sh \
    device/pc/wt20/wt20_health.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/wt20_health.sh \
    device/pc/wt20/wt20_acwake.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/wt20_acwake.sh

# Adaptive brightness from the ambient light sensor (framework default: unavailable)
PRODUCT_PACKAGE_OVERLAYS += device/pc/wt20/overlay

# The owner's PC may use adb without the on-screen authorization prompt
ifneq ($(WT20_PUBLIC),true)
PRODUCT_ADB_KEYS := device/pc/wt20/adbkey.pub
endif

# 4 GB of RAM: zram on by default (GRUB's ZRAM setting still overrides it, init.virt.rc)
PRODUCT_SYSTEM_PROPERTIES += \
    persist.sys.zram_enabled=1

# The sensor hub (ST 0483:91d1 over HID) reports the accelerometer with all three axes
# inverted relative to Android's frame: flat face-up read -1 g on z and auto-rotation came out
# upside down. sensors.iio applies these per-axis factors (measured 2026-11-08: upright with
# the camera on top then reads +9.8 on y).
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.iio.accel.x.opt_scale=-1 \
    ro.vendor.iio.accel.y.opt_scale=-1 \
    ro.vendor.iio.accel.z.opt_scale=-1

# The hub's fastest report interval is 16 ms for the accelerometer and the gyroscope (a 1 ms
# or 5 ms request reads back 62.5 Hz; measured 2026-11-09). It does not list its supported
# rates, so sensors.iio advertises this one (minDelay 16000 us) instead of 0. The
# magnetometer has no cap yet: FASTEST asks the hub for its own fastest interval. Add
# ro.vendor.iio.magn.max_freq once that rate is measured.
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.iio.accel.max_freq=62.5 \
    ro.vendor.iio.anglvel.max_freq=62.5

# No window blur (notification shade, recents): the owner prefers smooth animation on the
# 4.5 W HD 5300 to the effect. libinit_virt respects this explicit 0.
PRODUCT_VENDOR_PROPERTIES += \
    ro.surface_flinger.supports_background_blur=0

# Internal microphone (Conexant CX20753 pin 0x1a): +24 dB boost puts speech at ~-27 dB mean,
# no boost left it at ~-72 dB (measured on recordings made on the tablet)
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.audio.internal_mic_boost=2

# Hardware planes on Broadwell (gpu_workaround=all turns them off; this read-only property
# from build.prop wins over init.pc.rc's later setprop): drm_hwcomposer now respects their
# fixed stacking and their inability to blend (see external/drm_hwcomposer-upstream), so a
# fullscreen opaque layer (games, video) scans out with no GPU composition. The sprite
# cannot scale, so scaled layers stay on the GPU instead of failing the test commit.
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.hwc.drm.disable_planes=0 \
    vendor.hwc.drm.scale_with_gpu=1

# ARM apps through Google's ARM64-to-x86_64 translator (options.mk: WT20_ARM_TRANSLATION)
ifeq ($(WT20_ARM_TRANSLATION),true)
$(call inherit-product, vendor/google/ndk_translation/ndk_translation.mk)
endif

# Games: Mesa iris provides the OpenGL ES 3.2 extension pack, and the kernel has the xpad /
# PlayStation / Nintendo / Steam controller drivers
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml \
    frameworks/native/data/etc/android.hardware.gamepad.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.gamepad.xml

# Magnetometer hard-iron offset of the WT20's own magnets (uT, sensor frame, plus the local
# field strength): the compass starts calibrated; a figure-8 refines it (sensors-iio, apply16e)
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.iio.magn.hard_iron=-29.5,-72.5,99,44

# Per-game frame rate caps (Game Mode); the panel has a single 60 Hz mode
PRODUCT_VENDOR_PROPERTIES += \
    ro.surface_flinger.enable_frame_rate_override=true

# No modem sets the Wi-Fi regulatory domain: without it 5 GHz stays passive with world rules
PRODUCT_VENDOR_PROPERTIES += \
    ro.boot.wificountrycode=IQ

# lmkd for 3.7 GB with 75% zram: kill on real thrashing / exhausted swap, not on a busy zram
PRODUCT_VENDOR_PROPERTIES += \
    ro.lmk.swap_free_low_percentage=10 \
    ro.lmk.thrashing_limit=30 \
    ro.lmk.thrashing_limit_decay=50 \
    ro.lmk.psi_partial_stall_ms=70 \
    ro.lmk.swap_util_max=90

# Time zone from location (no modem): the GeoTZ offline provider
PRODUCT_PACKAGES += \
    com.android.geotz

# VA-API on the HD 5300 (options.mk: WT20_VAAPI_DRIVER); test: LIBVA_DRIVER_NAME=i965 vainfo
ifeq ($(WT20_VAAPI_DRIVER),true)
PRODUCT_PACKAGES += \
    i965_drv_video \
    libva \
    libva-android \
    vainfo
endif

# iHD next to i965 (options.mk: WT20_IHD_DRIVER); the HAL picks it with
# persist.vendor.wt20.vadriver=iHD; test: LIBVA_DRIVER_NAME=iHD vainfo
ifeq ($(WT20_IHD_DRIVER),true)
ifneq ($(WT20_VAAPI_DRIVER),true)
$(error WT20_IHD_DRIVER needs WT20_VAAPI_DRIVER := true (libva))
endif
PRODUCT_PACKAGES += \
    iHD_drv_video \
    libigdgmm_android
# iHD by default (apply22b); setprop persist.vendor.wt20.vadriver i965 goes back
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.wt20.vadriver=iHD
endif

# Game Mode app: per-game resolution / FPS cap / game mode (options.mk: WT20_GAME_MODE_APP)
ifeq ($(WT20_GAME_MODE_APP),true)
PRODUCT_PACKAGES += \
    WT20GameMode
endif

# Charging control (options.mk: WT20_CHARGING_CONTROL). toshiba_acpi's
# BAT1/charge_control_end_threshold is the EC's two-state charge mode: below 90 = stop at ~80%,
# 90-100 = charge to full. Lineage's health HAL in toggle mode writes 100 to charge and 80 to
# stop; no bypass (the EC has none) and no limit/deadline nodes.
ifeq ($(WT20_CHARGING_CONTROL),true)
PRODUCT_PACKAGES += \
    vendor.lineage.health-service.default
$(call soong_config_set,lineage_health,charging_control_charging_path,/sys/class/power_supply/BAT1/charge_control_end_threshold)
$(call soong_config_set,lineage_health,charging_control_charging_enabled,100)
$(call soong_config_set,lineage_health,charging_control_charging_disabled,80)
$(call soong_config_set_bool,lineage_health,charging_control_supports_toggle,true)
$(call soong_config_set_bool,lineage_health,charging_control_supports_bypass,false)
$(call soong_config_set_bool,lineage_health,charging_control_supports_deadline,false)
$(call soong_config_set_bool,lineage_health,charging_control_supports_limit,false)
PRODUCT_PACKAGE_OVERLAYS += device/pc/wt20/charging/overlay
PRODUCT_COPY_FILES += \
    device/pc/wt20/charging/init.wt20.charging.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.wt20.charging.rc
endif

# Power HAL hints (options.mk: WT20_POWER_HAL): vendor_init applies the GPU limits and the
# Toshiba cooling method the perfmgr HAL requests through vendor.powerhal.wt20.* properties.
# A touch or fling boost lasts at most 3 s (HAL default 5.65 s): heat on a fanless 4.5 W chip.
ifeq ($(WT20_POWER_HAL),true)
PRODUCT_COPY_FILES += \
    device/pc/wt20/power/init.wt20.power.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.wt20.power.rc
PRODUCT_VENDOR_PROPERTIES += \
    vendor.powerhal.interaction.max=3000
endif

# Thermal HAL (options.mk: WT20_THERMAL_HAL; SELinux: pc_x86_64/BoardConfig.mk)
ifeq ($(WT20_THERMAL_HAL),true)
PRODUCT_PACKAGES += \
    android.hardware.thermal-service.wt20
endif

# Hotspot next to the station (options.mk: WT20_WIFI_STA_AP): the vendor Wi-Fi HAL, and wlan1
# created on the iwlwifi phy before Wi-Fi starts (the HAL cannot create interfaces itself)
ifeq ($(WT20_WIFI_STA_AP),true)
PRODUCT_PACKAGES += \
    android.hardware.wifi-service
PRODUCT_COPY_FILES += \
    device/pc/wt20/init.wt20.wifi_ap.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/init.wt20.wifi_ap.rc \
    device/pc/wt20/wt20_wifi_ap.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/wt20_wifi_ap.sh
endif

# Wi-Fi Direct (Miracast; the B6 HAL derives the P2P name from it too): the supplicant's P2P
# device on wlan0 instead of the default "p2p0", which iwlwifi does not have
ifneq ($(filter true,$(WT20_WIFI_STA_AP) $(WT20_MIRACAST)),)
PRODUCT_VENDOR_PROPERTIES += \
    wifi.direct.interface=p2p-dev-wlan0
endif

# Miracast source (options.mk: WT20_MIRACAST): config_enableWifiDisplay
ifeq ($(WT20_MIRACAST),true)
PRODUCT_PACKAGE_OVERLAYS += device/pc/wt20/overlay-miracast
endif

# LiveDisplay picture adjustment in software (options.mk: WT20_LIVEDISPLAY)
ifeq ($(WT20_LIVEDISPLAY),true)
PRODUCT_PACKAGE_OVERLAYS += device/pc/wt20/overlay-livedisplay
endif

# Sound over HDMI / DisplayPort (options.mk: WT20_HDMI_AUDIO): the audio HAL accepts HDMI devices
ifeq ($(WT20_HDMI_AUDIO),true)
$(call soong_config_set_bool,wt20_audio,hdmi_output,true)
endif

# WT20_TOUCH_HAL (apply15v, options.mk): high touch sensitivity for the Atmel mXT1664T
ifeq ($(WT20_TOUCH_HAL),true)
PRODUCT_PACKAGES += \
    vendor.lineage.touch-service.wt20
endif

# Hardware video decode (options.mk: WT20_HW_VIDEO_DECODE, apply17c): the FFmpeg VA-API Codec2
# HAL as IComponentStore/default (mediacodec domain; SELinux: pc_x86_64/BoardConfig.mk), its
# codec list (rank 128, before c2.android's 512), the kill switch, and the self-test with its
# vectors: adb shell wt20_c2_vaapi_selftest (or tools/verify17.sh)
ifeq ($(WT20_HW_VIDEO_DECODE),true)
ifneq ($(WT20_VAAPI_DRIVER),true)
$(error WT20_HW_VIDEO_DECODE needs WT20_VAAPI_DRIVER := true (i965_drv_video and libva))
endif
PRODUCT_SOONG_NAMESPACES += \
    external/ffmpeg \
    external/ffmpeg_codec2 \
    device/pc/wt20/media/selftest
PRODUCT_PACKAGES += \
    android.hardware.media.c2-service-ffmpeg \
    android.hardware.media.c2-ffmpeg-default-seccomp_policy \
    android.hardware.media.c2-ffmpeg-extended-seccomp_policy \
    android.hardware.media.c2-service-ffmpeg.wt20.xml \
    wt20_c2_vaapi_selftest
PRODUCT_COPY_FILES += \
    device/pc/wt20/media/android.hardware.media.c2-service-ffmpeg.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/android.hardware.media.c2-service-ffmpeg.rc \
    device/pc/wt20/media/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    system/cros-codecs/src/codec/h264/test_data/test-25fps.h264:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/test-25fps.h264 \
    system/cros-codecs/src/codec/h264/test_data/test-25fps.h264.md5:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/test-25fps.h264.md5 \
    system/cros-codecs/src/codec/h264/test_data/test-25fps-interlaced.h264:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/test-25fps-interlaced.h264 \
    system/cros-codecs/src/codec/h264/test_data/test-25fps-interlaced.h264.md5:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/test-25fps-interlaced.h264.md5 \
    system/cros-codecs/src/codec/vp8/test_data/test-25fps.vp8:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/test-25fps.vp8 \
    system/cros-codecs/src/codec/vp8/test_data/test-25fps.vp8.md5:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/test-25fps.vp8.md5 \
    hardware/interfaces/media/res/bbb_mpeg2_352x288_1mbps_60fps.m2v:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/bbb_mpeg2_352x288_1mbps_60fps.m2v \
    hardware/interfaces/media/res/bbb_avc_176x144_300kbps_60fps.h264:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/bbb_avc_176x144_300kbps_60fps.h264 \
    hardware/interfaces/media/res/bbb_mpeg2_176x144_105kbps_25fps.m2v:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/bbb_mpeg2_176x144_105kbps_25fps.m2v \
    device/pc/wt20/media/vectors/wt20_crop_168x136.h264:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/wt20_crop_168x136.h264 \
    device/pc/wt20/media/vectors/wt20_crop_168x136.h264.md5:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/wt20_crop_168x136.h264.md5 \
    device/pc/wt20/media/vectors/wt20_baseline_176x144.h264:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/wt20_baseline_176x144.h264 \
    device/pc/wt20/media/vectors/wt20_baseline_176x144.h264.md5:$(TARGET_COPY_OUT_VENDOR)/etc/wt20_hwdec/wt20_baseline_176x144.h264.md5
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.wt20.hwdec=true \
    persist.vendor.wt20.hwdec.swfallback=true
endif

# WT20 HUD: always-on-top performance overlay (options.mk: WT20_PERF_HUD; SELinux:
# pc_x86_64/BoardConfig.mk)
ifeq ($(WT20_PERF_HUD),true)
PRODUCT_PACKAGES += \
    WT20PerfHud
endif

# TV variant (options.mk: WT20_TV): the TV home screen, and the setup / panel script that
# init.wt20.rc starts when ro.wt20.tv=1
ifeq ($(WT20_TV),true)
PRODUCT_PACKAGES += \
    WT20TvLauncher
PRODUCT_COPY_FILES += \
    device/pc/wt20/wt20_tv.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/wt20_tv.sh
# the TV is the screen: an external display comes on by itself, without the "mirror
# display?" dialog (it would show on the tablet's panel, which is dark while HDMI is in)
PRODUCT_SYSTEM_PROPERTIES += \
    ro.wt20.tv=1 \
    persist.sys.display.enable_on_connect.external=true
endif

# Android TV product (apply22a, WT20_ATV): Android TV's density at 1080p (init.virt.rc's
# setprop of ro.sf.lcd_density then does nothing), the external display on without the "mirror
# display?" dialog, and the tablet's panel dark while HDMI is in (wt20_atv.sh, started as the
# wt20_tv service by init.wt20.rc when ro.wt20.tv=1)
ifeq ($(WT20_ATV),true)
PRODUCT_COPY_FILES += \
    device/pc/wt20/wt20_atv.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/wt20_tv.sh \
    device/pc/wt20/privapp-permissions-wt20-tv.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/privapp-permissions-wt20-tv.xml
PRODUCT_SYSTEM_PROPERTIES += \
    ro.wt20.tv=1 \
    persist.sys.display.enable_on_connect.external=true
PRODUCT_VENDOR_PROPERTIES += \
    ro.sf.lcd_density=320
PRODUCT_PACKAGES += \
    WT20TvFrameworkFixOverlay \
    WT20TvSettingsFixOverlay
# apply22f: as on Google's Google TV emulator image (no SetupWraith there either)
PRODUCT_PRODUCT_PROPERTIES += \
    ro.setupwizard.mode=DISABLED
endif
