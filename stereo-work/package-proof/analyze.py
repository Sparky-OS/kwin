#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require the reference clip's expected eye colours and matching frames."""
from pathlib import Path
import sys

import numpy as np
from PIL import Image

namespace = {}
source = Path('/workspace/package-proof/layers.py').read_text().split("sw = int(os.environ")[0]
exec(compile(source, 'layers.py', 'exec'), namespace)
eye_info = namespace['eye_info']
sub_info = namespace['sub_info']
mode = sys.argv[1]
out = Path('/workspace/evidence/coordinated-packages') / mode
files = sorted(out.glob('frame-*.png'))
assert len(files) == 6, len(files)
for path in files:
    picture = np.asarray(Image.open(path).convert('RGB'))
    expected_width = 2560 if mode == 'stereo' else 1280
    assert picture.shape[1] == expected_width, (path, picture.shape)
    eyes = [picture[:, :1280], picture[:, 1280:]] if mode == 'stereo' else [picture]
    if '--swap' in sys.argv:
        eyes.reverse()
    views = [eye_info(eye) for eye in eyes]
    subtitles = [sub_info(eye) for eye in eyes]
    assert all(views) and all(subtitles), (path, views, subtitles)
    frames = [item['frame'] for item in views + subtitles]
    assert len(set(frames)) == 1, (path, frames)
    if mode == 'stereo':
        colours = [item['colour'] for item in views]
        assert colours == ['red', 'blue'], (path, colours)
        assert all(item['green_left'] and item['geom_ok'] for item in views), views
        x, y, width, height = views[0]['rect']
        difference = np.any(eyes[0] != eyes[1], axis=2)
        difference[y:y + height, x:x + width] = False
        assert not difference.any(), (path, int(difference.sum()))
        print(f'PASS: {path.name} red/blue, frame={frames[0]}, controls identical')
    else:
        print(f'PASS: {path.name} mono, frame={frames[0]}')
