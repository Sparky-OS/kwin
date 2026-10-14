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
import re
import sys
from PIL import Image, ImageChops

W, H = 640, 480
PALETTE = False
PACKING_GAP = 30
MODERN_L = ((.4154, .4710, .1669), (-.0458, -.0484, -.0257), (-.0547, -.0615, .0128))
MODERN_R = ((-.0109, -.0364, -.0060), (.3756, .7333, .0111), (-.0651, -.1287, 1.2971))

CRT_L = ((.456, .500, .176), (-.040, -.038, -.016), (-.015, -.021, -.005))
CRT_R = ((-.043, -.088, -.002), (.378, .734, -.018), (-.072, -.113, 1.226))


def eye(right, x, y):
    if PALETTE:
        return ((16 if right else 160) + 16 * (x % 2),
                (224 if right else 32) + 16 * (y % 2),
                (128 if right else 16) + 16 * ((x + y) % 2))
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
    if mode == '2d':
        return eye(False, x, y)
    if mode == 'sbs-full':
        return eye((x >= W) ^ swap, x % W, y)
    if mode == 'frame-packing':
        if H <= y < H + PACKING_GAP:
            return (0, 0, 0)
        right = y >= H + PACKING_GAP
        return eye(right ^ swap, x, y - (H + PACKING_GAP) if right else y)
    if mode.startswith('anaglyph-'):
        dec = (lambda v: v ** 2.2) if mode.endswith('gamma22') else decode
        enc = (lambda v: max(0, min(1, v)) ** (1/2.2)) if mode.endswith('gamma22') else encode
        l = [dec(v / 255) for v in eye(swap, x, y)]
        r = [dec(v / 255) for v in eye(not swap, x, y)]
        return tuple(quantize(enc(sum(a*v + b*z for a, b, v, z in zip(lm, rm, l, r))))
                     for lm, rm in zip(CRT_L if 'crt' in mode else MODERN_L,
                                       CRT_R if 'crt' in mode else MODERN_R))
    if mode.startswith('cf-'):
        l = [decode(v / 255) for v in eye(swap, x, y)]
        r = [decode(v / 255) for v in eye(not swap, x, y)]
        linear = [(a+b)/2 if mode.endswith('back') else b/(a+b) if a+b >= 1e-10 else .5
                  for a, b in zip(l, r)]
        # DX9 multipass binds each panel to RT0; MRT only sRGB-encodes RT0.
        enc = (lambda v: v) if mode == 'cf-mrt-front' else encode
        return tuple(quantize(enc(v)) for v in linear)
    if mode.startswith('encoded-cf-'):
        l, r = eye(swap, x, y), eye(not swap, x, y)
        return tuple(quantize((a+b)/510 if mode.endswith('back') else b/(a+b) if a+b else .5)
                     for a, b in zip(l, r))
    if mode.startswith('copy-'):
        return eye(mode.endswith('right') ^ swap, x, y)
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
    width = 2 * W if mode == 'sbs-full' else W
    height = 2 * H + PACKING_GAP if mode == 'frame-packing' else H
    image = Image.new('RGB', (width, height))
    image.putdata([pixel(mode, x, y, swap) for y in range(height) for x in range(width)])
    return image


def difference(actual, prediction):
    if actual.size != prediction.size:
        return {'size_mismatch': [list(actual.size), list(prediction.size)]}
    diff = ImageChops.difference(actual, prediction)
    values = list(diff.getdata())
    return {'pixels': len(values), 'different_pixels': sum(any(v) for v in values),
            'over_one_lsb_pixels': sum(max(v) > 1 for v in values),
            'max_channel_error': max(map(max, values))}


def capture_path(folder, tag):
    ppm = folder / (tag + '.ppm')
    return ppm if ppm.exists() else folder / (tag + '-framebuffer.png')


def observe(folder, tag):
    raw = Image.open(capture_path(folder, tag)).convert('RGB')
    props = {}
    for line in (folder / ('grab-' + tag + '.txt')).read_text().splitlines():
        if line.startswith('property '):
            _, name, value = line.split()
            props[name] = int(value)
    x, y, w, h = (props['SRC_' + key] >> 16 for key in ('X', 'Y', 'W', 'H'))
    assert props['CRTC_W'] == w and props['CRTC_H'] == h, 'scaling needs a separate readback path'
    raw.save(folder / (tag + '-framebuffer.png'))
    image = raw.crop((x, y, x+w, y+h))
    rotation = props.get('rotation', 1)
    if rotation & 16: image = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    if rotation & 32: image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    for bit, transform in ((2, Image.Transpose.ROTATE_90), (4, Image.Transpose.ROTATE_180), (8, Image.Transpose.ROTATE_270)):
        if rotation & bit: image = image.transpose(transform)
    image.save(folder / (tag + '.png'))
    return image


def main():
    folder = pathlib.Path(sys.argv[1])
    results = {}
    for file in folder.glob('*.ppm'):
        Image.open(file).save(file.with_name(file.stem + '-framebuffer.png'))
    methods = ['rows-left', 'rows-right', 'columns-left', 'columns-right', 'checker-left',
               'checker-right', 'sbs', 'tab', 'anaglyph-modern', 'pair-dual', 'pair-dual-second',
               'pair-mirror-h', 'pair-mirror-h-second', 'pair-mirror-v', 'pair-mirror-v-second',
               'pair-mirror-hv', 'pair-mirror-hv-second', 'pair-mirror-vh', 'pair-mirror-vh-second']
    for mode in methods:
        file = capture_path(folder, mode)
        if not file.exists():
            results[mode] = {'missing_capture': True}
            continue
        actual = observe(folder, mode)
        prediction = expected(mode)
        prediction.save(folder / (mode + '-reference.png'))
        results[mode] = difference(actual, prediction)
        results[mode]['swapped_reference'] = difference(actual, expected(mode, True))
        if mode == 'anaglyph-modern':
            results[mode]['gamma22_diagnostic'] = difference(actual, expected('anaglyph-gamma22'))
    for tag, mode in [('pair-ized', 'cf-back'), ('pair-ized-second', 'cf-front')]:
        file = capture_path(folder, tag)
        if file.exists():
            actual = observe(folder, tag)
            prediction = expected(mode)
            prediction.save(folder / (tag + '-reference.png'))
            results[tag] = difference(actual, prediction)
            results[tag]['encoded_arithmetic_diagnostic'] = difference(actual, expected('encoded-' + mode))
            results[tag]['encoded_arithmetic_swapped'] = difference(actual, expected('encoded-' + mode, True))
            if mode == 'cf-front':
                mrt = expected('cf-mrt-front')
                mrt.save(folder / (tag + '-mrt-reference.png'))
                results[tag]['dx9_mrt_reference'] = difference(actual, mrt)
            results[tag]['swapped_reference'] = difference(actual, expected(mode, True))
        else:
            results[tag] = {'missing_capture': True}
    for mode in ('rows-left', 'rows-right', 'columns-left', 'columns-right'):
        file = capture_path(folder, mode)
        if file.exists():
            results['optimized-' + mode] = difference(observe(folder, mode), expected('optimized-' + mode))
    for tag in ('sequential-left', 'sequential-right'):
        file = capture_path(folder, tag)
        if not file.exists():
            results[tag] = {'missing_capture': True}
            continue
        raw = Image.open(file).convert('RGB')
        assert raw.size == (2*W, H)
        result = {}
        for index, side in enumerate(('left', 'right')):
            actual = raw.crop((index*W, 0, (index+1)*W, H))
            actual.save(folder / (tag + '-' + side + '.png'))
            prediction = expected('copy-' + side)
            prediction.save(folder / (tag + '-' + side + '-reference.png'))
            result[side] = difference(actual, prediction)
            result[side]['swapped_reference'] = difference(actual, expected('copy-' + side, True))
            marked = prediction.copy()
            marked.putpixel((0, 0), (255,)*3 if index == 0 else (0,)*3)
            marked.putpixel((1, 0), (0,)*3 if index == 0 else (255,)*3)
            result[side]['cmo_iz_marker'] = difference(actual, marked)
            for name, color in [('blue', (0,0,255)), ('white', (255,)*3)]:
                marked = prediction.copy()
                for x in range(W):
                    marked.putpixel((x, H-1), color if x < W*(1 if index == 0 else 3)//4 else (0,)*3)
                result[side][name + '_line'] = difference(actual, marked)
        samples = [tuple(map(int, match)) for match in re.findall(
            r'seq (\d+) src_x (\d+) src_w (\d+)', (folder / ('flip-' + tag + '.txt')).read_text())]
        result['scanout'] = {'samples': len(samples), 'source_x_values': sorted(set(x for _,x,_ in samples)),
            'source_widths': sorted(set(w for _,_,w in samples)),
            'consecutive_sequence_pairs': sum(b[0] == a[0]+1 for a,b in zip(samples,samples[1:])),
            'observed_changes': sum(b[1] != a[1] for a,b in zip(samples,samples[1:]))}
        result['scanout']['both_eyes_observed'] = len(samples) == 40 and result['scanout']['source_x_values'] == [0,W] and result['scanout']['source_widths'] == [W]
        polls = folder / ('poll-' + tag + '.txt')
        if polls.exists():
            xs = [int(x) for x in re.findall(r'poll \d+ src_x (\d+)', polls.read_text())]
            result['long_poll'] = {'samples': len(xs), 'source_x_values': sorted(set(xs)),
                'observed_changes': sum(a != b for a,b in zip(xs,xs[1:]))}
        result['different_pixels'] = sum(result[side]['different_pixels'] for side in ('left','right'))
        if not result['scanout']['both_eyes_observed']:
            result['different_pixels'] = -1
        results[tag] = result
    negative = capture_path(folder, 'swapped-rows')
    if negative.exists():
        actual = observe(folder, 'swapped-rows')
        results['actual_swapped_input'] = difference(actual, expected('rows-left'))
        results['actual_swapped_input']['swapped_reference'] = difference(actual, expected('rows-left', True))
    inputs = Image.new('RGB', (2*W, H))
    inputs.putdata([eye(x >= W, x % W, y) for y in range(H) for x in range(2*W)])
    inputs.save(folder / 'input-sbs-reference.png')
    (folder / 'comparisons.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    if len(sys.argv) > 2:
        checks = sys.argv[2:]
        if any(results.get(name, {}).get('different_pixels', -1) != 0 for name in checks):
            raise SystemExit(1)


if __name__ == '__main__':
    main()
