#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).parent))
spec = importlib.util.spec_from_file_location('izedref', Path(__file__).with_name('ized3d-reference.py'))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
colors = [((0, 0, 0), (0, 0, 0)), ((0, 0, 0), (1, 1, 1)),
          ((0, 0, 0), (8, 8, 8)), ((0, 0, 0), (16, 16, 16)),
          ((255, 0, 0), (0, 255, 255)), ((255, 255, 255), (255, 255, 255)),
          ((160, 32, 16), (16, 224, 128)), ((176, 48, 32), (32, 240, 144))]
r.ref.W, r.ref.H = 8, 2
r.ref.eye = lambda right, x, y: colors[(x + y % 2) % 8][right]
r.ref.quantize = lambda value: value
result = {'source': 'wiz3D 9bcd3da52d52 OutputMethods/S3DOutput/Shaders_dx9.psh',
          'width': 8, 'height': 2,
          'left': [r.ref.eye(False, x, y) for y in range(2) for x in range(8)],
          'right': [r.ref.eye(True, x, y) for y in range(2) for x in range(8)], 'cases': []}
for formula, mode in enumerate(r.MODES):
    for transfer, domain in enumerate(('linear', 'encoded')):
        for panel in (0, 1):
            for swap in (False, True):
                result['cases'].append({'name': f'{mode}-{domain}-{panel}-{int(swap)}',
                    'formula': formula, 'transfer': transfer, 'panel': panel, 'swap': swap,
                    'expected': [r.pixel(mode, domain, bool(panel), x, y, swap) for y in range(2) for x in range(8)]})
Path(sys.argv[1]).write_text(json.dumps(result, separators=(',', ':')) + '\n')
print('Generated', len(result['cases']), 'independent shader cases with 16 pixels each')
