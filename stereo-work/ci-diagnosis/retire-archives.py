#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Retain the dependency inventory once both builds can use the extracted prefix."""

import hashlib
from pathlib import Path
import shutil
import time

evidence = Path('/workspace/evidence')
cache = Path('/home/user/diagnosis/cache/artifacts')
while not (evidence / 'kwin-ci-diagnosis-one-value.xml').exists():
    time.sleep(5)

metadata = evidence / 'kwin-ci-diagnosis-dependency-metadata'
metadata.mkdir(exist_ok=True)
for path in cache.glob('*.json'):
    shutil.copyfile(path, metadata / path.name)

with (evidence / 'kwin-ci-diagnosis-retired-archives.tsv').open('w', buffering=1) as output:
    output.write('archive\tbytes\tsha256\n')
    for path in sorted(cache.glob('*.tar')):
        size = path.stat().st_size
        with path.open('rb') as archive:
            digest = hashlib.file_digest(archive, 'sha256').hexdigest()
        assert path.stat().st_size == size
        output.write(f'{path.name}\t{size}\t{digest}\n')
        path.unlink()
