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
case_set=$(cat /tmp/work/rig/case-set 2>/dev/null || echo matrix)
if [ "$case_set" != matrix ]; then out=/tmp/work/evidence/$case_set; fi
out=/tmp/work/evidence/$(cat /tmp/work/rig/output-name)
mkdir -p "$out"
mkdir -p /tmp/build
cd /tmp/build
cp /tmp/work/rig/scratch/build/control /tmp/work/rig/scratch/build/fixture /tmp/work/rig/scratch/build/drm-grab .
cp /tmp/work/rig/scratch/build/drm-flipwatch .
kwin_wayland --version > "$out/version.txt" 2>&1
sha256sum /usr/bin/kwin_wayland /usr/lib/x86_64-linux-gnu/libkwin.so.6.7.4 > "$out/runtime-sha256.txt"
kwin_wayland --drm --socket=wl-audit --no-lockscreen --no-global-shortcuts --no-kactivities > "$out/kwin.log" 2>&1 &
kp=$!
i=0
while [ "$i" -lt 90 ] && [ ! -S "$XDG_RUNTIME_DIR/wl-audit" ]; do sleep 1; i=$((i + 1)); done
export WAYLAND_DISPLAY=wl-audit
if [ ! -S "$XDG_RUNTIME_DIR/wl-audit" ]; then echo "AUDIT_NO_SOCKET"; kill "$kp"; exit 1; fi
timeout 30 kscreen-doctor -o > "$out/outputs.txt" 2>&1
echo "$?" > "$out/outputs.exit"
./control enable > "$out/enable.txt" 2>&1 || exit 3
./control list > "$out/modes.txt" 2>&1
shoot() {
    tag=$1
    HOLD_SECONDS=180 WAYLAND_DEBUG=client ./fixture > "$out/client-$tag.log" 2>&1 &
    client=$!
    count=0
    while ! grep -q '^fullscreen ' "$out/client-$tag.log" && [ "$count" -lt 120 ] && kill -0 "$client" 2>/dev/null; do
        sleep 1
        count=$((count+1))
    done
    if ! grep -q '^fullscreen ' "$out/client-$tag.log"; then
        echo "FIXTURE_NOT_READY $tag"
        kill "$client" 2>/dev/null
        wait "$client"
        echo "FIXTURE_EXIT $?" > "$out/failure-$tag.txt"
        ./drm-grab /dev/dri/card0 "$out/unready-$tag.ppm" >> "$out/failure-$tag.txt" 2>&1
        if ! kill -0 "$kp" 2>/dev/null; then
            wait "$kp"
            echo "KWIN_EXIT $?" >> "$out/failure-$tag.txt"
        fi
        dmesg > "$out/kernel.log"
        exit 4
    fi
    count=0
    while [ "$count" -lt 15 ]; do
        sleep 1
        ./drm-grab /dev/dri/card0 "$out/$tag.ppm" > "$out/grab-$tag.txt" 2>&1
        if grep -Eq '^nonblack [1-9]' "$out/grab-$tag.txt"; then break; fi
        count=$((count+1))
    done
    case "$tag" in pair*) ./drm-grab /dev/dri/card0 "$out/$tag-second.ppm" 1 > "$out/grab-$tag-second.txt" 2>&1 ;; esac
    case "$tag" in sequential*)
        ./drm-flipwatch /dev/dri/card0 HDMI-A-1 40 > "$out/flip-$tag.txt" 2>&1
        ./drm-flipwatch /dev/dri/card0 HDMI-A-1 120 100000 > "$out/poll-$tag.txt" 2>&1
        ;;
    esac
    kill "$client"
    wait "$client"
}
case "$case_set" in
    cache-rows-first) sequence="rows ized rows" ;;
    cache-ized-first) sequence="ized rows ized" ;;
    *) sequence="" ;;
esac
if [ -n "$sequence" ]; then
    step=0
    for mode in $sequence; do
        step=$((step+1))
        if [ "$mode" = rows ]; then
            tag=rows-step$step
            ./control mode 0x100 > "$out/config-$tag.txt" 2>&1 || exit 3
        else
            tag=pair-ized-step$step
            ./control pair 3 0 0 > "$out/config-$tag.txt" 2>&1 || exit 3
        fi
        shoot "$tag"
    done
    kill "$kp"
    wait "$kp"
    exit 0
fi
if [ "$case_set" = ized-first ]; then
    ./control pair 3 0 0 > "$out/config-pair-ized.txt" 2>&1 || exit 3
    shoot pair-ized
    kill "$kp"
    wait "$kp"
    exit 0
fi
if [ "$case_set" = sequential-fresh ]; then
    for entry in 'sequential-left 0x40000' 'sequential-right 0x80000'; do
        set -- $entry
        ./control mode "$2" > "$out/config-$1.txt" 2>&1 || exit 3
        shoot "$1"
    done
    kill "$kp"
    wait "$kp"
    exit 0
fi
for entry in 'rows-left 0x100' 'rows-right 0x200' 'columns-left 0x400' 'columns-right 0x800' 'checker-left 0x1000' 'checker-right 0x2000' 'anaglyph-modern 0x40' 'anaglyph-crt 0x80' 'sbs 4' 'tab 8'; do
    set -- $entry
    ./control mode "$2" > "$out/config-$1.txt" 2>&1 || continue
    shoot "$1"
done
for entry in 'dual 1 0 0' 'mirror-h 2 0 1' 'mirror-v 2 0 2' 'mirror-hv 2 1 2' 'mirror-vh 2 2 1' 'ized 3 0 0'; do
    set -- $entry
    name=$1
    shift
    ./control pair "$@" > "$out/config-pair-$name.txt" 2>&1 || continue
    shoot "pair-$name"
done
./control mode 0x100 > "$out/config-negative.txt" 2>&1 && SWAP_EYES=1 shoot swapped-rows
for entry in 'sequential-left 0x40000' 'sequential-right 0x80000'; do
    set -- $entry
    ./control mode "$2" > "$out/config-$1.txt" 2>&1 || continue
    shoot "$1"
done
kill "$kp"
wait "$kp"
exit 0
