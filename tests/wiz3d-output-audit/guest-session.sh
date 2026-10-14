#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/root LANG=C.UTF-8 LC_ALL=C.UTF-8
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
export KWIN_DRM_DEVICES=/dev/dri/card0 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4
export QT_QPA_PLATFORM=wayland
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
case_set=$(cat /tmp/work/rig/case-set 2>/dev/null || echo matrix)
if [ "$case_set" = swap-matrix ] && [ -z "${AUDIT_CASE:-}" ]; then
    probes=$(cat /tmp/work/rig/swap-probes 2>/dev/null || echo 'rows-left rows-right columns-left columns-right checker-left checker-right anaglyph-modern anaglyph-crt sbs tab sbs-full frame-packing 2d pair-dual pair-mirror-h pair-mirror-v pair-ized')
    for probe in $probes; do
        export AUDIT_CASE=$probe
        export XDG_CONFIG_HOME=/tmp/config-$probe XDG_CACHE_HOME=/tmp/cache-$probe
        export XDG_RUNTIME_DIR=/run/user/0/$probe
        mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
        chmod 700 "$XDG_RUNTIME_DIR"
        dbus-run-session -- sh /tmp/work/rig/guest-session.sh || exit "$?"
    done
    exit 0
fi
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- sh /tmp/work/rig/guest-session.sh
fi
out=/tmp/work/evidence/vkms
case_set=$(cat /tmp/work/rig/case-set 2>/dev/null || echo matrix)
if [ "$case_set" != matrix ]; then out=/tmp/work/evidence/$case_set; fi
out=/tmp/work/evidence/$(cat /tmp/work/rig/output-name)
if [ -n "${AUDIT_CASE:-}" ]; then out=$out/$AUDIT_CASE; fi
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
if [ "$case_set" = swap-matrix ] || [ "$case_set" = swap-settings ]; then
    echo "Output state is recorded by the versioned control client." > "$out/outputs.txt"
    echo "not-run" > "$out/outputs.exit"
else
    timeout 30 kscreen-doctor -o > "$out/outputs.txt" 2>&1
    echo "$?" > "$out/outputs.exit"
fi
./control enable > "$out/enable.txt" 2>&1 || exit 3
./control list > "$out/modes.txt" 2>&1
if [ "$case_set" = swap-settings ]; then
    LD_LIBRARY_PATH=/tmp/work/rig/ui/lib QT_PLUGIN_PATH=/tmp/work/rig/ui/plugins KSCREEN_BACKEND_INPROCESS=1 KSCREEN_BACKEND=KWayland \
        /tmp/work/rig/ui/swap-settings > "$out/settings-roundtrip.log" 2>&1
    result=$?
    echo "$result" > "$out/settings-roundtrip.exit"
    kill "$kp"
    wait "$kp"
    exit "$result"
fi
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
if [ "$case_set" = swap-matrix ]; then
    export PALETTE_FIXTURE=1
    case "$AUDIT_CASE" in
        rows-left) flag=0x100 ;; rows-right) flag=0x200 ;;
        columns-left) flag=0x400 ;; columns-right) flag=0x800 ;;
        checker-left) flag=0x1000 ;; checker-right) flag=0x2000 ;;
        anaglyph-modern) flag=0x40 ;; anaglyph-crt) flag=0x80 ;;
        sbs) flag=4 ;; tab) flag=8 ;; sbs-full) flag=0x20 ;;
        frame-packing) flag=0x10 ;; 2d) flag=0 ;;
        pair-dual) set -- 1 0 0 ;; pair-mirror-h) set -- 2 0 1 ;;
        pair-mirror-v) set -- 2 0 2 ;; pair-ized) set -- 3 0 0 ;;
        *) exit 2 ;;
    esac
    case "$AUDIT_CASE" in
        pair*) ./control pair "$@" > "$out/config.txt" 2>&1 || exit 3 ;;
        sbs-full) ./control mode "$flag" 1920 1080 > "$out/config.txt" 2>&1 || exit 3 ;;
        frame-packing) ./control mode "$flag" 1280 720 > "$out/config.txt" 2>&1 || exit 3 ;;
        *) ./control mode "$flag" > "$out/config.txt" 2>&1 || exit 3 ;;
    esac
    ./control swap 0 0 > "$out/swap-off.txt" 2>&1 || exit 3
    shoot "$AUDIT_CASE-off"
    ./control swap 0 1 > "$out/swap-first.txt" 2>&1 || exit 3
    shoot "$AUDIT_CASE-first"
    case "$AUDIT_CASE" in
        pair*)
            ./control swap 1 1 > "$out/swap-both.txt" 2>&1 || exit 3
            shoot "$AUDIT_CASE-both"
            ;;
    esac
    kill "$kp"
    wait "$kp"
    exit 0
fi
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
