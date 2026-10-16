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
#include "drm_stereo.h"
#include "main.h"
#include "qpainter/qpainterbackend.h"

#include <QScopeGuard>
#include <QTest>

#include <cstring>
#include <fcntl.h>

using namespace KWin;

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

class FrameLayer : public DrmPipelineLayer
{
public:
    FrameLayer(DrmPlane *plane, std::shared_ptr<DrmFramebuffer> framebuffer)
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

// Every HDMI 1.4a 3D structure a display lists (Appendix H, Table H-2), and what KWin tells the
// kernel with them: the frame each one sends, the side by side (full) timing and the 3D metadata of
// the picture and depth structures (Table H-4)
class MockDepthStructuresTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void listsEveryStructure();
    void commitsTheStructure_data();
    void commitsTheStructure();
};

static std::unique_ptr<MockGpu> findRenderDevice()
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
                return std::make_unique<MockGpu>(fd, device->nodes[DRM_NODE_RENDER], 1);
            }
        }
    }
    return nullptr;
}

static const QList<std::pair<uint32_t, OutputModeline::Flag>> s_structures{
    {DRM_MODE_FLAG_3D_FRAME_PACKING, OutputModeline::Flag::Stereo3DFramePacking},
    {DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE, OutputModeline::Flag::Stereo3DFieldAlternative},
    {DRM_MODE_FLAG_3D_LINE_ALTERNATIVE, OutputModeline::Flag::Stereo3DLineAlternative},
    {DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL, OutputModeline::Flag::Stereo3DSideBySideFull},
    {DRM_MODE_FLAG_3D_L_DEPTH, OutputModeline::Flag::Stereo3DLDepth},
    {DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH, OutputModeline::Flag::Stereo3DLDepthGraphics},
    {DRM_MODE_FLAG_3D_TOP_AND_BOTTOM, OutputModeline::Flag::Stereo3DTopAndBottom},
    {DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF, OutputModeline::Flag::Stereo3DSideBySideHalf},
};

// the 2D mode, then the same mode in each 3D structure, and one 3D flag no structure has
static std::shared_ptr<MockConnector> stereoConnector(MockGpu *gpu, bool properties)
{
    auto connector = std::make_shared<MockConnector>(gpu);
    const drmModeModeInfo base = connector->modes.front();
    for (const auto &[drmFlag, flag] : s_structures) {
        drmModeModeInfo mode = base;
        mode.flags |= drmFlag;
        connector->modes.push_back(mode);
    }
    drmModeModeInfo reserved = base;
    reserved.flags |= 15 << 14;
    connector->modes.push_back(reserved);
    if (properties) {
        connector->props << MockProperty(connector.get(), QStringLiteral("side by side full timing"), 0, DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_ENUM,
                                         {QByteArrayLiteral("whole line doubled"), QByteArrayLiteral("blanking kept")});
        connector->props << MockProperty(connector.get(), QStringLiteral("HDMI 3D metadata"), 0, DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_BLOB);
    }
    return connector;
}

void MockDepthStructuresTest::listsEveryStructure()
{
    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice();
    QVERIFY(mockGpu);
    const auto connector = stereoConnector(mockGpu.get(), false);
    mockGpu->connectors = {connector};
    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto drmConnector = std::make_shared<DrmConnector>(gpu.get(), connector->id);
    QVERIFY(drmConnector->init());
    const auto modes = drmConnector->modes();
    // the 2D mode and every structure once; the reserved flag is left out
    QCOMPARE(modes.size(), 1 + s_structures.size());
    for (const auto &[drmFlag, flag] : s_structures) {
        const auto count = std::ranges::count_if(modes, [flag](const auto &mode) {
            return (mode->flags() & OutputModeline::AllStereo3D) == OutputModeline::Flags(flag);
        });
        QVERIFY2(count == 1, qPrintable(QStringLiteral("structure %1 listed %2 times").arg(drmFlag >> 14).arg(count)));
    }
    // a connector without the properties is still an output; it just has neither setting
    auto pipeline = std::make_unique<DrmPipeline>(drmConnector.get());
    auto output = std::make_unique<DrmOutput>(drmConnector, pipeline.get());
    QVERIFY(!(output->capabilities() & BackendOutput::Capability::SideBySideFullTiming));
    QVERIFY(output->capabilities() & BackendOutput::Capability::StereoDepth);
}

void MockDepthStructuresTest::commitsTheStructure_data()
{
    QTest::addColumn<int>("structure");
    QTest::addColumn<bool>("blankingKept");
    QTest::addColumn<uint32_t>("viewingDistance");
    for (int i = 0; i < s_structures.size(); ++i) {
        QTest::addRow("structure-%d", s_structures[i].first >> 14) << i << false << 0u;
    }
    const int depth = 4;
    QTest::addRow("L + depth at 250 cm") << depth << false << 250u;
    QTest::addRow("side by side full, blanking kept") << 3 << true << 0u;
}

void MockDepthStructuresTest::commitsTheStructure()
{
    QFETCH(int, structure);
    QFETCH(bool, blankingKept);
    QFETCH(uint32_t, viewingDistance);

    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice();
    QVERIFY(mockGpu);
    const auto connector = stereoConnector(mockGpu.get(), true);
    mockGpu->connectors = {connector};
    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto renderBackend = backend->createQPainterBackend();
    Q_UNUSED(renderBackend);
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto drmConnector = std::make_shared<DrmConnector>(gpu.get(), connector->id);
    QVERIFY(drmConnector->init());
    auto pipeline = std::make_unique<DrmPipeline>(drmConnector.get());
    auto output = std::make_unique<DrmOutput>(drmConnector, pipeline.get());
    QVERIFY(output->capabilities() & BackendOutput::Capability::SideBySideFullTiming);
    QVERIFY(output->capabilities() & BackendOutput::Capability::StereoDepth);
    auto plane = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes[0]->id);
    QVERIFY(plane->init());
    auto crtc = std::make_unique<DrmCrtc>(gpu.get(), mockGpu->crtcs[0]->id, 0, plane.get());
    QVERIFY(crtc->init());
    pipeline->setCrtc(crtc.get());

    const auto flag = s_structures[structure].second;
    const auto modes = drmConnector->modes();
    const auto it = std::ranges::find_if(modes, [flag](const auto &mode) {
        return (mode->flags() & OutputModeline::AllStereo3D) == OutputModeline::Flags(flag);
    });
    QVERIFY(it != modes.end());
    const auto mode = *it;
    const drmModeModeInfo &info = *mode->nativeMode();
    const StereoLayout layout = stereoLayoutForMode(mode->flags());
    const QSize frame = stereoFrameSize(info, layout);

    // as the backend applies a configuration: queued, committed, then applied
    OutputConfiguration configuration;
    const auto change = configuration.changeSet(output.get());
    change->enabled = true;
    change->currentMode = mode->modeline();
    change->sideBySideFullTiming = blankingKept ? SideBySideFullTiming::BlankingKept : SideBySideFullTiming::WholeLineDoubled;
    change->stereoViewingDistance = viewingDistance;
    QVERIFY(output->queueChanges(change));
    pipeline->setEnable(true);
    pipeline->setActive(true);

    auto *mockFb = new MockFb(mockGpu.get(), frame.width(), frame.height());
    auto framebufferData = std::make_shared<DrmFramebufferData>(gpu.get(), mockFb->id, nullptr);
    const auto framebuffer = std::make_shared<DrmFramebuffer>(framebufferData, nullptr, FileDescriptor{});
    auto layer = std::make_unique<FrameLayer>(plane.get(), framebuffer);
    layer->setEnabled(true);
    layer->setSourceRect(Rect(QPoint(0, 0), frame));
    layer->setTargetRect(Rect(QPoint(0, 0), frame));
    pipeline->setLayers({layer.get()});

    mockGpu->atomicCommits.clear();
    QCOMPARE(DrmPipeline::commitPipelines({pipeline.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset), DrmPipeline::Error::None);
    output->applyQueuedChanges(change);
    QCOMPARE(pipeline->mode(), mode);
    QVERIFY(!mockGpu->atomicCommits.isEmpty());
    const auto &properties = mockGpu->atomicCommits.constLast();
    const auto valueOf = [&](MockObject *object, const QString &name) -> std::optional<uint64_t> {
        const uint32_t prop = object->getPropId(name);
        const auto found = std::ranges::find_if(properties, [object, prop](const Prop &value) {
            return value.obj == object->id && value.prop == prop;
        });
        return found == properties.end() ? std::nullopt : std::optional(found->value);
    };

    // the plane scans out the whole 3D frame the kernel times (HDMI 1.4a, Figures 8-3 and H-2 to H-6)
    QCOMPARE(valueOf(mockGpu->planes[0].get(), QStringLiteral("SRC_W")), uint64_t(frame.width()) << 16);
    QCOMPARE(valueOf(mockGpu->planes[0].get(), QStringLiteral("SRC_H")), uint64_t(frame.height()) << 16);
    switch (layout) {
    case StereoLayout::FieldAlternative:
    case StereoLayout::LDepth:
        QCOMPARE(frame, QSize(info.hdisplay, info.vtotal + info.vdisplay));
        break;
    case StereoLayout::LDepthGraphics:
        QCOMPARE(frame, QSize(info.hdisplay, 3 * info.vtotal + info.vdisplay));
        break;
    case StereoLayout::LineAlternative:
        QCOMPARE(frame, QSize(info.hdisplay, 2 * info.vdisplay));
        break;
    default:
        break;
    }

    QCOMPARE(valueOf(connector.get(), QStringLiteral("side by side full timing")), uint64_t(blankingKept ? 1 : 0));

    // the 3D metadata only with a picture and depth structure, as the depth is coded
    const auto blobId = valueOf(connector.get(), QStringLiteral("HDMI 3D metadata"));
    QVERIFY(blobId);
    if (!isPictureAndDepth(layout)) {
        QCOMPARE(*blobId, uint64_t(0));
        return;
    }
    QVERIFY(*blobId != 0);
    // the commit holds the blob, the kernel keeps its own reference afterwards
    const QByteArray blob = mockGpu->blobContents.value(*blobId);
    QCOMPARE(blob.size(), 32);
    const auto *bytes = reinterpret_cast<const uint8_t *>(blob.constData());
    QCOMPARE(bytes[0], uint8_t(0)); // 3D_Metadata_type: parallax information (Table H-4)
    QCOMPARE(bytes[1], uint8_t(8)); // 3D_Metadata_Length
    const StereoDepthCoding coding(QSize(info.hdisplay, info.vdisplay), QSize(1600, 900), viewingDistance);
    const auto expected = coding.metadata();
    QVERIFY(std::memcmp(bytes + 2, expected.data(), expected.size()) == 0);
    QCOMPARE(int(bytes[6] << 8 | bytes[7]), viewingDistance ? int(viewingDistance) : 270);
    QCOMPARE(int(bytes[8] << 8 | bytes[9]), 160);
}

int main(int argc, char **argv)
{
    TestApplication app(argc, argv);
    MockDepthStructuresTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mockDepthStructuresTest.moc"
