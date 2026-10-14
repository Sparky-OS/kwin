#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Use the bench's HDMI EDID with a small 640x480@120 preferred timing."""
import pathlib
import sys

b = bytearray(pathlib.Path(sys.argv[1]).read_bytes())
# 50.4 MHz, 800x525 total, 640x480 active: 120 Hz. First DTD is preferred.
b[54:72] = bytes.fromhex('b0 13 80 a0 20 e0 2d 10 10 60 a2 00 40 84 63 00 00 18')
b[96] = 144
b[127] = -sum(b[:127]) % 256
pathlib.Path(sys.argv[2]).write_bytes(b)
