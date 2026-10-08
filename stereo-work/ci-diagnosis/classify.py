#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require a result for every failed CI executable on both source commits."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
evidence = root / 'evidence'
expected = (root / 'ci-diagnosis/failed-tests.txt').read_text().splitlines()


def results(name, names=expected):
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
    assert set(result) == set(names), (set(names) - set(result), set(result) - set(names))
    return result


cleanup = results('one-value')
base = results('base')
fixed = results('fixed', ['kwin-testStereoDownscale'])
retest_names = [name for name in expected if name != 'kwin-testStereoDownscale'
                and cleanup[name][0] == 'fail' and base[name][0] == 'pass']
retests = results('retest', retest_names) if retest_names else {}
lines = ['| Test | One-value | Base | Corrected cleanup | Classification |', '|---|---|---|---|---|']
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
    correction = 'unchanged; not rerun'
    if name in fixed:
        outcome, duration = fixed[name]
        correction = f'{outcome} ({duration:.2f}s)'
        if category == 'fails only one-value; investigate regression' and outcome == 'pass':
            category = 'one-value regression; corrected test data passes'
        elif outcome != 'pass':
            category = 'corrected test still fails; investigate regression'
    elif name in retests:
        outcome, duration = retests[name]
        correction = f'{outcome} ({duration:.2f}s)'
        category = ('cleanup failure not reproduced on the same implementation' if outcome == 'pass'
                    else 'cleanup-only failure persists; investigate regression')
    counts[category] = counts.get(category, 0) + 1
    lines.append(f'| {name} | {current} ({current_time:.2f}s) | {prior} ({prior_time:.2f}s) | {correction} | {category} |')
(evidence / 'kwin-ci-diagnosis-table.md').write_text('\n'.join(lines) + '\n')
for category, count in counts.items():
    print(f'{count}: {category}')
sys.exit(int(any('investigate' in category or 'not verified' in category for category in counts)))
