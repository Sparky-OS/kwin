#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stop only this round's containers if either agreed disk floor is crossed."""

from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import subprocess
import time

root = Path(__file__).resolve().parent.parent
evidence = root / 'evidence'
os.environ['DOCKER_HOST'] = 'unix:///K3D/temp/partners/queue/mesa-stereo/docker.sock'
containers = ['mesa-stereo-kwin-ci-diagnosis', 'mesa-stereo-kwin16-local']
with (evidence / 'kwin16-disk-monitor.log').open('a', buffering=1) as output:
    while not (evidence / 'kwin16-disk-monitor.done').exists():
        available = {path: shutil.disk_usage(path).free for path in ('/K3D', '/')}
        output.write(f'{datetime.now(timezone.utc).isoformat()} '
                     f'K3D={available["/K3D"]} root={available["/"]}\n')
        if available['/K3D'] < 20 * 1024**3 or available['/'] < 12 * 1024**3:
            (evidence / 'kwin16-disk-floor-blocker.txt').write_text(str(available) + '\n')
            for container in containers:
                subprocess.run(['docker', 'stop', '--time', '20', container],
                               stdout=output, stderr=subprocess.STDOUT)
            break
        time.sleep(30)
