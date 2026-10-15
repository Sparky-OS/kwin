#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a static rows fixture before, during and after a libkscreen swap."""
import json
from pathlib import Path
import sys
import reference as ref

root = Path(sys.argv[1])
ref.PALETTE = True
results = {}
for tag, swap in [('static-initial', False), ('libkscreen-first', True), ('libkscreen-off', False)]:
    actual = ref.observe(root, tag)
    expected = ref.expected('rows-left', swap)
    expected.save(root / (tag + '-reference.png'))
    result = ref.difference(actual, expected)
    result['exchanged_reference'] = ref.difference(actual, ref.expected('rows-left', not swap))
    result['passed'] = result.get('different_pixels') == 0 and result['exchanged_reference'].get('different_pixels') == ref.W * ref.H
    results[tag] = result
passed = all(result['passed'] for result in results.values()) and (root / 'settings-roundtrip.exit').read_text().strip() == '0'
report = {'passed': passed, 'captures': results}
(root / 'comparisons.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if passed else 1)
