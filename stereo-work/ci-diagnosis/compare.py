#!/usr/bin/env python3
"""Compare the failed CI tests with fixed dependencies and one CTest worker."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

import yaml

source = Path('/home/user/diagnosis/kwin')
evidence = Path('/workspace/evidence')
os.chdir(source)
sys.path.insert(0, str(source / 'ci-utilities'))
from components import CommonUtils, Dependencies, EnvironmentHandler, Package, PlatformFlavor, TestHandler


def configuration():
    scripts = Path(CommonUtils.scriptsBaseDirectory())
    config = yaml.safe_load((scripts / 'config/global.yml').read_text())
    CommonUtils.recursiveUpdate(config, yaml.safe_load(Path('.kde-ci.yml').read_text()))
    project = scripts / 'config/kwin.yml'
    if project.exists():
        CommonUtils.recursiveUpdate(config, yaml.safe_load(project.read_text()))
    return config


config = configuration()
scripts = Path(CommonUtils.scriptsBaseDirectory())
platform = PlatformFlavor.PlatformFlavor('Linux/Qt6/Shared')
resolver = Dependencies.Resolver(str(scripts / 'repo-metadata/projects-invent'),
                                 str(scripts / 'repo-metadata/branch-rules.yml'), platform)
runtime = resolver.resolve(config['RuntimeDependencies'], 'partner/kwin-one-value-v2')
registry = Package.Registry('/home/user/diagnosis/cache/artifacts',
                            'https://invent.kde.org/', None, 'teams/ci-artifacts/suse-qt6.11')
for contents, metadata in registry.retrieveDependencies(runtime, dependenciesToIgnore=['kwin'], runtime=True):
    with tarfile.open(contents) as archive:
        archive.extractall(source / '_install', filter='fully_trusted')

for name, commit in [('one-value', '060866b448'), ('base', '0ec08b5bd8')]:
    print(f'=== {name}: {commit} ===', flush=True)
    subprocess.run(['git', 'checkout', '--detach', commit], check=True)
    config = configuration()
    environment = EnvironmentHandler.generateFor(str(source / '_install'), config)
    environment.update({str(key): str(value) for key, value in config['Environment'].items()})
    if name == 'base':
        with (evidence / 'kwin-ci-diagnosis-base-build.log').open('w') as output:
            subprocess.run(['cmake', '--build', '_build', '--parallel', '2'], env=environment,
                           stdout=output, stderr=subprocess.STDOUT, check=True)
            staging_env = dict(environment, DESTDIR=str(source / '_staging'))
            subprocess.run(['cmake', '--install', '_build'], env=staging_env,
                           stdout=output, stderr=subprocess.STDOUT, check=True)
        staged = source / '_staging' / str(source / '_install').lstrip('/')
        shutil.copytree(staged, source / '_install', dirs_exist_ok=True)
    options = config['Options']
    options['tests-run-in-parallel'] = False
    options['ctest-arguments'] = '--tests-from-file /workspace/ci-diagnosis/failed-tests.txt'
    with (evidence / f'kwin-ci-diagnosis-{name}-environment.txt').open('w') as output:
        for key in ('CI', 'CI_JOB_ID', 'ASAN_OPTIONS', 'LD_LIBRARY_PATH', 'QT_PLUGIN_PATH',
                    'QML2_IMPORT_PATH', 'LIBGL_ALWAYS_SOFTWARE'):
            output.write(f'{key}={environment.get(key, "") }\n')
        output.write(f'commit={subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()}\n')
        output.write('ctest_workers=1\ncontainer_cpus=2\ntimeout=90\n')
        output.write(f'loadavg={os.getloadavg()}\n')
    passed = TestHandler.run(config, str(source), str(source / '_build'),
                             str(source / '_install'), environment)
    (evidence / f'kwin-ci-diagnosis-{name}.rc').write_text(f'{0 if passed else 1}\n')
    shutil.copyfile(source / 'JUnitTestResults.xml',
                    evidence / f'kwin-ci-diagnosis-{name}.xml')
    print(f'=== {name}: passed={passed}, load={os.getloadavg()} ===', flush=True)
