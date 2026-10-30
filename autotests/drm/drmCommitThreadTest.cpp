/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/session.h"
#include "drm_backend.h"
#include "drm_commit.h"
#include "drm_commit_thread.h"
#include "drm_gpu.h"

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

QTEST_GUILESS_MAIN(DrmCommitThreadTest)
#include "drmCommitThreadTest.moc"
