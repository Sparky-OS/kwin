/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/outputconfiguration.h"
#include "core/session.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_layer.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_plane.h"
#include "qpainter/qpainterbackend.h"

#include <QTest>

#include <fcntl.h>

using namespace KWin;

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

    gpu.reset();
    GpuManager::s_self.reset();
}

QTEST_GUILESS_MAIN(MockFrameSequentialTest)
#include "mockFrameSequentialTest.moc"
