/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/outputconfiguration.h"
#include "core/renderloop.h"
#include "core/session.h"
#include "main.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_commit.h"
#include "drm_commit_thread.h"
#include "drm_crtc.h"
#include "drm_egl_layer.h"
#include "drm_gpu.h"
#include "drm_layer.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_plane.h"
#include "qpainter/qpainterbackend.h"

#include <QScopeGuard>
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
    void pairAcrossGpusShowsEachEye();
    void unpairingReleasesWaitingFrame();
    void removedPartnerReleasesWaitingFrame();
    void pairCommitUsesOneCommitThread();
    void pairPageflipKeepsPartnerCommit();
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
    QTest::addColumn<bool>("firstSwap");
    QTest::addColumn<bool>("secondSwap");
    for (auto reflection : {StereoPairReflection::None, StereoPairReflection::Horizontal, StereoPairReflection::Vertical}) {
        for (bool firstSwap : {false, true}) {
            for (bool secondSwap : {false, true}) {
                QTest::addRow("reflection-%d-swap-%d-%d", int(reflection), firstSwap, secondSwap)
                    << StereoPairRole::Left << StereoPairRole::Right << reflection << firstSwap << secondSwap;
            }
        }
    }
}

void MockStereoPairTest::atomicCommitReadsEachEye()
{
    QFETCH(StereoPairRole, firstRole);
    QFETCH(StereoPairRole, secondRole);
    QFETCH(StereoPairReflection, secondReflection);
    QFETCH(bool, firstSwap);
    QFETCH(bool, secondSwap);

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
    // the GPU is destroyed after what holds its buffers, which are declared after it, then the manager goes
    const auto resetManager = qScopeGuard([] {
        GpuManager::s_self.reset();
    });
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
        change->stereoPairMode = secondReflection == StereoPairReflection::None ? StereoPairMode::DualProjection : StereoPairMode::MirrorRig;
        change->stereoPairRole = role;
        change->stereoPairReflection = reflection;
        change->stereoSwapEyes = output == outputA.get() ? firstSwap : secondSwap;
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
    QCOMPARE(sourceX(planeA->id()), uint64_t(firstSwap ? 1920 : 0) << 16);
    QCOMPARE(sourceX(planeB->id()), uint64_t(secondSwap ? 0 : 1920) << 16);

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
    QCOMPARE(rotationProperty(planeB->id()), secondReflection == StereoPairReflection::Horizontal ? uint64_t(36)
                                             : secondReflection == StereoPairReflection::Vertical ? uint64_t(33) : uint64_t(1));

}

// Two outputs paired as left and right eye, on one GPU or on two, with a mode set and a layer each.
class PairRig
{
public:
    explicit PairRig(bool twoGpus)
    {
        mockGpus[0] = findRenderDevice();
        mockGpus[1] = twoGpus ? findRenderDevice() : nullptr;
        if (!mockGpus[0] || (twoGpus && !mockGpus[1])) {
            return;
        }
        for (int i = 0; i < 2; i++) {
            MockGpu *mockGpu = mockGpus[twoGpus ? i : 0].get();
            connectors[i] = std::make_shared<MockConnector>(mockGpu);
            connectors[i]->addMode(1920, 1080, 60);
            mockGpu->connectors.push_back(connectors[i]);
        }
        session = Session::create(Session::Type::Noop);
        backend = std::make_unique<DrmBackend>(session.get());
        renderBackend = backend->createQPainterBackend();
        for (int g = 0; g < (twoGpus ? 2 : 1); g++) {
            gpus[g] = std::make_unique<DrmGpu>(backend.get(), mockGpus[g]->fd, DrmDevice::open(mockGpus[g]->devNode));
            if (!gpus[g]->updateOutputs()) {
                return;
            }
        }
        for (int i = 0; i < 2; i++) {
            const int g = twoGpus ? i : 0;
            const int pipe = twoGpus ? 0 : i;
            gpuOf[i] = gpus[g].get();
            outputs[i] = gpuOf[i]->drmOutputs().value(pipe);
            if (!outputs[i]) {
                return;
            }
            planes[i] = std::make_unique<DrmPlane>(gpuOf[i], mockGpus[g]->planes[pipe]->id);
            crtcs[i] = std::make_unique<DrmCrtc>(gpuOf[i], mockGpus[g]->crtcs[pipe]->id, pipe, planes[i].get());
            if (!planes[i]->init() || !crtcs[i]->init()) {
                return;
            }
            auto *mockFb = new MockFb(mockGpus[g].get(), 3840, 1080);
            framebuffers[i] = std::make_shared<DrmFramebuffer>(std::make_shared<DrmFramebufferData>(gpuOf[i], mockFb->id, nullptr), nullptr, FileDescriptor{});
            layers[i] = std::make_unique<PairLayer>(planes[i].get(), framebuffers[i]);
            layers[i]->setEnabled(true);
            layers[i]->setSourceRect(Rect(QPoint(0, 0), QSize(3840, 1080)));
            layers[i]->setTargetRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
            DrmPipeline *pipeline = outputs[i]->pipeline();
            pipeline->setCrtc(crtcs[i].get());
            pipeline->setMode(outputs[i]->connector()->modes().front());
            pipeline->setEnable(true);
            pipeline->setActive(true);
            pipeline->setLayers({layers[i].get()});
        }
        setPair(true);
        for (int g = 0; g < (twoGpus ? 2 : 1); g++) {
            QList<DrmPipeline *> pipelines;
            for (int i = 0; i < 2; i++) {
                if (gpuOf[i] == gpus[g].get()) {
                    pipelines.push_back(outputs[i]->pipeline());
                }
            }
            if (DrmPipeline::commitPipelines(pipelines, gpus[g].get(), DrmPipeline::CommitMode::CommitModeset) != DrmPipeline::Error::None) {
                return;
            }
            mockGpus[g]->atomicCommits.clear();
        }
        valid = true;
    }

    // pairs (or unpairs) the two outputs the way the output configuration does
    void setPair(bool paired)
    {
        OutputConfiguration configuration;
        for (int i = 0; i < 2; i++) {
            const auto change = configuration.changeSet(outputs[i]);
            change->enabled = true;
            change->uuid = i == 0 ? QStringLiteral("A") : QStringLiteral("B");
            const QString partner = i == 0 ? QStringLiteral("B") : QStringLiteral("A");
            change->stereoPartner = paired ? partner : QString();
            change->stereoPairMode = paired ? StereoPairMode::DualProjection : StereoPairMode::None;
            change->stereoPairRole = i == 0 ? StereoPairRole::Left : StereoPairRole::Right;
        }
        for (int i = 0; i < 2; i++) {
            const auto change = configuration.changeSet(outputs[i]);
            if (outputs[i]->queueChanges(change)) {
                outputs[i]->applyQueuedChanges(change);
            }
        }
    }

    std::shared_ptr<OutputFrame> frame(int i)
    {
        outputs[i]->renderLoop()->prepareNewFrame();
        return std::make_shared<OutputFrame>(outputs[i]->renderLoop(), std::chrono::nanoseconds(16'666'667));
    }

    DrmPipeline::Error present(int i, const std::shared_ptr<OutputFrame> &frame)
    {
        return gpuOf[i]->presentStereoPair(outputs[i], {layers[i].get()}, frame);
    }

    // SRC_X of output i's plane in the last atomic commit of its GPU that has it
    std::optional<uint64_t> committedSourceX(int i) const
    {
        MockGpu *mockGpu = mockGpus[gpuOf[i] == gpus[0].get() ? 0 : 1].get();
        const uint32_t prop = mockGpu->findPlane(planes[i]->id())->getPropId(QStringLiteral("SRC_X"));
        for (auto commit = mockGpu->atomicCommits.crbegin(); commit != mockGpu->atomicCommits.crend(); ++commit) {
            for (const Prop &value : *commit) {
                if (value.obj == planes[i]->id() && value.prop == prop) {
                    return value.value;
                }
            }
        }
        return std::nullopt;
    }

    struct ManagerScope
    {
        ManagerScope()
        {
            GpuManager::s_self = std::make_unique<GpuManager>();
        }
        ~ManagerScope()
        {
            GpuManager::s_self.reset();
        }
    } manager;
    bool valid = false;
    std::unique_ptr<MockGpu> mockGpus[2];
    std::unique_ptr<Session> session;
    std::unique_ptr<DrmBackend> backend;
    std::unique_ptr<QPainterBackend> renderBackend;
    std::unique_ptr<DrmGpu> gpus[2];
    DrmGpu *gpuOf[2] = {};
    DrmOutput *outputs[2] = {};
    std::unique_ptr<DrmPlane> planes[2];
    std::unique_ptr<DrmCrtc> crtcs[2];
    std::shared_ptr<DrmFramebuffer> framebuffers[2];
    std::unique_ptr<PairLayer> layers[2];
    std::shared_ptr<MockConnector> connectors[2];
};

void MockStereoPairTest::pairAcrossGpusShowsEachEye()
{
    // A pair whose outputs sit on two GPUs cannot share one atomic commit:
    // each output still has to show its own eye, and never fail to present.
    PairRig rig(true);
    QVERIFY(rig.valid);
    QVERIFY(rig.outputs[0]->isStereoPair());
    QVERIFY(rig.outputs[1]->isStereoPair());
    for (int i = 0; i < 2; i++) {
        QCOMPARE(rig.present(i, rig.frame(i)), DrmPipeline::Error::None);
    }
    QTRY_VERIFY(rig.committedSourceX(0).has_value());
    QTRY_VERIFY(rig.committedSourceX(1).has_value());
    QCOMPARE(*rig.committedSourceX(0), uint64_t(0));
    QCOMPARE(*rig.committedSourceX(1), uint64_t(1920) << 16);
}

void MockStereoPairTest::unpairingReleasesWaitingFrame()
{
    // The first output of a pair to present waits for its partner. When the
    // pair is undone before the partner presents, the waiting frame has to be
    // released, or that output never renders again.
    PairRig rig(false);
    QVERIFY(rig.valid);
    std::weak_ptr<OutputFrame> waiting;
    {
        const auto frame = rig.frame(0);
        waiting = frame;
        QCOMPARE(rig.present(0, frame), DrmPipeline::Error::None);
    }
    QVERIFY(!waiting.expired());
    rig.setPair(false);
    QVERIFY(!rig.outputs[0]->isStereoPair());
    QCOMPARE(rig.present(1, rig.frame(1)), DrmPipeline::Error::None);
    QVERIFY(waiting.expired());
}

void MockStereoPairTest::removedPartnerReleasesWaitingFrame()
{
    PairRig rig(false);
    QVERIFY(rig.valid);
    std::weak_ptr<OutputFrame> waiting;
    {
        const auto frame = rig.frame(0);
        waiting = frame;
        QCOMPARE(rig.present(0, frame), DrmPipeline::Error::None);
    }
    QVERIFY(!waiting.expired());
    rig.mockGpus[0]->connectors.removeOne(rig.connectors[1]);
    QVERIFY(rig.gpus[0]->updateOutputs());
    QCOMPARE(rig.gpus[0]->drmOutputs().size(), 1);
    QVERIFY(waiting.expired());
}

void MockStereoPairTest::pairCommitUsesOneCommitThread()
{
    // Whichever output of a pair presents first, the pair's commit has to go
    // to the same commit thread, so that the two threads never have commits
    // for the same planes in flight at once.
    PairRig rig(false);
    QVERIFY(rig.valid);
    QCOMPARE(rig.present(1, rig.frame(1)), DrmPipeline::Error::None);
    QCOMPARE(rig.present(0, rig.frame(0)), DrmPipeline::Error::None);
    QVERIFY(rig.outputs[0]->pipeline()->commitThread()->pageflipsPending());
    QVERIFY(!rig.outputs[1]->pipeline()->commitThread()->pageflipsPending());
}

void MockStereoPairTest::pairPageflipKeepsPartnerCommit()
{
    // The page flip of a pair's commit must not release a commit that the
    // partner's own commit thread has in flight.
    PairRig rig(false);
    QVERIFY(rig.valid);
    DrmPipeline *partner = rig.outputs[1]->pipeline();
    partner->commitThread()->setPendingCommit(std::make_unique<DrmLegacyCommit>(partner, rig.framebuffers[1], rig.frame(1)));
    QVERIFY(partner->commitThread()->pageflipsPending());
    auto pairCommit = std::make_unique<DrmAtomicCommit>(QList<DrmPipeline *>{rig.outputs[0]->pipeline(), partner});
    pairCommit->pageFlipped(std::chrono::steady_clock::now().time_since_epoch(), std::nullopt);
    pairCommit->pageFlipped(std::chrono::steady_clock::now().time_since_epoch(), std::nullopt);
    const bool kept = partner->commitThread()->pageflipsPending();
    // complete the partner's own commit, which releases its frame
    partner->pageFlipped(std::chrono::steady_clock::now().time_since_epoch(), std::nullopt, std::nullopt);
    QVERIFY(kept);
}

int main(int argc, char **argv)
{
    TestApplication app(argc, argv);
    MockStereoPairTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mockStereoPairTest.moc"
