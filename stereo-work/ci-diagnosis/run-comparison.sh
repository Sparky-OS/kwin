#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
while ! test -f /workspace/evidence/kwin-ci-diagnosis-build.rc; do
    sleep 15
done
test "$(cat /workspace/evidence/kwin-ci-diagnosis-build.rc)" = 0
exec > /workspace/evidence/kwin-ci-diagnosis-compare.log 2>&1
trap 'echo $? > /workspace/evidence/kwin-ci-diagnosis-compare.rc' EXIT
export CI=true CI_JOB_ID=6016 CI_COMMIT_REF_PROTECTED=false
export CI_PROJECT_DIR=/home/user/diagnosis/kwin CI_PROJECT_NAME=kwin
export CI_COMMIT_REF_NAME=partner/kwin-one-value-v2 CI_COMMIT_SHA=060866b448
export CI_REPOSITORY_URL=/home/user/diagnosis/kwin
export KDECI_CACHE_PATH=/home/user/diagnosis/cache/artifacts
export KDECI_CC_CACHE=/home/user/diagnosis/cache/caches
export KDECI_GITLAB_SERVER=https://invent.kde.org/
export KDECI_PACKAGE_PROJECT=teams/ci-artifacts/suse-qt6.11
export PYTHONPATH=/home/user/diagnosis/site KDE_CI_LOCAL_THREADS=2
python3 -u /workspace/ci-diagnosis/compare.py
