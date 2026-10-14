#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check each fresh output-mode session, including independent pair settings."""
import json
import pathlib
import sys
import reference as ref

MODES = ['rows-left', 'rows-right', 'columns-left', 'columns-right', 'checker-left',
         'checker-right', 'anaglyph-modern', 'anaglyph-crt', 'sbs', 'tab', 'sbs-full',
         'frame-packing', '2d', 'pair-dual', 'pair-mirror-h', 'pair-mirror-v', 'pair-ized']


def main():
    root = pathlib.Path(sys.argv[1])
    ref.PALETTE = True
    results = {}
    passed = True
    for mode in MODES:
        folder = root / mode
        ref.W, ref.H = (1920, 1080) if mode == 'sbs-full' else (1280, 720) if mode == 'frame-packing' else (640, 480)
        paired = mode.startswith('pair-')
        for state in ['off', 'first', 'both'] if paired else ['off', 'first']:
            for second in [False, True] if paired else [False]:
                tag = f'{mode}-{state}' + ('-second' if second else '')
                if not ref.capture_path(folder, tag).exists():
                    results[tag] = {'missing_capture': True}
                    passed = False
                    continue
                formula = mode + ('-second' if second else '')
                if mode == 'pair-ized':
                    formula = 'encoded-cf-front' if second else 'encoded-cf-back'
                elif mode.startswith('anaglyph-'):
                    formula += '-gamma22'
                swap = state == 'both' or (state == 'first' and not second)
                actual = ref.observe(folder, tag)
                predicted = ref.expected(formula, swap)
                exchanged = ref.expected(formula, not swap)
                predicted.save(folder / (tag + '-reference.png'))
                result = ref.difference(actual, predicted)
                result['exchanged_reference'] = ref.difference(actual, exchanged)
                expected_exchange = ref.difference(predicted, exchanged)['different_pixels']
                invariant = mode == '2d' or formula == 'encoded-cf-back'
                result['swap_invariant'] = invariant
                result['passed'] = (result.get('different_pixels') == 0
                                    and result['exchanged_reference'].get('different_pixels') == expected_exchange
                                    and (expected_exchange == 0 if invariant else expected_exchange > 0))
                passed &= result['passed']
                results[tag] = result
    report = {'passed': passed, 'captures': results}
    (root / 'comparisons.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
