#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
export DEBIAN_FRONTEND=noninteractive
exec > /proof-records/upgrade.log 2>&1
mkdir -p /newpackages
cp /workspace/pending/*.deb /newpackages/
cd /newpackages
dpkg-scanpackages . /dev/null > Packages
gzip -c Packages > Packages.gz
apt-ftparchive -o APT::FTPArchive::Release::Origin='Mesa stereo package proof' release . > Release
printf '%s\n' 'deb [trusted=yes] file:/newpackages ./' > /etc/apt/sources.list.d/proof.list
printf '%s\n' 'Package: *' 'Pin: release o=Mesa stereo package proof' 'Pin-Priority: 1002' > /etc/apt/preferences.d/proof
apt-get -y update
apt-cache policy kwin-wayland libstereo-declare1 plasma-wayland-protocols > /proof-records/upgrade-policy.txt
apt-get -s install kwin-wayland=4:6.7.4-2+stereo3d16 plasma-wayland-protocols=1.21.0-1+stereo3d3 > /proof-records/upgrade-plan.txt
grep -E '^Inst libstereo-declare1 .*1\.0\.1-1' /proof-records/upgrade-plan.txt
apt-get -y install kwin-wayland=4:6.7.4-2+stereo3d16 plasma-wayland-protocols=1.21.0-1+stereo3d3
dpkg-query -W kwin-wayland libstereo-declare1 plasma-wayland-protocols \
    haruna libmpvqt3 libmpv2 libgl1-mesa-dri libegl-mesa0 mesa-vulkan-layer-stereo > /proof-records/after.tsv
test "$(dpkg-query -W -f='${Version}' kwin-wayland)" = 4:6.7.4-2+stereo3d16
test "$(dpkg-query -W -f='${Version}' libstereo-declare1)" = 1.0.1-1
test "$(dpkg-query -W -f='${Version}' plasma-wayland-protocols)" = 1.21.0-1+stereo3d3
printf '%s\n' 'PASS: upgrading KWin pulls the new helper' > /proof-records/upgrade.rc
