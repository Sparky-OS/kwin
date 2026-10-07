#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
exec > /workspace/evidence/kwin-ci-diagnosis-build.log 2>&1
trap 'echo $? > /workspace/evidence/kwin-ci-diagnosis-build.rc' EXIT
mkdir -p /home/user/diagnosis/cache /home/user/diagnosis/site
cp /workspace/evidence/kde-ci-local-configure/site/sitecustomize.py /home/user/diagnosis/site/
if test ! -d /home/user/diagnosis/cache/artifacts; then
    cp -a /cache-ro/artifacts /home/user/diagnosis/cache/
fi
if test ! -d /home/user/diagnosis/kwin/.git; then
    git clone --no-hardlinks /workspace/kwin /home/user/diagnosis/kwin
fi
cd /home/user/diagnosis/kwin
git checkout --detach 060866b448
if test ! -d ci-utilities/.git; then
    git clone --no-hardlinks /workspace/evidence/kde-ci-local-configure/kde/ci-utilities ci-utilities
    git clone --no-hardlinks /workspace/evidence/kde-ci-local-configure/kde/ci-utilities/repo-metadata ci-utilities/repo-metadata
fi
export CI=true CI_JOB_ID=6016 CI_COMMIT_REF_PROTECTED=false
export CI_PROJECT_DIR=$PWD CI_PROJECT_NAME=kwin CI_COMMIT_REF_NAME=partner/kwin-one-value-v2
export CI_COMMIT_SHA=060866b448 CI_REPOSITORY_URL=$PWD
export KDECI_CACHE_PATH=/home/user/diagnosis/cache/artifacts
export KDECI_CC_CACHE=/home/user/diagnosis/cache/caches
export KDECI_GITLAB_SERVER=https://invent.kde.org/
export KDECI_PACKAGE_PROJECT=teams/ci-artifacts/suse-qt6.11
export PYTHONPATH=/home/user/diagnosis/site KDE_CI_LOCAL_THREADS=2
uname -m
git rev-parse HEAD
python3 -u ci-utilities/run-ci-build.py --project kwin --branch partner/kwin-one-value-v2 --platform Linux/Qt6/Shared --extra-cmake-args=-DBUILD_WITH_QT6=ON --extra-cmake-args=-DQT_MAJOR_VERSION=6 --extra-cmake-args=-DBUILD_TESTING=ON --only-build --skip-publishing
