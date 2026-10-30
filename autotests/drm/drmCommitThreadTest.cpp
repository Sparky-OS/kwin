/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/session.h"
#include "drm_backend.h"
#include "drm_commit.h"
#include "drm_commit_thread.h"
#include "drm_connector.h"
#include "drm_gpu.h"
#include "drm_pipeline.h"
#include "drm_plane.h"

#include <QLoggingCategory>
#include <QScopeGuard>
#include <QTest>

#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <thread>

using namespace KWin;
using namespace std::chrono_literals;

static std::unique_ptr<MockGpu> findRenderDevice(int crtcCount)
{
    const int deviceCount = drmGetDevices2(0, nullptr, 0);
    if (deviceCount <= 0) {
        return nullptr;
    }
    QList<drmDevice *> devices(deviceCount);
    if (drmGetDevices2(0, devices.data(), devices.size()) < 0) {
        return nullptr;
    }
    auto cleanup = qScopeGuard([&devices]() {
        drmFreeDevices(devices.data(), devices.size());
    });
    for (drmDevice *device : std::as_const(devices)) {
        if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
            const int fd = open(device->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
            if (fd != -1) {
                return std::make_unique<MockGpu>(fd, device->nodes[DRM_NODE_RENDER], crtcCount);
            }
        }
    }
    return nullptr;
}

class DrmCommitThreadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testStopWhileCommitting();
    void testRetargetedEyeIsTheCommittedOne();
};

void DrmCommitThreadTest::testStopWhileCommitting()
{
    // The destructor requests the thread's interruption and wakes it, with the mutex held. A thread
    // that checked for the request without the mutex, just before it was made, and then waited for
    // commits would miss that wake-up and never stop: the destructor would wait for it forever.
    // A new thread makes that check as soon as it runs, so each round here stops a thread a few
    // microseconds after starting it, a little later each round, to meet that moment; a watchdog
    // fails the test when a thread has not stopped after ten seconds.
    // (Without CAP_SYS_NICE every thread warns that it has no real time priority.)
    QLoggingCategory::setFilterRules(QStringLiteral("default.warning=false"));
    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    const auto mockGpu = findRenderDevice(1);
    QVERIFY(mockGpu);
    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    QVERIFY(gpu->atomicModeSetting());

    constexpr int rounds = 4000;
    std::mutex mutex;
    std::condition_variable progress;
    int stopped = 0;
    bool done = false;
    std::thread watchdog([&]() {
        std::unique_lock lock(mutex);
        while (!done) {
            const int before = stopped;
            const bool moved = progress.wait_for(lock, 10s, [&]() {
                return done || stopped != before;
            });
            if (!moved) {
                qFatal("A commit thread did not stop: the destructor waited ten seconds after %d of %d rounds", stopped, rounds);
            }
        }
    });
    const auto stopWatchdog = qScopeGuard([&]() {
        {
            std::unique_lock lock(mutex);
            done = true;
        }
        progress.notify_all();
        watchdog.join();
    });

    for (int round = 0; round < rounds; round++) {
        auto thread = std::make_unique<DrmCommitThread>(gpu.get(), QStringLiteral("test"));
        const auto stopAt = std::chrono::steady_clock::now() + std::chrono::microseconds(round % 200);
        while (std::chrono::steady_clock::now() < stopAt) {
        }
        thread.reset();
        {
            std::unique_lock lock(mutex);
            stopped++;
        }
        progress.notify_all();
    }
    QCOMPARE(stopped, rounds);
}

void DrmCommitThreadTest::testRetargetedEyeIsTheCommittedOne()
{
    // A frame sequential commit submitted for a later refresh than its frame's shows that refresh's
    // eye, counted from the refresh it was chosen for, and reports that eye to its pipeline.
    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    const auto mockGpu = findRenderDevice(1);
    QVERIFY(mockGpu);
    const auto mockConnector = std::make_shared<MockConnector>(mockGpu.get());
    mockGpu->connectors.push_back(mockConnector);
    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto connector = std::make_shared<DrmConnector>(gpu.get(), mockConnector->id);
    QVERIFY(connector->init());
    auto plane = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes.front()->id);
    QVERIFY(plane->init());
    auto pipeline = std::make_unique<DrmPipeline>(connector.get());

    const std::chrono::nanoseconds refresh = 8'333'333ns;
    const auto target = std::chrono::steady_clock::now();
    DrmAtomicCommit commit(QList<DrmPipeline *>{pipeline.get()});
    commit.addProperty(plane->srcX, uint64_t(1920) << 16);
    commit.setFrameSequentialEye(pipeline.get(), StereoEye::Right, plane.get(), {0, 1920}, target);
    QCOMPARE(commit.frameSequentialEye(pipeline.get()), std::optional(StereoEye::Right));
    // on time, and an even number of refreshes late, the chosen eye stays
    commit.retargetFrameSequentialEyes(target + 300us, refresh);
    QCOMPARE(commit.frameSequentialEye(pipeline.get()), std::optional(StereoEye::Right));
    commit.retargetFrameSequentialEyes(target + 2 * refresh, refresh);
    QCOMPARE(commit.frameSequentialEye(pipeline.get()), std::optional(StereoEye::Right));
    // an odd number late, the other one, counted from the chosen eye however often it is retargeted
    commit.retargetFrameSequentialEyes(target + refresh, refresh);
    QCOMPARE(commit.frameSequentialEye(pipeline.get()), std::optional(StereoEye::Left));
    commit.retargetFrameSequentialEyes(target + 3 * refresh - 500us, refresh);
    QCOMPARE(commit.frameSequentialEye(pipeline.get()), std::optional(StereoEye::Left));

    // what reaches the kernel is that eye's source
    const auto mockPlane = mockGpu->findPlane(plane->id());
    const auto sourceX = mockPlane->getPropId(QStringLiteral("SRC_X"));
    mockGpu->atomicCommits.clear();
    QVERIFY(commit.test());
    std::optional<uint64_t> committedX;
    for (const Prop &property : mockGpu->atomicCommits.constLast()) {
        if (property.obj == mockPlane->id && property.prop == sourceX) {
            committedX = property.value;
        }
    }
    QCOMPARE(committedX, std::optional<uint64_t>(0));

    // a commit whose eye has no frame to count from, such as a modeset, keeps it
    DrmAtomicCommit modeset(QList<DrmPipeline *>{pipeline.get()});
    modeset.setFrameSequentialEye(pipeline.get(), StereoEye::Right);
    modeset.retargetFrameSequentialEyes(target + refresh, refresh);
    QCOMPARE(modeset.frameSequentialEye(pipeline.get()), std::optional(StereoEye::Right));
}

QTEST_GUILESS_MAIN(DrmCommitThreadTest)
#include "drmCommitThreadTest.moc"
