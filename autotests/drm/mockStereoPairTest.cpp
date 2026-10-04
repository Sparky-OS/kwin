/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/outputconfiguration.h"
#include "core/session.h"
#include "main.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_crtc.h"
#include "drm_egl_layer.h"
#include "drm_gpu.h"
#include "drm_layer.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_plane.h"
#include "qpainter/qpainterbackend.h"

#include <QTest>

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

class PairLayer : public DrmPipelineLayer
{
public:
    PairLayer(DrmPlane *plane, std::shared_ptr<DrmFramebuffer> framebuffer)
        : DrmPipelineLayer(plane)
        , m_framebuffer(std::move(framebuffer))
    {
    }

    std::shared_ptr<DrmFramebuffer> currentBuffer() const override
    {
        return m_sharedBuffer ? m_sharedBuffer : m_framebuffer;
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

class MockStereoPairTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void atomicCommitReadsEachEye_data();
    void atomicCommitReadsEachEye();
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
                return std::make_unique<MockGpu>(fd, device->nodes[DRM_NODE_RENDER], 2);
            }
        }
    }
    return nullptr;
}

void MockStereoPairTest::atomicCommitReadsEachEye_data()
{
    QTest::addColumn<StereoPairRole>("firstRole");
    QTest::addColumn<StereoPairRole>("secondRole");
    QTest::addColumn<StereoPairReflection>("secondReflection");
    QTest::addRow("dual projection") << StereoPairRole::Left << StereoPairRole::Right << StereoPairReflection::None;
    QTest::addRow("mirror rig") << StereoPairRole::Left << StereoPairRole::Right << StereoPairReflection::Horizontal;
}

void MockStereoPairTest::atomicCommitReadsEachEye()
{
    QFETCH(StereoPairRole, firstRole);
    QFETCH(StereoPairRole, secondRole);
    QFETCH(StereoPairReflection, secondReflection);

    GpuManager::s_self = std::make_unique<GpuManager>();
    const auto mockGpu = findRenderDevice();
    QVERIFY(mockGpu);
    const auto connectorA = std::make_shared<MockConnector>(mockGpu.get());
    connectorA->addMode(1920, 1080, 60);
    const auto connectorB = std::make_shared<MockConnector>(mockGpu.get());
    connectorB->addMode(1920, 1080, 60);
    mockGpu->connectors = {connectorA, connectorB};

    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    const auto renderBackend = backend->createQPainterBackend();
    Q_UNUSED(renderBackend);
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    const auto drmConnectorA = std::make_shared<DrmConnector>(gpu.get(), connectorA->id);
    const auto drmConnectorB = std::make_shared<DrmConnector>(gpu.get(), connectorB->id);
    QVERIFY(drmConnectorA->init());
    QVERIFY(drmConnectorB->init());
    auto pipelineA = std::make_unique<DrmPipeline>(drmConnectorA.get());
    auto pipelineB = std::make_unique<DrmPipeline>(drmConnectorB.get());
    auto outputA = std::make_unique<DrmOutput>(drmConnectorA, pipelineA.get());
    auto outputB = std::make_unique<DrmOutput>(drmConnectorB, pipelineB.get());
    auto planeA = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes[0]->id);
    auto planeB = std::make_unique<DrmPlane>(gpu.get(), mockGpu->planes[1]->id);
    const auto addRotationProperty = [](MockPlane *plane) {
        plane->props << MockProperty(plane, QStringLiteral("rotation"), 0, DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_BITMASK,
                                      {QByteArrayLiteral("rotate-0"), QByteArrayLiteral("rotate-90"), QByteArrayLiteral("rotate-180"),
                                       QByteArrayLiteral("rotate-270"), QByteArrayLiteral("reflect-x"), QByteArrayLiteral("reflect-y")});
    };
    addRotationProperty(mockGpu->planes[0].get());
    addRotationProperty(mockGpu->planes[1].get());
    QVERIFY(planeA->init());
    QVERIFY(planeB->init());
    auto crtcA = std::make_unique<DrmCrtc>(gpu.get(), mockGpu->crtcs[0]->id, 0, planeA.get());
    auto crtcB = std::make_unique<DrmCrtc>(gpu.get(), mockGpu->crtcs[1]->id, 1, planeB.get());
    QVERIFY(crtcA->init());
    QVERIFY(crtcB->init());
    pipelineA->setCrtc(crtcA.get());
    pipelineB->setCrtc(crtcB.get());
    pipelineA->setMode(drmConnectorA->modes().front());
    pipelineB->setMode(drmConnectorB->modes().front());
    pipelineA->setEnable(true);
    pipelineB->setEnable(true);
    pipelineA->setActive(true);
    pipelineB->setActive(true);

    auto *mockFb = new MockFb(mockGpu.get(), 3840, 1080);
    auto framebufferData = std::make_shared<DrmFramebufferData>(gpu.get(), mockFb->id, nullptr);
    const auto framebuffer = std::make_shared<DrmFramebuffer>(framebufferData, nullptr, FileDescriptor{});
    auto layerA = std::make_unique<PairLayer>(planeA.get(), framebuffer);
    auto layerB = std::make_unique<PairLayer>(planeB.get(), framebuffer);
    for (PairLayer *layer : {layerA.get(), layerB.get()}) {
        layer->setEnabled(true);
        layer->setSourceRect(Rect(QPoint(0, 0), QSize(3840, 1080)));
        layer->setTargetRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    }
    pipelineA->setLayers({layerA.get()});
    pipelineB->setLayers({layerB.get()});

    OutputConfiguration configuration;
    const auto setPair = [&](DrmOutput *output, const QString &uuid, const QString &partner, StereoPairRole role, StereoPairReflection reflection) {
        const auto change = configuration.changeSet(output);
        change->uuid = uuid;
        change->stereoPartner = partner;
        change->stereoPairMode = StereoPairMode::MirrorRig;
        change->stereoPairRole = role;
        change->stereoPairReflection = reflection;
        output->applyChanges(configuration);
    };
    setPair(outputA.get(), QStringLiteral("A"), QStringLiteral("B"), firstRole, StereoPairReflection::None);
    setPair(outputB.get(), QStringLiteral("B"), QStringLiteral("A"), secondRole, secondReflection);
    QVERIFY(outputA->isStereoPair());
    QVERIFY(outputB->isStereoPair());

    mockGpu->atomicCommits.clear();
    const auto commitResult = DrmPipeline::commitPipelines({pipelineA.get(), pipelineB.get()}, gpu.get(), DrmPipeline::CommitMode::CommitModeset);
    QCOMPARE(commitResult, DrmPipeline::Error::None);
    QVERIFY(!mockGpu->atomicCommits.isEmpty());
    const auto &properties = mockGpu->atomicCommits.constLast();
    const auto sourceX = [&](uint32_t planeId) {
        const auto prop = mockGpu->findPlane(planeId)->getPropId(QStringLiteral("SRC_X"));
        const auto it = std::ranges::find_if(properties, [planeId, prop](const Prop &value) {
            return value.obj == planeId && value.prop == prop;
        });
        return it == properties.end() ? uint64_t(-1) : it->value;
    };
    QCOMPARE(sourceX(planeA->id()), uint64_t(0));
    QCOMPARE(sourceX(planeB->id()), uint64_t(1920) << 16);

    const auto fbProperty = [&](uint32_t planeId) {
        const auto prop = mockGpu->findPlane(planeId)->getPropId(QStringLiteral("FB_ID"));
        const auto it = std::ranges::find_if(properties, [planeId, prop](const Prop &value) {
            return value.obj == planeId && value.prop == prop;
        });
        return it == properties.end() ? uint64_t(0) : it->value;
    };
    QCOMPARE(fbProperty(planeA->id()), fbProperty(planeB->id()));

    const auto rotationProperty = [&](uint32_t planeId) {
        const auto prop = mockGpu->findPlane(planeId)->getPropId(QStringLiteral("rotation"));
        const auto it = std::ranges::find_if(properties, [planeId, prop](const Prop &value) {
            return value.obj == planeId && value.prop == prop;
        });
        return it == properties.end() ? uint64_t(0) : it->value;
    };
    QCOMPARE(rotationProperty(planeA->id()), uint64_t(1));
    QCOMPARE(rotationProperty(planeB->id()), secondReflection == StereoPairReflection::Horizontal ? uint64_t(36) : uint64_t(1));

    gpu.reset();
    GpuManager::s_self.reset();
}

int main(int argc, char **argv)
{
    TestApplication app(argc, argv);
    MockStereoPairTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mockStereoPairTest.moc"
