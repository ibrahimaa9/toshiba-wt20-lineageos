#!/bin/bash
# Runs as the build user INSIDE the chroot: builds LineageOS for pc_x86_64 (installer image + OTA zip).
# Usage: build.sh [make targets...]      JOBS=N overrides the parallelism (default 8).
cd ~/los || exit 1
export LC_ALL=C
export USE_CCACHE=1 CCACHE_EXEC=/usr/bin/ccache CCACHE_DIR=$HOME/.ccache
ccache -M 50G >/dev/null

echo "=== build start: $(date)"
source build/envsetup.sh
breakfast pc_x86_64 userdebug || { echo "BREAKFAST FAILED"; exit 1; }
m -j"${JOBS:-8}" "$@"
rc=$?
echo "=== build end: $(date), exit=$rc"
[ $rc -eq 0 ] && ls -la out/target/product/pc_x86_64/*.img out/target/product/pc_x86_64/*.zip 2>/dev/null
exit $rc
