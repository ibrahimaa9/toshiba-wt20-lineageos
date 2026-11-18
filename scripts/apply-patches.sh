#!/bin/bash
# Applies this repository's changes to a LineageOS for PC (LOS-PC) 23.2 checkout.
# Usage: scripts/apply-patches.sh <path to the LOS-PC tree, e.g. ~/los>
# Run it once on a clean tree synced with manifests/pinned-manifest.xml (see README.md).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
LOS=$(cd "${1:?usage: $0 <LOS-PC tree>}" && pwd)

echo "== device tree and new projects"
mkdir -p "$LOS/device/pc" "$LOS/external"
cp -a "$HERE/device/pc/wt20" "$LOS/device/pc/"
cp -a "$HERE/external/ffmpeg_codec2" "$LOS/external/"
# external/ffmpeg: unpatched FFmpeg 7.1.3 sources plus our Android.bp and configure output
if [ ! -f "$LOS/external/ffmpeg/libavcodec/h264dec.c" ]; then
    echo "   external/ffmpeg: extract https://ffmpeg.org/releases/ffmpeg-7.1.3.tar.xz there first:"
    echo "   mkdir -p $LOS/external/ffmpeg && tar -xJf ffmpeg-7.1.3.tar.xz --strip-components=1 -C $LOS/external/ffmpeg"
    exit 1
fi
cp -a "$HERE/external/ffmpeg/." "$LOS/external/ffmpeg/"

echo "== patches"
while read -r path patch; do
    [ -n "$path" ] || continue
    dir="$LOS/$path"
    [ -d "$dir" ] || { echo "   missing project $path (sync with the pinned manifest)"; exit 1; }
    if [ -s "$HERE/patches/$patch" ]; then
        if git -C "$dir" apply --check "$HERE/patches/$patch" 2>/dev/null; then
            git -C "$dir" apply "$HERE/patches/$patch"
            echo "   applied  $path"
        elif git -C "$dir" apply --reverse --check "$HERE/patches/$patch" 2>/dev/null; then
            echo "   already  $path"
        else
            echo "   FAILED   $path ($patch does not apply: is the tree at the pinned revision?)"; exit 1
        fi
    fi
    new="$HERE/patches/${patch%.patch}.new"
    [ -d "$new" ] && cp -a "$new/." "$dir/" && echo "   new files $path"
done < "$HERE/patches/series"
echo "== done. Next: Google apps (README.md), then scripts/build.sh"
