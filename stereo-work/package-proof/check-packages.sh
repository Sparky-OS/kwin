#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
mkdir -p /lintian-input /proof-records
cp /workspace/pending/*.deb /workspace/pending/build-records/* /workspace/pending/source/* /lintian-input/
cd /lintian-input
for changes in kwin_6.7.4-2+stereo3d16_amd64.changes stereo-declare_1.0.1-1_amd64.changes plasma-wayland-protocols_1.21.0-1+stereo3d3_amd64.changes; do
    result=0
    lintian --allow-root "$changes" > "/proof-records/lintian-$changes.log" 2>&1 || result=$?
    printf '%s\n' "$result" > "/proof-records/lintian-$changes.rc"
done
for package in *.deb; do
    printf '%s\n' "$package"
    dpkg-deb -f "$package" Package Version Architecture Maintainer Breaks
done > /proof-records/packages.txt
dpkg-deb -f kwin-wayland_6.7.4-2+stereo3d16_amd64.deb Breaks | grep -F 'libstereo-declare1 (<< 1.0.1-1)'
mkdir -p /source-check
dpkg-source -x kwin_6.7.4-2+stereo3d16.dsc /source-check/kwin > /proof-records/source-unpack.log 2>&1
dpkg-source -x stereo-declare_1.0.1-1.dsc /source-check/stereo-declare >> /proof-records/source-unpack.log 2>&1
dpkg-source -x plasma-wayland-protocols_1.21.0-1+stereo3d3.dsc /source-check/plasma-wayland-protocols >> /proof-records/source-unpack.log 2>&1
cmp /source-check/kwin/autotests/test_stereo_downscale.cpp /workspace/kwin/autotests/test_stereo_downscale.cpp
cmp /source-check/kwin/src/effect/globals.h /workspace/kwin/src/effect/globals.h
cmp /source-check/kwin/src/wayland/stereocontent_v1.cpp /workspace/kwin/src/wayland/stereocontent_v1.cpp
cmp /source-check/stereo-declare/stereo-declare.c /workspace/stereo-declare-1.0.1-src/stereo-declare.c
cmp /source-check/plasma-wayland-protocols/src/protocols/kde-stereo-content-v1.xml /workspace/plasma-wayland-protocols/src/protocols/kde-stereo-content-v1.xml
printf '%s\n' 'PASS: source packages unpack and match the corrected stereo interface' > /proof-records/source-check.rc
