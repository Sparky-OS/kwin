#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare both iZ3D panels against both transfer predictions and exchanged eyes."""
import importlib.util
import json
from pathlib import Path
import sys
import reference as ref

spec = importlib.util.spec_from_file_location('izedref', Path(__file__).with_name('ized3d-reference.py'))
ized = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ized)


def prediction(mode, domain, front, swap):
    return ref.periodic_image(ref.W, ref.H, lambda x, y: ized.pixel(mode, domain, front, x, y, swap))


def main():
    root, baseline = map(Path, sys.argv[1:3])
    ref.PALETTE = True
    results = {}
    passed = True
    for mode in ized.MODES:
        folder = root / mode
        for domain in ('linear', 'encoded'):
            for swap in (False, True):
                for front in (False, True):
                    tag = f'pair-ized-{domain}-' + ('on' if swap else 'off') + ('-second' if front else '')
                    key = mode + '/' + tag
                    if not ref.capture_path(folder, tag).exists():
                        results[key] = {'missing_capture': True}
                        passed = False
                        continue
                    actual = ref.observe(folder, tag)
                    predictions = {transfer: prediction(mode, transfer, front, swap) for transfer in ('linear', 'encoded')}
                    for transfer, image in predictions.items():
                        image.save(folder / (tag + '-reference-' + transfer + '.png'))
                    selected = predictions[domain]
                    exchanged = prediction(mode, domain, front, not swap)
                    result = ref.difference(actual, selected)
                    result['both_transfer_predictions'] = {transfer: ref.difference(actual, image) for transfer, image in predictions.items()}
                    result['exchanged_reference'] = ref.difference(actual, exchanged)
                    expected_exchange = ref.difference(selected, exchanged)['different_pixels']
                    invariant = (mode in ('cf', 'cfl', 'bw-cfl') and not front) or (mode in ('interlaced', 'checkerboard') and front)
                    result['swap_invariant'] = invariant
                    result['passed'] = (result.get('different_pixels') == 0
                        and result['exchanged_reference'].get('different_pixels') == expected_exchange
                        and (expected_exchange == 0 if invariant else expected_exchange > 0))
                    if not swap:
                        before = ref.observe(baseline, 'pair-ized-off' + ('-second' if front else ''))
                        result['before_cf_encoded_vs_requested_formula'] = ref.difference(before, selected)
                    passed &= result['passed']
                    results[key] = result
    report = {'passed': passed, 'captures': results}
    (root / 'comparisons.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
