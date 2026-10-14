#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
workspace=/K3D/temp/partners/queue/kwin-wiz3d-outputs
checkout=/mnt/build/kwin-wiz3d-outputs/kwin
export DOCKER_HOST=unix:///K3D/temp/partners/queue/kwin-wiz3d-outputs/docker.sock
mkdir -p "$workspace/rig/scratch" "$workspace/evidence"
protocols=$checkout/src/wayland/protocols/stereo
cp "$protocols/kde-output-device-v2.xml" "$workspace/rig/device.xml"
cp "$protocols/kde-output-management-v2.xml" "$workspace/rig/management.xml"
cp "$protocols/kde-stereo-content-v1.xml" "$workspace/rig/stereo.xml"
docker run --rm --user "$(id -u):$(id -g)" --cpus 4 --network none \
    -v "$workspace:$workspace" -w "$workspace" waterfox-stereo-bench:m3-kwin21 sh -eu -c '
    dpkg-query -W kwin-wayland kwin-common libkwin6 > evidence/kwin-image-packages.txt
    dpkg-query -L kwin-wayland kwin-common libkwin6 | while IFS= read -r file; do
        if [ -f "$file" ]; then printf "%s\n" "${file#/}"; fi
    done > rig/scratch/package-files.txt
    tar -C / --dereference -cf rig/scratch/kwin21.tar -T rig/scratch/package-files.txt
    sha256sum /usr/bin/kwin_wayland /usr/lib/x86_64-linux-gnu/libkwin.so.6.7.4 > evidence/package-sha256.txt
    '
