#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/root XDG_RUNTIME_DIR=/run/user/0 LANG=C.UTF-8 LC_ALL=C.UTF-8
export KWIN_DRM_DEVICES=/dev/dri/card0 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4
export QT_QPA_PLATFORM=wayland
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- sh /tmp/work/rig/guest-session.sh
fi
out=/tmp/work/evidence/vkms
mkdir -p "$out"
mkdir -p /tmp/build
cd /tmp/build
cp /tmp/work/rig/scratch/build/control /tmp/work/rig/scratch/build/fixture /tmp/work/rig/scratch/build/drm-grab .
kwin_wayland --version > "$out/version.txt" 2>&1
kwin_wayland --drm --socket=wl-audit --no-lockscreen --no-global-shortcuts --no-kactivities > "$out/kwin.log" 2>&1 &
kp=$!
i=0
while [ "$i" -lt 90 ] && [ ! -S "$XDG_RUNTIME_DIR/wl-audit" ]; do sleep 1; i=$((i + 1)); done
export WAYLAND_DISPLAY=wl-audit
if [ ! -S "$XDG_RUNTIME_DIR/wl-audit" ]; then echo "AUDIT_NO_SOCKET"; kill "$kp"; exit 1; fi
kscreen-doctor -o > "$out/outputs.txt" 2>&1
./control enable > "$out/enable.txt" 2>&1 || exit 3
./control list > "$out/modes.txt" 2>&1
shoot() {
    tag=$1
    HOLD_SECONDS=180 WAYLAND_DEBUG=client ./fixture > "$out/client-$tag.log" 2>&1 &
    client=$!
    count=0
    while ! grep -q '^fullscreen ' "$out/client-$tag.log" && [ "$count" -lt 60 ]; do
        sleep 1
        count=$((count+1))
    done
    count=0
    while [ "$count" -lt 15 ]; do
        sleep 1
        ./drm-grab /dev/dri/card0 "$out/$tag.ppm" > "$out/grab-$tag.txt" 2>&1
        if grep -Eq '^nonblack [1-9]' "$out/grab-$tag.txt"; then break; fi
        count=$((count+1))
    done
    case "$tag" in pair*) ./drm-grab /dev/dri/card0 "$out/$tag-second.ppm" 1 > "$out/grab-$tag-second.txt" 2>&1 ;; esac
    kill "$client"
    wait "$client"
}
for entry in 'rows-left 0x100' 'anaglyph-modern 0x40'; do
    set -- $entry
    ./control mode "$2" > "$out/config-$1.txt" 2>&1 || continue
    shoot "$1"
done
for entry in 'ized 3 0 0'; do
    set -- $entry
    name=$1
    shift
    ./control pair "$@" > "$out/config-pair-$name.txt" 2>&1 || continue
    shoot "pair-$name"
done
./control mode 0x100 > "$out/config-negative.txt" 2>&1 && SWAP_EYES=1 shoot swapped-rows
kill "$kp"
wait "$kp"
