#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
export DEBIAN_FRONTEND=noninteractive
mkdir -p /proof-records
exec > /proof-records/install.log 2>&1
printf '%s\n' 'deb [trusted=yes] file:/repo ./' > /etc/apt/sources.list.d/stereo.list
printf '%s\n' 'Package: *' 'Pin: release o=Sparky Stereo OS' 'Pin-Priority: 1001' > /etc/apt/preferences.d/stereo
apt-get -y update
apt-get -y --no-install-recommends install \
    kwin-wayland=4:6.7.4-2+stereo3d15 libstereo-declare1=1.0.0-1 \
    haruna dbus-x11 dbus xvfb xauth x11-utils mesa-utils vulkan-tools \
    libgl1-mesa-dri libegl-mesa0 libglx-mesa0 libgbm1 mesa-vulkan-drivers \
    mesa-vulkan-layer-stereo qml6-module-org-kde-desktop breeze-icon-theme \
    fonts-dejavu-core python3-numpy python3-pil apt-utils dpkg-dev lintian
dpkg-query -W kwin-wayland libstereo-declare1 haruna libmpvqt3 libmpv2 \
    libgl1-mesa-dri libegl-mesa0 mesa-vulkan-layer-stereo > /proof-records/before.tsv
test "$(dpkg-query -W -f='${Version}' kwin-wayland)" = 4:6.7.4-2+stereo3d15
test "$(dpkg-query -W -f='${Version}' libstereo-declare1)" = 1.0.0-1
apt-cache policy kwin-wayland libstereo-declare1 > /proof-records/before-policy.txt
printf '%s\n' 'PASS: repository KWin +15 and old helper installed' > /proof-records/base.rc
