#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
workspace=/K3D/temp/partners/queue/kwin-wiz3d-outputs
packages=${1:?Pass a package folder relative to the workspace}
case "$packages" in /*|*..*) exit 2 ;; esac
export DOCKER_HOST=unix:///K3D/temp/partners/queue/kwin-wiz3d-outputs/docker.sock
printf '%s\n' kwin-wiz3d-outputs-package-overlay >> "$workspace/.artifacts"
docker run --rm --platform linux/amd64 --name kwin-wiz3d-outputs-package-overlay \
    --user "$(id -u):$(id -g)" --cpus 4 --network none \
    -v "$workspace:$workspace" -w "$workspace" \
    waterfox-stereo-bench:m3-kwin21 sh -eu -c '
    packages=$1
    stage=rig/scratch/package-overlay
    if [ -e "$stage" ]; then rm -r -- "$stage"; fi
    mkdir -p "$stage"
    for name in kwin-wayland kwin-common libkwin6; do
        set -- "$packages"/${name}_*.deb
        [ "$#" = 1 ] && [ -f "$1" ]
        dpkg-deb -x "$1" "$stage"
    done
    tar -C "$stage" --dereference -cf rig/scratch/kwin.tar .
    sha256sum "$stage/usr/bin/kwin_wayland" "$stage/usr/lib/x86_64-linux-gnu/libkwin.so.6.7.4" > rig/package-sha256.txt
    rm -r -- "$stage"
    ' sh "$packages"
