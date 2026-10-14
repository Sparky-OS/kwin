#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare DRM captures with independent wiz3D DX9 formulas, including sRGB.

The oracle uses wiz3D OutputMethods/*/Shaders_dx9.psh and Output_dx9.cpp at
9bcd3da52d52. It never reads a KWin shader or derives expected pixels from a
capture. PNG files marked reference are predictions, not compositor captures.
"""
import json
import math
import pathlib
import sys
from PIL import Image, ImageChops

W, H = 640, 480
MODERN_L = ((.4154, .4710, .1669), (-.0458, -.0484, -.0257), (-.0547, -.0615, .0128))
MODERN_R = ((-.0109, -.0364, -.0060), (.3756, .7333, .0111), (-.0651, -.1287, 1.2971))


def eye(right, x, y):
    return (16 + x % 64 if right else 128 + x % 64,
            32 + y % 32 * 4,
            128 + (x + y) % 64 if right else 16 + (x + y) % 64)


def decode(v):
    return v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4


def encode(v):
    v = max(0, min(1, v))
    return 12.92 * v if v <= .0031308 else 1.055 * v ** (1 / 2.4) - .055


def quantize(v):
    # drm-grab keeps high 8 bits of the AB48 scanout. Apply the same observation
    # to the analytic prediction; this is not an 8-bit nearest-rounding oracle.
    return max(0, min(65535, math.floor(v * 65535 + .5))) >> 8


def pixel(mode, x, y, swap=False):
    if mode.startswith(('rows-', 'columns-', 'checker-')):
        parity = y if mode.startswith('rows') else x if mode.startswith('columns') else x + y
        return eye(bool(parity % 2) ^ mode.endswith('right') ^ swap, x, y)
    if mode.startswith('optimized-'):
        vertical = 'columns' in mode
        parity = x if vertical else y
        right = bool(parity % 2) ^ mode.endswith('right')
        offset = -1 if right else 1
        a = eye(right ^ swap, x, y)
        b = eye(right ^ swap, (x + offset) % W if vertical else x,
                y if vertical else (y + offset) % H)
        return tuple(quantize((v + z) / 510) for v, z in zip(a, b))
    if mode in ('sbs', 'tab'):
        right = x >= W // 2 if mode == 'sbs' else y >= H // 2
        u = x % (W // 2) * 2 if mode == 'sbs' else x
        v = y if mode == 'sbs' else y % (H // 2) * 2
        a = eye(right ^ swap, u, v)
        b = eye(right ^ swap, u + (mode == 'sbs'), v + (mode == 'tab'))
        return tuple(quantize((a[c] + b[c]) / 510) for c in range(3))
    if mode == 'anaglyph-modern':
        l = [decode(v / 255) for v in eye(swap, x, y)]
        r = [decode(v / 255) for v in eye(not swap, x, y)]
        return tuple(quantize(encode(sum(a*v + b*z for a, b, v, z in zip(lm, rm, l, r))))
                     for lm, rm in zip(MODERN_L, MODERN_R))
    if mode.startswith('cf-'):
        l = [decode(v / 255) for v in eye(swap, x, y)]
        r = [decode(v / 255) for v in eye(not swap, x, y)]
        linear = [(a+b)/2 if mode.endswith('back') else b/(a+b) if a+b >= 1e-10 else .5
                  for a, b in zip(l, r)]
        return tuple(quantize(encode(v)) for v in linear)
    if mode.startswith('encoded-cf-'):
        l, r = eye(swap, x, y), eye(not swap, x, y)
        return tuple(quantize((a+b)/510 if mode.endswith('back') else b/(a+b) if a+b else .5)
                     for a, b in zip(l, r))
    right = mode.endswith('second')
    if mode.startswith('pair-mirror'):
        kind = mode.removeprefix('pair-mirror-').removesuffix('-second')
        reflection = {'h': ('', 'h'), 'v': ('', 'v'), 'hv': ('h', 'v'), 'vh': ('v', 'h')}[kind][right]
        if reflection == 'h': x = W - 1 - x
        if reflection == 'v': y = H - 1 - y
    if mode.startswith('pair-'):
        return eye(right ^ swap, x, y)
    raise ValueError(mode)


def expected(mode, swap=False):
    image = Image.new('RGB', (W, H))
    image.putdata([pixel(mode, x, y, swap) for y in range(H) for x in range(W)])
    return image


def difference(actual, prediction):
    if actual.size != prediction.size:
        return {'size_mismatch': [list(actual.size), list(prediction.size)]}
    diff = ImageChops.difference(actual, prediction)
    values = list(diff.getdata())
    return {'pixels': len(values), 'different_pixels': sum(any(v) for v in values),
            'over_one_lsb_pixels': sum(max(v) > 1 for v in values),
            'max_channel_error': max(map(max, values))}


def main():
    folder = pathlib.Path(sys.argv[1])
    results = {}
    methods = ['rows-left', 'rows-right', 'columns-left', 'columns-right', 'checker-left',
               'checker-right', 'sbs', 'tab', 'anaglyph-modern', 'pair-dual', 'pair-dual-second',
               'pair-mirror-h', 'pair-mirror-h-second', 'pair-mirror-v', 'pair-mirror-v-second',
               'pair-mirror-hv', 'pair-mirror-hv-second', 'pair-mirror-vh', 'pair-mirror-vh-second']
    for mode in methods:
        file = folder / (mode + '.ppm')
        if not file.exists():
            results[mode] = {'missing_capture': True}
            continue
        actual = Image.open(file).convert('RGB')
        actual.save(folder / (mode + '.png'))
        prediction = expected(mode)
        prediction.save(folder / (mode + '-reference.png'))
        results[mode] = difference(actual, prediction)
        results[mode]['swapped_reference'] = difference(actual, expected(mode, True))
    for tag, mode in [('pair-ized', 'cf-back'), ('pair-ized-second', 'cf-front')]:
        file = folder / (tag + '.ppm')
        if file.exists():
            actual = Image.open(file).convert('RGB')
            actual.save(folder / (tag + '.png'))
            prediction = expected(mode)
            prediction.save(folder / (tag + '-reference.png'))
            results[tag] = difference(actual, prediction)
            results[tag]['encoded_arithmetic_diagnostic'] = difference(actual, expected('encoded-' + mode))
            results[tag]['swapped_reference'] = difference(actual, expected(mode, True))
        else:
            results[tag] = {'missing_capture': True}
    for mode in ('rows-left', 'rows-right', 'columns-left', 'columns-right'):
        file = folder / (mode + '.ppm')
        if file.exists():
            results['optimized-' + mode] = difference(Image.open(file).convert('RGB'), expected('optimized-' + mode))
    negative = folder / 'swapped-rows.ppm'
    if negative.exists():
        actual = Image.open(negative).convert('RGB')
        actual.save(folder / 'swapped-rows.png')
        results['actual_swapped_input'] = difference(actual, expected('rows-left'))
        results['actual_swapped_input']['swapped_reference'] = difference(actual, expected('rows-left', True))
    inputs = Image.new('RGB', (2*W, H))
    inputs.putdata([eye(x >= W, x % W, y) for y in range(H) for x in range(2*W)])
    inputs.save(folder / 'input-sbs-reference.png')
    (folder / 'comparisons.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
