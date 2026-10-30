/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/outputconfiguration.h"
#include "core/renderbackend.h"
#include "core/renderloop.h"
#include "core/renderloop_p.h"
#include "core/session.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_layer.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_plane.h"
#include "main.h"
#include "qpainter/qpainterbackend.h"

#include <QScopeGuard>
#include <QTest>

#include <fcntl.h>

using namespace KWin;
using namespace std::chrono_literals;

// the render loop asks the application whether it is terminating
class TestApplication : public Application
{
public:
    TestApplication(int &argc, char **argv)
        : Application(argc, argv)
    {
    }

protected:
    void performStartup() override
    {
    }
};

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

class TestLayer : public DrmPipelineLayer
{
public:
    TestLayer(DrmPlane *plane, std::shared_ptr<DrmFramebuffer> framebuffer)
        : DrmPipelineLayer(plane)
        , m_framebuffer(std::move(framebuffer))
    {
    }

    std::shared_ptr<DrmFramebuffer> currentBuffer() const override
    {
        return m_framebuffer;
    }

    void releaseBuffers() override
    {
    }

protected:
    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override
    {
        return std::nullopt;
    }

    bool doEndFrame(const Region &, const Region &, OutputFrame *) override
    {
        return true;
    }

private:
    std::shared_ptr<DrmFramebuffer> m_framebuffer;
};

class MockFrameSequentialTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testScanout_data();
    void testScanout();
    void testHeldPairTargetsNextRefresh();
    void testEyeOfTargetedRefresh();
    void testLateCommitShowsItsRefreshEye();
};

void MockFrameSequentialTest::testScanout_data()
{
    QTest::addColumn<OutputModeline::Flag>("layout");
    QTest::addColumn<StereoEye>("firstEye");
    QTest::addRow("left first") << OutputModeline::Flag::Stereo3DSequentialLeftFirst << StereoEye::Left;
    QTest::addRow("right first") << OutputModeline::Flag::Stereo3DSequentialRightFirst << StereoEye::Right;
}

void MockFrameSequentialTest::testScanout()
{
    QFETCH(OutputModeline::Flag, layout);
    QFETCH(StereoEye, firstEye);

    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice(1);
    QVERIFY(mockGpu);

    const auto mockConnector = std::make_shared<MockConnector>(mockGpu.get());
    mockConnector->addMode(1920, 1080, 120);
    mockGpu->connectors.push_back(mockConnector);

    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto renderBackend = backend->createQPainterBackend();
    Q_UNUSED(renderBackend);
    // the GPU is destroyed after what holds its buffers, which are declared after it, then the manager goes
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto connector = std::make_shared<DrmConnector>(gpu.get(), mockConnector->id);
    QVERIFY(connector->init());
    const auto baseMode = connector->modes().front();
    const auto sequentialMode = std::make_shared<DrmConnectorMode>(baseMode, layout);
    auto plane = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes.front()->id);
    QVERIFY(plane->init());
    auto crtc = std::make_unique<DrmCrtc>(gpu.get(), mockGpu->crtcs.front()->id, 0, plane.get());
    QVERIFY(crtc->init());
    auto pipeline = std::make_unique<DrmPipeline>(connector.get());
    pipeline->setCrtc(crtc.get());
    pipeline->setMode(sequentialMode);
    pipeline->setEnable(true);
    pipeline->setActive(true);
    auto *const mockFb = new MockFb(mockGpu.get(), 3840, 1080);
    auto framebufferData = std::make_shared<DrmFramebufferData>(gpu.get(), mockFb->id, nullptr);
    auto drmFramebuffer = std::make_shared<DrmFramebuffer>(framebufferData, nullptr, FileDescriptor{});
    auto layer = std::make_unique<TestLayer>(plane.get(), drmFramebuffer);
    layer->setEnabled(true);
    layer->setTargetRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    layer->setSourceRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    pipeline->setLayers({layer.get()});
    QCOMPARE(DrmPipeline::commitPipelines({pipeline.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset), DrmPipeline::Error::None);
    mockGpu->atomicCommits.clear();

    const auto primaryPlane = pipeline->crtc()->primaryPlane();
    const auto mockPlane = mockGpu->findPlane(primaryPlane->id());
    QVERIFY(mockPlane);

    const auto sourceX = mockPlane->getPropId(QStringLiteral("SRC_X"));
    const auto sourceW = mockPlane->getPropId(QStringLiteral("SRC_W"));
    const auto framebufferProperty = mockPlane->getPropId(QStringLiteral("FB_ID"));

    struct PlaneCommit {
        uint64_t x;
        uint64_t width;
        uint64_t fb;
    };
    auto lastPlaneCommit = [&]() -> std::optional<PlaneCommit> {
        if (mockGpu->atomicCommits.isEmpty()) {
            return std::nullopt;
        }
        const auto &properties = mockGpu->atomicCommits.constLast();
        std::optional<PlaneCommit> result;
        for (const Prop &property : properties) {
            if (property.obj != mockPlane->id) {
                continue;
            }
            if (!result) {
                result = PlaneCommit{};
            }
            if (property.prop == sourceX) {
                result->x = property.value;
            } else if (property.prop == sourceW) {
                result->width = property.value;
            } else if (property.prop == framebufferProperty) {
                result->fb = property.value;
            }
        }
        return result;
    };

    mockGpu->atomicCommits.clear();
    const std::shared_ptr<OutputFrame> frame;
    const uint64_t expectedWidth = uint64_t(1920) << 16;
    const uint64_t expectedRightX = uint64_t(1920) << 16;
    auto presentAndReadSource = [&](StereoEye eye) -> std::optional<uint64_t> {
        if (DrmPipeline::commitPipelines({pipeline.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset) != DrmPipeline::Error::None
            || mockGpu->atomicCommits.size() != 1) {
            return std::nullopt;
        }
        const auto commit = lastPlaneCommit();
        if (!commit || commit->width != expectedWidth || commit->x != (eye == StereoEye::Left ? 0 : expectedRightX)) {
            return std::nullopt;
        }
        return commit->fb;
    };

    const auto firstBuffer = presentAndReadSource(firstEye);
    QVERIFY(firstBuffer);
    pipeline->pageFlipped(std::chrono::nanoseconds(1), 100, firstEye);
    QCOMPARE(pipeline->frameSequentialNeedsNewFrame(), false);
    mockGpu->atomicCommits.clear();

    const StereoEye secondEye = !firstEye;
    const auto secondBuffer = presentAndReadSource(secondEye);
    QVERIFY(secondBuffer);
    QCOMPARE(*secondBuffer, *firstBuffer);
    pipeline->pageFlipped(std::chrono::nanoseconds(2), 102, secondEye);
    QCOMPARE(pipeline->frameSequentialNeedsNewFrame(), false);
    mockGpu->atomicCommits.clear();

    // The skipped sequence is a transient slot. The next eye still follows
    // the vblank sequence instead of toggling the last submitted eye.
    const auto relockedBuffer = presentAndReadSource(secondEye);
    QVERIFY(relockedBuffer);
    QCOMPARE(*relockedBuffer, *firstBuffer);
    pipeline->pageFlipped(std::chrono::nanoseconds(3), 103, secondEye);
    QCOMPARE(pipeline->frameSequentialNeedsNewFrame(), true);
    mockGpu->atomicCommits.clear();

    const auto nextPairBuffer = presentAndReadSource(firstEye);
    QVERIFY(nextPairBuffer);
    QCOMPARE(*nextPairBuffer, *firstBuffer);
    pipeline->pageFlipped(std::chrono::nanoseconds(4), 104, firstEye);
    QCOMPARE(pipeline->frameSequentialNeedsNewFrame(), false);

}

void MockFrameSequentialTest::testHeldPairTargetsNextRefresh()
{
    // Rendering predicted to take longer than a refresh makes the render loop queue frames two
    // refreshes ahead (triple buffering). KWin's frame sequential twin needs a commit on every
    // refresh, each for the refresh right after the last: a held pair re-committed two refreshes
    // after the last one lands only on every second refresh, with the eye due on the first, so it
    // shows the same eye every time. Its frames are for the next refresh; other layouts keep
    // queueing ahead.
    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice(1);
    QVERIFY(mockGpu);
    const auto mockConnector = std::make_shared<MockConnector>(mockGpu.get());
    mockConnector->addMode(1920, 1080, 120);
    mockGpu->connectors.push_back(mockConnector);

    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto renderBackend = backend->createQPainterBackend();
    Q_UNUSED(renderBackend);
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto connector = std::make_shared<DrmConnector>(gpu.get(), mockConnector->id);
    QVERIFY(connector->init());
    const auto mode2d = connector->modes().front();
    const auto sequentialMode = std::make_shared<DrmConnectorMode>(mode2d, OutputModeline::Flag::Stereo3DSequentialLeftFirst);
    auto pipeline = std::make_unique<DrmPipeline>(connector.get());
    auto output = std::make_unique<DrmOutput>(connector, pipeline.get());
    RenderLoop *loop = output->renderLoop();
    RenderLoopPrivate *const loopPrivate = RenderLoopPrivate::get(loop);

    // what the render loop targets after the vblank at @p timestamp, in refreshes after it
    const auto refreshesAhead = [&](std::chrono::nanoseconds timestamp) {
        const std::chrono::nanoseconds refresh(1'000'000'000'000ull / loop->refreshRate());
        return std::llround((loop->nextPresentationTimestamp() - timestamp).count() / double(refresh.count()));
    };
    const auto now = []() {
        return std::chrono::nanoseconds(std::chrono::steady_clock::now().time_since_epoch());
    };

    // the 2D mode, rendering predicted at 40 ms: the next frame is queued two refreshes ahead
    pipeline->setMode(mode2d);
    pipeline->applyPendingChanges();
    loopPrivate->renderJournal.add(40ms, now());
    QVERIFY(loop->predictedRenderTime() > 2 * std::chrono::nanoseconds(1'000'000'000'000ull / loop->refreshRate()));
    auto vblank = now();
    loopPrivate->notifyVblank(vblank);
    loop->scheduleRepaint();
    QCOMPARE(refreshesAhead(vblank), 2);
    loopPrivate->compositeTimer.stop();

    // the frame sequential twin of the same mode, the same prediction: each frame lands where it
    // was targeted; the first eye of a pair landed on refresh 100, and the re-commit of the held
    // pair is for refresh 101
    pipeline->setMode(sequentialMode);
    pipeline->applyPendingChanges();
    vblank = loop->nextPresentationTimestamp();
    pipeline->pageFlipped(vblank, 100, StereoEye::Left);
    QCOMPARE(pipeline->frameSequentialNeedsNewFrame(), false);
    QCOMPARE(refreshesAhead(vblank), 1);
    loopPrivate->compositeTimer.stop();
    // so is the next pair's first eye, whether it is rendered or re-committed
    vblank = loop->nextPresentationTimestamp();
    pipeline->pageFlipped(vblank, 101, StereoEye::Right);
    QCOMPARE(pipeline->frameSequentialNeedsNewFrame(), true);
    QCOMPARE(refreshesAhead(vblank), 1);
    loopPrivate->compositeTimer.stop();

    // back in 2D the render loop queues ahead again
    pipeline->setMode(mode2d);
    pipeline->applyPendingChanges();
    vblank = loop->nextPresentationTimestamp();
    loopPrivate->notifyVblank(vblank);
    loop->scheduleRepaint();
    QCOMPARE(refreshesAhead(vblank), 2);
    loopPrivate->compositeTimer.stop();
}

void MockFrameSequentialTest::testEyeOfTargetedRefresh()
{
    // A page-flip event handled after the next refresh has passed: the frame that follows is for
    // the refresh after that one, and carries its eye. Choosing the eye of the refresh right after
    // the completed one would show the wrong eye on the refresh that passed and again on the one
    // the frame lands on.
    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice(1);
    QVERIFY(mockGpu);
    const auto mockConnector = std::make_shared<MockConnector>(mockGpu.get());
    mockConnector->addMode(1920, 1080, 120);
    mockGpu->connectors.push_back(mockConnector);

    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto renderBackend = backend->createQPainterBackend();
    Q_UNUSED(renderBackend);
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto connector = std::make_shared<DrmConnector>(gpu.get(), mockConnector->id);
    QVERIFY(connector->init());
    const auto sequentialMode = std::make_shared<DrmConnectorMode>(connector->modes().front(), OutputModeline::Flag::Stereo3DSequentialLeftFirst);
    auto plane = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes.front()->id);
    QVERIFY(plane->init());
    auto crtc = std::make_unique<DrmCrtc>(gpu.get(), mockGpu->crtcs.front()->id, 0, plane.get());
    QVERIFY(crtc->init());
    auto pipeline = std::make_unique<DrmPipeline>(connector.get());
    auto output = std::make_unique<DrmOutput>(connector, pipeline.get());
    pipeline->setCrtc(crtc.get());
    pipeline->setMode(sequentialMode);
    pipeline->setEnable(true);
    pipeline->setActive(true);
    auto *const mockFb = new MockFb(mockGpu.get(), 3840, 1080);
    auto layer = std::make_unique<TestLayer>(plane.get(), std::make_shared<DrmFramebuffer>(std::make_shared<DrmFramebufferData>(gpu.get(), mockFb->id, nullptr), nullptr, FileDescriptor{}));
    layer->setEnabled(true);
    layer->setTargetRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    layer->setSourceRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    pipeline->setLayers({layer.get()});
    QCOMPARE(DrmPipeline::commitPipelines({pipeline.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset), DrmPipeline::Error::None);
    pipeline->applyPendingChanges();

    RenderLoop *loop = output->renderLoop();
    RenderLoopPrivate *const loopPrivate = RenderLoopPrivate::get(loop);
    loopPrivate->compositeTimer.stop();
    const std::chrono::nanoseconds refresh(1'000'000'000'000ull / loop->refreshRate());
    const auto now = []() {
        return std::chrono::nanoseconds(std::chrono::steady_clock::now().time_since_epoch());
    };
    const auto mockPlane = mockGpu->findPlane(plane->id());
    const auto sourceX = mockPlane->getPropId(QStringLiteral("SRC_X"));
    const auto committedEye = [&]() -> std::optional<StereoEye> {
        for (const Prop &property : mockGpu->atomicCommits.constLast()) {
            if (property.obj == mockPlane->id && property.prop == sourceX) {
                return property.value == 0 ? StereoEye::Left : StereoEye::Right;
            }
        }
        return std::nullopt;
    };

    // the left eye of a pair landed on refresh 100; its event is handled a refresh and a quarter later
    // (after the modeset's own completion, which the render loop also took as a vblank)
    QTest::qSleep(20);
    const auto vblank = now() - refresh * 5 / 4;
    pipeline->pageFlipped(vblank, 100, StereoEye::Left);
    loopPrivate->compositeTimer.stop();
    loop->prepareNewFrame();
    auto frame = std::make_shared<OutputFrame>(loop, refresh);
    loop->newFramePrepared();
    const auto target = frame->targetPageflipTime().time_since_epoch();
    QCOMPARE(std::llround((target - vblank).count() / double(refresh.count())), 2ll);
    // refresh 102 is a left eye's: a new pair starts there, and its frame shows the left eye
    QVERIFY(pipeline->frameSequentialNeedsNewFrame());
    mockGpu->atomicCommits.clear();
    QCOMPARE(pipeline->testPresent(frame), DrmPipeline::Error::None);
    QCOMPARE(committedEye(), std::optional(StereoEye::Left));
    frame.reset();

    // landing there, it relocks the order: the next frame, on time, is for 103, a right eye's
    pipeline->pageFlipped(target, 102, StereoEye::Left);
    loopPrivate->compositeTimer.stop();
    loop->prepareNewFrame();
    auto next = std::make_shared<OutputFrame>(loop, refresh);
    loop->newFramePrepared();
    QCOMPARE(std::llround((next->targetPageflipTime().time_since_epoch() - target).count() / double(refresh.count())), 1ll);
    QVERIFY(!pipeline->frameSequentialNeedsNewFrame());
    mockGpu->atomicCommits.clear();
    QCOMPARE(pipeline->testPresent(next), DrmPipeline::Error::None);
    QCOMPARE(committedEye(), std::optional(StereoEye::Right));
    next.reset();

    // a commit without a frame (a modeset) keeps the eye after the last completed refresh
    mockGpu->atomicCommits.clear();
    QCOMPARE(DrmPipeline::commitPipelines({pipeline.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset), DrmPipeline::Error::None);
    QCOMPARE(committedEye(), std::optional(StereoEye::Right));
}

void MockFrameSequentialTest::testLateCommitShowsItsRefreshEye()
{
    // A frame for refresh 101, a right eye's, that reaches the commit thread only after refresh 101
    // has begun: the thread submits it for refresh 102, and the commit must show 102's eye, the left
    // one, not the eye of the refresh its frame was for.
    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice(1);
    QVERIFY(mockGpu);
    const auto mockConnector = std::make_shared<MockConnector>(mockGpu.get());
    mockConnector->addMode(1920, 1080, 120);
    mockGpu->connectors.push_back(mockConnector);

    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto renderBackend = backend->createQPainterBackend();
    Q_UNUSED(renderBackend);
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto connector = std::make_shared<DrmConnector>(gpu.get(), mockConnector->id);
    QVERIFY(connector->init());
    const auto sequentialMode = std::make_shared<DrmConnectorMode>(connector->modes().front(), OutputModeline::Flag::Stereo3DSequentialLeftFirst);
    auto plane = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes.front()->id);
    QVERIFY(plane->init());
    auto crtc = std::make_unique<DrmCrtc>(gpu.get(), mockGpu->crtcs.front()->id, 0, plane.get());
    QVERIFY(crtc->init());
    auto pipeline = std::make_unique<DrmPipeline>(connector.get());
    auto output = std::make_unique<DrmOutput>(connector, pipeline.get());
    pipeline->setCrtc(crtc.get());
    pipeline->setMode(sequentialMode);
    pipeline->setEnable(true);
    pipeline->setActive(true);
    auto *const mockFb = new MockFb(mockGpu.get(), 3840, 1080);
    auto layer = std::make_unique<TestLayer>(plane.get(), std::make_shared<DrmFramebuffer>(std::make_shared<DrmFramebufferData>(gpu.get(), mockFb->id, nullptr), nullptr, FileDescriptor{}));
    layer->setEnabled(true);
    layer->setTargetRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    layer->setSourceRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    pipeline->setLayers({layer.get()});
    QCOMPARE(DrmPipeline::commitPipelines({pipeline.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset), DrmPipeline::Error::None);
    pipeline->applyPendingChanges();

    RenderLoop *loop = output->renderLoop();
    RenderLoopPrivate *const loopPrivate = RenderLoopPrivate::get(loop);
    loopPrivate->compositeTimer.stop();
    const std::chrono::nanoseconds refresh(1'000'000'000'000ull / loop->refreshRate());
    const auto now = []() {
        return std::chrono::nanoseconds(std::chrono::steady_clock::now().time_since_epoch());
    };
    const auto mockPlane = mockGpu->findPlane(plane->id());
    const auto sourceX = mockPlane->getPropId(QStringLiteral("SRC_X"));
    const auto commits = [&mockGpu]() {
        std::scoped_lock lock(mockGpu->m_mutex);
        return mockGpu->atomicCommits;
    };

    // the left eye of a pair landed on refresh 100, a quarter of a refresh ago (after the modeset's
    // own completion, which the render loop also took as a vblank)
    QTest::qSleep(20);
    const auto vblank = now() - refresh / 4;
    pipeline->pageFlipped(vblank, 100, StereoEye::Left);
    loopPrivate->compositeTimer.stop();
    loop->prepareNewFrame();
    auto frame = std::make_shared<OutputFrame>(loop, refresh);
    loop->newFramePrepared();
    QCOMPARE(std::llround((frame->targetPageflipTime().time_since_epoch() - vblank).count() / double(refresh.count())), 1ll);
    // the frame reaches the commit thread when refresh 101 has begun
    while (now() < vblank + refresh * 13 / 10) {
        QTest::qSleep(1);
    }
    const auto before = commits().size();
    QCOMPARE(pipeline->present({layer.get()}, frame), DrmPipeline::Error::None);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (commits().size() == before) {
        QVERIFY(std::chrono::steady_clock::now() < deadline);
        QTest::qSleep(1);
    }
    std::optional<StereoEye> shown;
    for (const Prop &property : commits().constLast()) {
        if (property.obj == mockPlane->id && property.prop == sourceX) {
            shown = property.value == 0 ? StereoEye::Left : StereoEye::Right;
        }
    }
    QCOMPARE(shown, std::optional(StereoEye::Left));
    frame.reset();
}

int main(int argc, char **argv)
{
    TestApplication app(argc, argv);
    MockFrameSequentialTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mockFrameSequentialTest.moc"
