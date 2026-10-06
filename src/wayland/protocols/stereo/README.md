<!--
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
    SPDX-License-Identifier: CC0-1.0
-->

Copies of the three Plasma protocols that carry the stereo 3D additions (kde-output-device-v2 and
kde-output-management-v2 with the stereo mode flags and virtual stereo, and kde-stereo-content-v1),
as they are in the stereo3d branch of plasma-wayland-protocols.

KWin takes them from here only while PlasmaWaylandProtocols, as installed, does not have the
additions (see KWIN_STEREO_PROTOCOLS_DIR in the top level CMakeLists.txt). Once it has them this
folder is not used and can go.
