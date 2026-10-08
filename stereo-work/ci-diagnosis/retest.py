#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Retest cleanup-only mismatches after the corrected build finishes."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import yaml

source = Path('/home/user/diagnosis/kwin')
evidence = Path('/workspace/evidence')
while not (evidence / 'kwin-ci-diagnosis-compare.rc').exists():
    time.sleep(5)
assert (evidence / 'kwin-ci-diagnosis-compare.rc').read_text().strip() == '0'
assert (evidence / 'kwin-ci-diagnosis-fixed.rc').read_text().strip() == '0'
os.chdir(source)
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
assert commit.startswith('a21d282774')
changed = subprocess.check_output(['git', 'diff', '--name-only', '060866b448', 'HEAD'], text=True)
assert changed.strip() == 'autotests/test_stereo_downscale.cpp'

os.environ.update({
    'CI': 'true', 'CI_JOB_ID': '6016', 'CI_COMMIT_REF_PROTECTED': 'false',
    'CI_PROJECT_DIR': str(source), 'CI_PROJECT_NAME': 'kwin',
    'CI_COMMIT_REF_NAME': 'partner/kwin-one-value-v2', 'CI_COMMIT_SHA': '060866b448',
    'CI_REPOSITORY_URL': str(source),
    'KDECI_CACHE_PATH': '/home/user/diagnosis/cache/artifacts',
    'KDECI_CC_CACHE': '/home/user/diagnosis/cache/caches',
    'KDECI_GITLAB_SERVER': 'https://invent.kde.org/',
    'KDECI_PACKAGE_PROJECT': 'teams/ci-artifacts/suse-qt6.11',
    'KDE_CI_LOCAL_THREADS': '2',
})
sys.path.insert(0, str(source / 'ci-utilities'))
from components import CommonUtils, EnvironmentHandler, TestHandler

scripts = Path(CommonUtils.scriptsBaseDirectory())
config = yaml.safe_load((scripts / 'config/global.yml').read_text())
CommonUtils.recursiveUpdate(config, yaml.safe_load(Path('.kde-ci.yml').read_text()))
project = scripts / 'config/kwin.yml'
if project.exists():
    CommonUtils.recursiveUpdate(config, yaml.safe_load(project.read_text()))
config['Options']['tests-run-in-parallel'] = False
config['Options']['ctest-arguments'] = '--tests-from-file /workspace/evidence/kwin-ci-diagnosis-retest-tests.txt'
environment = EnvironmentHandler.generateFor(str(source / '_install'), config)
environment.update({str(key): str(value) for key, value in config['Environment'].items()})
with (evidence / 'kwin-ci-diagnosis-retest-environment.txt').open('w') as output:
    output.write(f'commit={commit}\nimplementation_diff=downscale test data only\n')
    output.write('ctest_workers=1\ncontainer_cpus=2\ntimeout=90\n')
    for key in ('CI', 'CI_JOB_ID', 'ASAN_OPTIONS', 'LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH'):
        output.write(f'{key}={environment.get(key, "")}\n')
    output.write(f'loadavg={os.getloadavg()}\n')

for process in Path('/proc').glob('[0-9]*'):
    try:
        executable = (process / 'exe').resolve(strict=True)
        if executable.is_relative_to(source / '_build'):
            os.kill(int(process.name), signal.SIGKILL)
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
subprocess.run(['killall', '-9', 'Xwayland', 'kscreenlocker_greet'], check=False)
passed = TestHandler.run(config, str(source), str(source / '_build'),
                         str(source / '_install'), environment)
(evidence / 'kwin-ci-diagnosis-retest.rc').write_text(f'{0 if passed else 1}\n')
shutil.copyfile(source / 'JUnitTestResults.xml', evidence / 'kwin-ci-diagnosis-retest.xml')
print(f'Cleanup mismatch retest: passed={passed}', flush=True)
sys.exit(int(not passed))
