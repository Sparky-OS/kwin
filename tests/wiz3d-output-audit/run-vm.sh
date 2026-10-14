#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
workspace=/K3D/temp/partners/queue/kwin-wiz3d-outputs
source=/mnt/build/kwin-wiz3d-outputs/kwin/tests/wiz3d-output-audit
old=/K3D/temp/partners/queue/mesa-stereo/vkms-bench
root=/K3D/temp/partners/queue/displayxr-kwin/rig/root
case_set=$(cat "$workspace/rig/case-set" 2>/dev/null || echo matrix)
case "$case_set" in matrix|ized-first|sequential-fresh|cache-rows-first|cache-ized-first) ;; *) echo "Unknown case set" >&2; exit 2 ;; esac
output=vkms
if [ "$case_set" != matrix ]; then output=$case_set; fi
output=${AUDIT_OUTPUT:-$output}
case "$output" in *..*|/*) echo "Output must be relative to evidence" >&2; exit 2 ;; esac
printf '%s\n' "$output" > "$workspace/rig/output-name"
export DOCKER_HOST=unix:///K3D/temp/partners/queue/kwin-wiz3d-outputs/docker.sock
mkdir -p "$workspace/rig/scratch/overlay/phases" "$workspace/rig/scratch/overlay/data" "$workspace/rig/scratch/overlay/bin" "$workspace/evidence/vkms"
mkdir -p "$workspace/evidence/$output"
printf '%s\n' kwin-wiz3d-outputs-rig >> "$workspace/.artifacts"
bench=/K3D/temp/publish-sso/tools/vkms-stereo-bench
cp "$bench/guest/phases/lib.sh" "$workspace/rig/scratch/overlay/phases/lib.sh"
cp "$bench/edid/stereo-test.bin" "$workspace/rig/scratch/overlay/data/stereo-test.bin"
cp "$old/bin/drm-grab" "$workspace/rig/scratch/overlay/bin/drm-grab"
cp "$source/init" "$workspace/rig/scratch/overlay/init"
cp "$source/guest-session.sh" "$workspace/rig/guest-session.sh"
cp "$source/eye-fixture.c" "$workspace/rig/eye-fixture.c"
cp "$source/drm-grab.c" "$workspace/rig/drm-grab.c"
cp "$source/drm-flipwatch.c" "$workspace/rig/drm-flipwatch.c"
chmod 755 "$workspace/rig/scratch/overlay/init"
docker run --rm --platform linux/amd64 --name kwin-wiz3d-outputs-rig --user "$(id -u):$(id -g)" --cpus 4 --network none \
    -v "$workspace:$workspace" -v "$source:$source:ro" -w "$workspace" \
    waterfox-stereo-bench:m3-kwin21 sh -eu -c '
    python3 "$1/make-control.py" rig/control.c rig/device.xml rig/management.xml
    python3 "$1/make-edid.py" rig/scratch/overlay/data/stereo-test.bin rig/scratch/overlay/data/stereo-test.bin
    python3 "$1/mkcpio.py" rig/scratch/overlay rig/scratch/overlay.cpio.gz
    ' sh "$source"
cat "$old/vm/bench.cpio.gz" "$workspace/rig/scratch/overlay.cpio.gz" > "$workspace/rig/scratch/audit.cpio.gz"
docker run --rm --platform linux/amd64 --name kwin-wiz3d-outputs-rig --user "$(id -u):$(id -g)" --cpus 4 --network none \
    -v "$workspace:$workspace" -w "$workspace/rig" \
    kde-ci-local/suse-qt613:latest sh -eu -c '
    mkdir -p scratch/build
    cp ./*.c scratch/build/
    cd scratch/build
    for name in device management stereo; do
        wayland-scanner client-header ../../$name.xml $name-client.h
        wayland-scanner private-code ../../$name.xml $name-protocol.c
    done
    xdg=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header "$xdg" xdg-shell-client-protocol.h
    wayland-scanner private-code "$xdg" xdg-shell-protocol.c
    viewport=/usr/share/wayland-protocols/stable/viewporter/viewporter.xml
    wayland-scanner client-header "$viewport" viewporter-client.h
    wayland-scanner private-code "$viewport" viewporter-protocol.c
    gcc -O2 -o control control.c device-protocol.c management-protocol.c $(pkg-config --cflags --libs wayland-client)
    gcc -O2 -o fixture eye-fixture.c stereo-protocol.c xdg-shell-protocol.c viewporter-protocol.c $(pkg-config --cflags --libs wayland-client)
    gcc -O2 -o drm-grab drm-grab.c $(pkg-config --cflags libdrm)
    gcc -O2 -o drm-flipwatch drm-flipwatch.c $(pkg-config --cflags libdrm)
    ' > "$workspace/evidence/build-probes.log" 2>&1
docker run --rm --platform linux/amd64 --name kwin-wiz3d-outputs-rig --user "$(id -u):$(id -g)" --cpus 4 --network none \
    -v "$workspace:$workspace" -w "$workspace" -v "$old:$old:ro" -v "$root:$root:ro" \
    -v /usr/bin/qemu-system-x86_64:/opt/qemu-system-x86_64:ro \
    -v /usr/lib/x86_64-linux-gnu:/opt/host-libs:ro \
    -v /usr/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:ro \
    -v /usr/share/qemu:/opt/qemu-data:ro -v /usr/share/seabios:/opt/seabios:ro \
    -e QEMU_MODULE_DIR=/opt/host-libs/qemu debian:testing sh -c '
    ulimit -c 0
    timeout 900 /opt/host-libs/ld-linux-x86-64.so.2 --library-path /opt/host-libs /opt/qemu-system-x86_64 \
        -L /opt/qemu-data -bios /opt/seabios/bios-256k.bin -machine q35 -accel tcg -cpu max -smp 4 -m 3G \
        -display none -vga none -nic none -monitor none -serial stdio -no-reboot \
        -kernel "$1/vm/bzImage-full" -initrd rig/scratch/audit.cpio.gz \
        -append "console=ttyS0 panic=-1 loglevel=4 vkms.create_default_dev=0" \
        -virtfs "local,path=$PWD,mount_tag=work,security_model=none,id=work" \
        -virtfs "local,path=$2,mount_tag=root,security_model=none,id=root,readonly=on" \
        > "evidence/$3/console.log" 2>&1
    ' sh "$old" "$root" "$output"
grep -q 'AUDIT_GUEST_EXIT 0' "$workspace/evidence/$output/console.log"
