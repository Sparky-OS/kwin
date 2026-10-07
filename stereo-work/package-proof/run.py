#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the installed Haruna and helper through the installed headless KWin."""
import gzip
import os
from pathlib import Path
import re
import subprocess
import sys
import time

root = Path('/workspace')
mode = sys.argv[1]
assert mode in ('stereo', 'mono')
out = root / 'evidence/coordinated-packages' / mode
out.mkdir(parents=True, exist_ok=True)
runtime = out / 'runtime'
runtime.mkdir(mode=0o700, exist_ok=True)
home = out / 'home'
home.mkdir(exist_ok=True)
env = dict(os.environ, HOME=str(home), XDG_RUNTIME_DIR=str(runtime),
           LANG='C.UTF-8', LC_ALL='C.UTF-8',
           KWIN_SCREENSHOT_NO_PERMISSION_CHECKS='1',
           KWIN_WAYLAND_NO_PERMISSION_CHECKS='1')
for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'LD_LIBRARY_PATH', 'QT_PLUGIN_PATH'):
    env.pop(name, None)
kwin = subprocess.Popen(['/usr/bin/kwin_wayland', '--virtual', '--xwayland',
                         '--socket=proof', '--width', '1280', '--height', '800'],
                        env=env, stdout=(out / 'kwin.log').open('w'), stderr=subprocess.STDOUT)
client = None
try:
    for _ in range(240):
        if (runtime / 'proof').exists():
            break
        assert kwin.poll() is None, 'KWin exited before creating its socket'
        time.sleep(.25)
    assert (runtime / 'proof').exists(), 'KWin socket timeout'
    time.sleep(2)
    client_env = dict(env, WAYLAND_DISPLAY='proof', QT_QPA_PLATFORM='wayland',
                      WAYLAND_DEBUG='1', QT_LOGGING_RULES='*.debug=false')
    client = subprocess.Popen(['/usr/bin/haruna', str(root / 'package-proof/clips' / mode / f'{mode}.mp4')],
                              env=client_env, stdout=(out / 'client.log').open('w'),
                              stderr=subprocess.STDOUT)
    time.sleep(12)
    assert client.poll() is None, 'Haruna exited during playback'
    for index in range(6):
        capture_env = dict(env, WAYLAND_DISPLAY='proof', QT_QPA_PLATFORM='wayland',
                           QT_CAPTURE_SCREEN='1', QT_CAPTURE_PNG=str(out / f'frame-{index}.png'))
        with (out / f'capture-{index}.log').open('w') as log:
            result = subprocess.run([str(root / 'package-proof/qt-capture'), '', 'red,blue', 'nolabel'],
                                    env=capture_env, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=60)
        # Exit 1 is its mean-colour check; the clip's frame markers are checked separately.
        assert result.returncode in (0, 1), f'capture transport failed: {result.returncode}'
        assert (out / f'frame-{index}.png').exists(), 'capture produced no image'
        time.sleep(.5)
    maps = Path(f'/proc/{client.pid}/maps').read_text()
    (out / 'helper-mapping.txt').write_text('\n'.join(line for line in maps.splitlines()
                                                   if 'libstereo-declare' in line) + '\n')
    assert 'libstereo-declare.so.1' in maps, 'installed helper was not loaded'
finally:
    if client is not None and client.poll() is None:
        client.terminate()
        try:
            client.wait(10)
        except subprocess.TimeoutExpired:
            client.kill()
            client.wait()
    kwin.terminate()
    try:
        kwin.wait(10)
    except subprocess.TimeoutExpired:
        kwin.kill()
        kwin.wait()

log = (out / 'client.log').read_text()
requests = '\n'.join(line for line in log.splitlines()
                     if re.search(r'kde_stereo_content|wp_content_type|get_subsurface', line))
(out / 'requests.txt').write_text(requests + '\n')
assert 'set_content_class' not in log, 'removed protocol request was sent'
declarations = len(re.findall(r'kde_stereo_content_v1.*\.set_content\(3\)', log))
assert declarations == (1 if mode == 'stereo' else 0), declarations
if mode == 'stereo':
    assert re.search(r'wp_content_type_v1.*\.set_content_type\(2\)', log), 'video kind missing'
with gzip.open(out / 'client.log.gz', 'wt') as compressed:
    compressed.write(log)
(out / 'client.log').unlink()
print(f'PASS: installed {mode} Haruna stays alive, loads the helper and declares {declarations} stereo surfaces')
