#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent reading of wiz3D S3DOutput/Shaders_dx9.psh, without calibration tables."""
import reference as ref

MODES = ('cf', 'cfl', 'bw-cfl', 'left-right-cfl', 'interlaced', 'checkerboard')


def formula(mode, left, right, front=False, linear=True):
    if mode == 'bw-cfl':
        weights = (.2989, .5870, .1140)
        left = [sum(a * b for a, b in zip(left, weights))] * 3
        right = [sum(a * b for a, b in zip(right, weights))] * 3
    total = [a + b for a, b in zip(left, right)]
    if mode == 'cf':
        threshold = 1e-10 if linear else 1e-6
        return [b / t if t >= threshold else .5 for b, t in zip(right, total)] if front else [t / 2 for t in total]
    luminance_sum = sum(total)
    modulation = sum(right) / luminance_sum if luminance_sum >= .01 else .5
    if mode == 'left-right-cfl':
        return [t * (modulation if front else 1 - modulation) for t in total]
    if mode in ('cfl', 'bw-cfl'):
        return [modulation] * 3 if front else [t / 2 for t in total]
    raise ValueError(mode)


def pixel(mode, domain, front, x, y, swap=False):
    assert mode in MODES and domain in ('linear', 'encoded')
    linear = domain == 'linear'
    decode = ref.decode if linear else lambda v: v
    encode = ref.encode if linear else lambda v: max(0, min(1, v))
    def eye(right, xx=x, yy=y):
        return [decode(v / 255) for v in ref.eye(right ^ swap, xx % ref.W, yy % ref.H)]
    if mode in ('interlaced', 'checkerboard'):
        right = bool((y if mode == 'interlaced' else x + y) % 2)
        if front:
            result = [0 if right else 1] * 3
        elif mode == 'interlaced':
            a, b = eye(right), eye(right, x, y + (-1 if right else 1))
            result = [(v + w) / 2 for v, w in zip(a, b)]
        else:
            result = eye(right)
    else:
        result = formula(mode, eye(False), eye(True), front, linear)
    return tuple(ref.quantize(encode(v)) for v in result)


if __name__ == '__main__':
    ref.PALETTE = True
    for mode in MODES:
        for domain in ('linear', 'encoded'):
            print(mode, domain, 'back', pixel(mode, domain, False, 0, 0),
                  'front', pixel(mode, domain, True, 0, 0))
