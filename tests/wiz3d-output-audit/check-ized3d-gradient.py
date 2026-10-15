#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Distinguish the iZ3D half-row offset direction using the gradient fixture."""
import importlib.util
import json
from pathlib import Path
import sys
from PIL import Image
import reference as ref

spec = importlib.util.spec_from_file_location('izedref', Path(__file__).with_name('ized3d-reference.py'))
ized = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ized)
ref.PALETTE = False
root = Path(sys.argv[1])


def prediction(domain, front, swap, sign=1):
    # The input repeats every 64 pixels; use an interior tile, then evaluate
    # the real image edges separately so counterfactual wrapping is exact too.
    pixel = lambda x, y: ized.pixel('interlaced', domain, front, x, y, swap, sign)
    tile = Image.new('RGB', (64, 64))
    tile.putdata([pixel(x, y + 64) for y in range(64) for x in range(64)])
    image = Image.new('RGB', (ref.W, ref.H))
    for y in range(0, ref.H, 64):
        for x in range(0, ref.W, 64):
            image.paste(tile, (x, y))
    for y in (0, ref.H - 1):
        for x in range(ref.W):
            image.putpixel((x, y), pixel(x, y))
    return image


results = {}
for domain in ('linear', 'encoded'):
    for swap in (False, True):
        for front in (False, True):
            tag = f'pair-ized-{domain}-' + ('on' if swap else 'off') + ('-second' if front else '')
            actual = ref.observe(root, tag)
            expected = prediction(domain, front, swap)
            expected.save(root / (tag + '-reference.png'))
            exchanged = prediction(domain, front, not swap)
            opposite_offset = prediction(domain, front, swap, -1)
            opposite_offset.save(root / (tag + '-wrong-offset.png'))
            result = ref.difference(actual, expected)
            result['exchanged_reference'] = ref.difference(actual, exchanged)
            result['opposite_offset_reference'] = ref.difference(actual, opposite_offset)
            expected_offset_difference = ref.difference(expected, opposite_offset)['different_pixels']
            result['both_transfer_predictions'] = {other: ref.difference(actual, prediction(other, front, swap)) for other in ('linear', 'encoded')}
            result['passed'] = (result.get('different_pixels') == 0
                and result['exchanged_reference'].get('different_pixels') == (0 if front else ref.W * ref.H)
                and result['opposite_offset_reference'].get('different_pixels') == expected_offset_difference
                and (expected_offset_difference == 0 if front else expected_offset_difference > 0))
            results[tag] = result
report = {'passed': all(value['passed'] for value in results.values()), 'captures': results}
(root / 'comparisons.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['passed'] else 1)
