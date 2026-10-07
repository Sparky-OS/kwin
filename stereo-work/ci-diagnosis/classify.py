#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require a result for every failed CI executable on both source commits."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
evidence = root / 'evidence'
expected = (root / 'ci-diagnosis/failed-tests.txt').read_text().splitlines()


def results(name):
    tree = ET.parse(evidence / f'kwin-ci-diagnosis-{name}.xml')
    result = {}
    for test in tree.findall('.//testcase'):
        name = test.attrib['name']
        assert name not in result, name
        if test.find('skipped') is not None or test.get('status') == 'notrun':
            outcome = 'skipped'
        elif test.find('failure') is not None or test.find('error') is not None:
            outcome = 'fail'
        else:
            outcome = 'pass'
        result[name] = (outcome, float(test.attrib.get('time', 0)))
    assert set(result) == set(expected), (set(expected) - set(result), set(result) - set(expected))
    return result


cleanup = results('one-value')
base = results('base')
lines = ['| Test | One-value | Base | Classification |', '|---|---|---|---|']
counts = {}
for name in expected:
    current, current_time = cleanup[name]
    prior, prior_time = base[name]
    category = {
        ('pass', 'pass'): 'passes both; serial retest resolves original failure',
        ('fail', 'fail'): 'fails both; also present in accepted base',
        ('fail', 'pass'): 'fails only one-value; investigate regression',
        ('pass', 'fail'): 'passes only one-value',
    }.get((current, prior), 'skipped; not verified')
    counts[category] = counts.get(category, 0) + 1
    lines.append(f'| {name} | {current} ({current_time:.2f}s) | {prior} ({prior_time:.2f}s) | {category} |')
(evidence / 'kwin-ci-diagnosis-table.md').write_text('\n'.join(lines) + '\n')
for category, count in counts.items():
    print(f'{count}: {category}')
sys.exit(int(any('investigate' in category or 'not verified' in category for category in counts)))
