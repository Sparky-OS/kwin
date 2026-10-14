#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail unless the measured audit contains both positive and negative controls."""
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
def read(name):
    return json.loads((root / name / 'comparisons.json').read_text())

matrix, pilot, fresh = read('vkms'), read('pilot'), read('ized-first')
positive = ['rows-left', 'rows-right', 'columns-left', 'columns-right',
            'checker-left', 'checker-right', 'sbs', 'tab', 'pair-dual', 'pair-dual-second']
for kind in ('h', 'v', 'hv', 'vh'):
    positive += ['pair-mirror-' + kind, 'pair-mirror-' + kind + '-second']
for name in positive:
    assert matrix[name]['different_pixels'] == 0, name
    assert matrix[name]['swapped_reference']['different_pixels'] == 307200, name
print(f'PASS: {len(positive)} exact positive captures; every swapped reference fails at all pixels')
negative = pilot['actual_swapped_input']
assert negative['different_pixels'] == 307200
assert negative['swapped_reference']['different_pixels'] == 0
print('PASS: actual swapped input fails the original oracle and passes the swapped oracle')
assert matrix['anaglyph-modern']['different_pixels'] > 0
for name in ('pair-ized', 'pair-ized-second'):
    assert fresh[name]['different_pixels'] == 307200
    assert fresh[name]['encoded_arithmetic_diagnostic']['different_pixels'] == 0
assert fresh['pair-ized-second']['dx9_mrt_reference']['different_pixels'] == 307200
assert fresh['pair-ized-second']['encoded_arithmetic_swapped']['different_pixels'] == 307200
print('PASS: anaglyph and iZ3D differences retained; no tolerance promoted to equality')
sequential = read('sequential-fresh')
for name in ('sequential-left', 'sequential-right'):
    result = sequential[name]
    assert result['different_pixels'] == -1
    assert result['scanout']['samples'] == 40
    assert not result['scanout']['both_eyes_observed']
    assert result['long_poll']['samples'] == 120
    assert result['long_poll']['observed_changes'] == 0
for side in ('left', 'right'):
    assert sequential['sequential-left'][side]['different_pixels'] == 0
    assert sequential['sequential-left'][side]['cmo_iz_marker']['different_pixels'] == 2
    assert sequential['sequential-left'][side]['blue_line']['different_pixels'] == 640
    assert sequential['sequential-left'][side]['white_line']['different_pixels'] == 640
    assert sequential['sequential-right'][side]['different_pixels'] == 307200
print('PASS: temporal failures and marker differences retained explicitly')
package = (root / 'package-sha256.txt').read_text()
for name in ('ized-first', 'sequential-fresh'):
    assert (root / name / 'runtime-sha256.txt').read_text() == package
print('PASS: both fresh guests ran the package-image KWin executable and library')
