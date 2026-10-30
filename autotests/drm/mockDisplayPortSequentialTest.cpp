/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/session.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_connector.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_layer.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_plane.h"
#include "drm_render_backend.h"
#include "drm_stereo.h"

#include <QScopeGuard>
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

// a primary plane's layer whose renderer, like Mesa's, picks a tiled modifier wherever the layer allows one
class AllocatingLayer : public DrmPipelineLayer
{
public:
    AllocatingLayer(DrmPlane *plane, MockGpu *mockGpu)
        : DrmPipelineLayer(plane)
        , m_mockGpu(mockGpu)
    {
    }

    bool preparePresentationTest() override
    {
        const ModifierList modifiers = supportedDrmFormats().value(DRM_FORMAT_XRGB8888);
        if (modifiers.isEmpty()) {
            return false;
        }
        const auto tiled = std::ranges::find_if(modifiers, [](uint64_t modifier) {
            return modifier != DRM_FORMAT_MOD_LINEAR;
        });
        m_modifier = tiled != modifiers.end() ? *tiled : modifiers.front();
        auto *const mockFb = new MockFb(m_mockGpu, 3840, 1080, m_modifier);
        m_framebuffer = std::make_shared<DrmFramebuffer>(std::make_shared<DrmFramebufferData>(m_plane->gpu(), mockFb->id, nullptr), nullptr, FileDescriptor{});
        return true;
    }

    std::shared_ptr<DrmFramebuffer> currentBuffer() const override
    {
        return m_framebuffer;
    }

    void releaseBuffers() override
    {
        m_framebuffer.reset();
    }

    uint64_t modifier() const
    {
        return m_modifier;
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
    MockGpu *const m_mockGpu;
    std::shared_ptr<DrmFramebuffer> m_framebuffer;
    uint64_t m_modifier = DRM_FORMAT_MOD_INVALID;
};

class AllocatingRenderBackend : public DrmRenderBackend
{
public:
    explicit AllocatingRenderBackend(MockGpu *mockGpu)
        : m_mockGpu(mockGpu)
    {
    }

    std::unique_ptr<DrmPipelineLayer> createDrmPlaneLayer(DrmPlane *plane) override
    {
        return std::make_unique<AllocatingLayer>(plane, m_mockGpu);
    }

    std::unique_ptr<DrmPipelineLayer> createDrmPlaneLayer(DrmGpu *, DrmPlane::TypeIndex) override
    {
        return nullptr;
    }

    std::unique_ptr<DrmOutputLayer> createLayer(DrmVirtualOutput *) override
    {
        return nullptr;
    }

private:
    MockGpu *const m_mockGpu;
};

/**
 * DisplayPort's own frame sequential stereo (DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL of the edition kernel):
 * the kernel alternates the halves of one frame of both eyes and signals the eye, so KWin lists the
 * mode, offers the twin of a 2D mode only where a TEST_ONLY commit takes it, commits the whole frame
 * every time and paces pairs at half the refresh.
 */
class MockDisplayPortSequentialTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();
    void testLayout();
    void testDeclaredMode();
    void testTwin();
    void testProbe_data();
    void testProbe();
    void testScanout();
    void testLinearFallback_data();
    void testLinearFallback();

private:
    struct Rig
    {
        std::unique_ptr<MockGpu> mockGpu;
        std::shared_ptr<MockConnector> mockConnector;
        std::unique_ptr<Session> session;
        std::unique_ptr<DrmBackend> backend;
        std::unique_ptr<DrmGpu> gpu;
        std::shared_ptr<DrmConnector> connector;
        std::unique_ptr<DrmPlane> plane;
        std::unique_ptr<DrmCrtc> crtc;
    };
    std::unique_ptr<Rig> makeRig(const std::function<void(MockConnector *)> &modes, bool allowed);
    std::shared_ptr<DrmFramebuffer> makeFramebuffer(Rig *rig, uint32_t width, uint32_t height);
    std::optional<drmModeModeInfo> committedMode(Rig *rig) const;
    std::optional<uint64_t> committedPlaneProperty(Rig *rig, const QString &name) const;
};

void MockDisplayPortSequentialTest::init()
{
    GpuManager::s_self = std::make_unique<GpuManager>();
}

void MockDisplayPortSequentialTest::cleanup()
{
    GpuManager::s_self.reset();
}

std::unique_ptr<MockDisplayPortSequentialTest::Rig> MockDisplayPortSequentialTest::makeRig(const std::function<void(MockConnector *)> &modes, bool allowed)
{
    auto rig = std::make_unique<Rig>();
    rig->mockGpu = findRenderDevice(1);
    if (!rig->mockGpu) {
        return nullptr;
    }
    rig->mockConnector = std::make_shared<MockConnector>(rig->mockGpu.get());
    rig->mockConnector->modes.clear();
    modes(rig->mockConnector.get());
    rig->mockConnector->frameSequentialAllowed = allowed;
    rig->mockGpu->connectors.push_back(rig->mockConnector);
    rig->session = Session::create(Session::Type::Noop);
    rig->backend = std::make_unique<DrmBackend>(rig->session.get());
    rig->gpu = std::make_unique<DrmGpu>(rig->backend.get(), rig->mockGpu->fd, DrmDevice::open(rig->mockGpu->devNode));
    rig->connector = std::make_shared<DrmConnector>(rig->gpu.get(), rig->mockConnector->id);
    if (!rig->connector->init()) {
        return nullptr;
    }
    rig->plane = std::make_unique<DrmPlane>(rig->gpu.get(), rig->mockGpu->planes.front()->id);
    if (!rig->plane->init()) {
        return nullptr;
    }
    rig->crtc = std::make_unique<DrmCrtc>(rig->gpu.get(), rig->mockGpu->crtcs.front()->id, 0, rig->plane.get());
    if (!rig->crtc->init()) {
        return nullptr;
    }
    return rig;
}

std::shared_ptr<DrmFramebuffer> MockDisplayPortSequentialTest::makeFramebuffer(Rig *rig, uint32_t width, uint32_t height)
{
    auto *const mockFb = new MockFb(rig->mockGpu.get(), width, height);
    auto data = std::make_shared<DrmFramebufferData>(rig->gpu.get(), mockFb->id, nullptr);
    return std::make_shared<DrmFramebuffer>(data, nullptr, FileDescriptor{});
}

std::optional<drmModeModeInfo> MockDisplayPortSequentialTest::committedMode(Rig *rig) const
{
    if (rig->mockGpu->atomicCommits.isEmpty()) {
        return std::nullopt;
    }
    const auto mockCrtc = rig->mockGpu->findCrtc(rig->crtc->id());
    const auto modeId = mockCrtc->getPropId(QStringLiteral("MODE_ID"));
    for (const Prop &property : rig->mockGpu->atomicCommits.constLast()) {
        if (property.obj == mockCrtc->id && property.prop == modeId) {
            const QByteArray bytes = rig->mockGpu->blobContents.value(property.value);
            if (bytes.size() != sizeof(drmModeModeInfo)) {
                return std::nullopt;
            }
            drmModeModeInfo info;
            memcpy(&info, bytes.constData(), sizeof(info));
            return info;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> MockDisplayPortSequentialTest::committedPlaneProperty(Rig *rig, const QString &name) const
{
    if (rig->mockGpu->atomicCommits.isEmpty()) {
        return std::nullopt;
    }
    const auto mockPlane = rig->mockGpu->findPlane(rig->plane->id());
    const auto id = mockPlane->getPropId(name);
    for (const Prop &property : rig->mockGpu->atomicCommits.constLast()) {
        if (property.obj == mockPlane->id && property.prop == id) {
            return property.value;
        }
    }
    return std::nullopt;
}

void MockDisplayPortSequentialTest::testLayout()
{
    // the kernel's layout is its own 3D mode, never one of the twins KWin alternates itself
    QCOMPARE(stereoFlagsForDrmMode(DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL), OutputModeline::Flags(OutputModeline::Flag::Stereo3DSequentialDisplayPort));
    QCOMPARE(DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL, 9 << 14);
    QVERIFY(OutputModeline::AllStereo3D.testFlag(OutputModeline::Flag::Stereo3DSequentialDisplayPort));
    QCOMPARE(stereoLayoutForMode(OutputModeline::Flag::Stereo3DSequentialDisplayPort), StereoLayout::SequentialDisplayPort);
    QCOMPARE(stereoLayoutForMode(OutputModeline::Flag::Stereo3DSequentialDisplayPort | OutputModeline::Flag::VirtualStereo), StereoLayout::SequentialDisplayPort);
    QVERIFY(isDisplayPortFrameSequential(StereoLayout::SequentialDisplayPort));
    QVERIFY(!isFrameSequential(StereoLayout::SequentialDisplayPort));
    QVERIFY(!isDisplayPortFrameSequential(StereoLayout::SequentialLeftFirst));
    QVERIFY(isFullFrameStereo(StereoLayout::SequentialDisplayPort));

    drmModeModeInfo mode{};
    mode.hdisplay = 1920;
    mode.vdisplay = 1080;
    mode.vtotal = 1125;
    // the frame of both eyes, side by side, as drm_mode_get_hv_timing() sizes it for this layout
    QCOMPARE(stereoFrameSize(mode, StereoLayout::SequentialDisplayPort, OutputModeline::Flag::Stereo3DSequentialDisplayPort), QSize(3840, 1080));
    QCOMPARE(stereoRightEyeOffset(mode, StereoLayout::SequentialDisplayPort), QPoint(1920, 0));
    QCOMPARE(stereoEyeSize(mode, OutputModeline::Flag::Stereo3DSequentialDisplayPort | OutputModeline::Flag::VirtualStereo), QSize(1920, 1080));

    // a pair per two refreshes; every other layout presents once per refresh
    QCOMPARE(stereoPresentationRate(120000, StereoLayout::SequentialDisplayPort), 60000u);
    QCOMPARE(stereoPresentationRate(119880, StereoLayout::SequentialDisplayPort), 59940u);
    QCOMPARE(stereoPresentationRate(120000, StereoLayout::SequentialLeftFirst), 120000u);
    QCOMPARE(stereoPresentationRate(60000, StereoLayout::None), 60000u);
}

void MockDisplayPortSequentialTest::testDeclaredMode()
{
    // a display that declares frame sequential stereo: the kernel lists the 2D mode and its copy with the flag
    const auto rig = makeRig([](MockConnector *connector) {
        connector->addMode(1920, 1080, 120, true);
        drmModeModeInfo sequential = connector->modes.constLast();
        sequential.flags |= DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL;
        sequential.type &= ~DRM_MODE_TYPE_PREFERRED;
        connector->modes.push_back(sequential);
    }, true);
    QVERIFY(rig);
    const auto modes = rig->connector->modes();
    QCOMPARE(modes.size(), 2);
    QCOMPARE(stereoLayoutForMode(modes[0]->flags()), StereoLayout::None);
    QCOMPARE(stereoLayoutForMode(modes[1]->flags()), StereoLayout::SequentialDisplayPort);
    QVERIFY(!(modes[1]->flags() & OutputModeline::Flag::VirtualStereo));
    QVERIFY(!(modes[1]->flags() & OutputModeline::Flag::Preferred));
    QCOMPARE(modes[1]->size(), QSize(1920, 1080));

    // the display's own 3D mode stands in for the twin of the same timing
    modes[0]->setFrameSequentialAccepted(true);
    const QList<std::shared_ptr<OutputMode>> base{modes[0], modes[1]};
    const auto listed = DrmConnector::withVirtualStereoModes(base, {}, false, true);
    const auto sequentialTwins = std::ranges::count_if(listed, [](const auto &mode) {
        return stereoLayoutForMode(mode->flags()) == StereoLayout::SequentialDisplayPort && (mode->flags() & OutputModeline::Flag::VirtualStereo);
    });
    QCOMPARE(sequentialTwins, 0);
}

void MockDisplayPortSequentialTest::testTwin()
{
    const auto rig = makeRig([](MockConnector *connector) {
        connector->addMode(1920, 1080, 60, true);
        connector->addMode(1920, 1080, 120);
    }, true);
    QVERIFY(rig);
    const auto modes = rig->connector->modes();
    QCOMPARE(modes.size(), 2);
    const auto sixty = modes[0]->refreshRate() < modes[1]->refreshRate() ? modes[0] : modes[1];
    const auto hundredTwenty = sixty == modes[0] ? modes[1] : modes[0];
    const QList<std::shared_ptr<OutputMode>> base{modes[0], modes[1]};
    const auto countTwins = [](const QList<std::shared_ptr<OutputMode>> &listed) -> std::optional<std::shared_ptr<DrmConnectorMode>> {
        std::optional<std::shared_ptr<DrmConnectorMode>> found;
        for (const auto &mode : listed) {
            if (stereoLayoutForMode(mode->flags()) == StereoLayout::SequentialDisplayPort) {
                if (found) {
                    return std::nullopt;
                }
                found = std::static_pointer_cast<DrmConnectorMode>(mode);
            }
        }
        return found;
    };

    // untested and refused modes have no twin, nor does any mode with "Other stereo formats" off
    QVERIFY(!countTwins(DrmConnector::withVirtualStereoModes(base, {}, false, true)));
    hundredTwenty->setFrameSequentialAccepted(false);
    QVERIFY(!countTwins(DrmConnector::withVirtualStereoModes(base, {}, false, true)));
    hundredTwenty->setFrameSequentialAccepted(true);
    QVERIFY(!countTwins(DrmConnector::withVirtualStereoModes(base, {}, true, false)));
    // the software twins stay, as their own options
    const auto listed = DrmConnector::withVirtualStereoModes(base, {}, false, true);
    QVERIFY(std::ranges::any_of(listed, [](const auto &mode) {
        return mode->flags() & OutputModeline::Flag::Stereo3DSequentialLeftFirst;
    }));
    QVERIFY(std::ranges::any_of(listed, [](const auto &mode) {
        return mode->flags() & OutputModeline::Flag::Stereo3DSequentialRightFirst;
    }));

    const auto twin = countTwins(listed);
    QVERIFY(twin);
    QCOMPARE((*twin)->refreshRate(), hundredTwenty->refreshRate());
    QCOMPARE((*twin)->flags() & ~OutputModeline::Flags(OutputModeline::Flag::Generated),
             OutputModeline::Flags(OutputModeline::Flag::VirtualStereo | OutputModeline::Flag::Stereo3DSequentialDisplayPort));
    QCOMPARE((*twin)->virtualBase(), hundredTwenty);
    // the kernel sends this twin: the base's 2D timing with the frame sequential flag, as a blob of its own
    QCOMPARE((*twin)->nativeMode()->flags & DRM_MODE_FLAG_3D_MASK, uint32_t(DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL));
    QCOMPARE((*twin)->nativeMode()->hdisplay, hundredTwenty->nativeMode()->hdisplay);
    QCOMPARE((*twin)->nativeMode()->clock, hundredTwenty->nativeMode()->clock);
    QCOMPARE(hundredTwenty->nativeMode()->flags & DRM_MODE_FLAG_3D_MASK, 0u);
    QVERIFY((*twin)->blob() != hundredTwenty->blob());
    // KWin's own twins are sent as their 2D mode
    const auto softwareTwin = std::make_shared<DrmConnectorMode>(hundredTwenty, OutputModeline::Flag::Stereo3DSequentialLeftFirst);
    QCOMPARE(softwareTwin->blob(), hundredTwenty->blob());
    // a 60 Hz mode gets no frame sequential twin of either kind: each eye would be at 30 Hz
    sixty->setFrameSequentialAccepted(true);
    QVERIFY(!std::ranges::any_of(DrmConnector::withVirtualStereoModes(base, {}, false, true), [&sixty](const auto &mode) {
        return mode->refreshRate() == sixty->refreshRate() && stereoLayoutForMode(mode->flags()) != StereoLayout::None
            && (mode->flags() & (OutputModeline::Flag::Stereo3DSequentialDisplayPort | OutputModeline::Flag::Stereo3DSequentialLeftFirst));
    }));
}

void MockDisplayPortSequentialTest::testProbe_data()
{
    QTest::addColumn<bool>("allowed");
    QTest::addRow("connector opted in") << true;
    QTest::addRow("connector without opt-in") << false;
}

void MockDisplayPortSequentialTest::testProbe()
{
    QFETCH(bool, allowed);
    const auto rig = makeRig([](MockConnector *connector) {
        connector->addMode(1920, 1080, 120, true);
    }, allowed);
    QVERIFY(rig);
    const auto base = rig->connector->modes().front();
    const auto twin = std::make_shared<DrmConnectorMode>(base, OutputModeline::Flag::Stereo3DSequentialDisplayPort);
    const auto frame = makeFramebuffer(rig.get(), 3840, 1080);
    rig->mockGpu->atomicCommits.clear();
    QCOMPARE(rig->connector->testFrameSequential(rig->crtc.get(), twin, frame), allowed);
    QCOMPARE(rig->mockGpu->atomicCommits.size(), 1);
    // the test carried the flagged mode and a plane over the frame of both eyes
    const auto mode = committedMode(rig.get());
    QVERIFY(mode);
    QCOMPARE(mode->flags & DRM_MODE_FLAG_3D_MASK, uint32_t(DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL));
    QCOMPARE(mode->hdisplay, 1920);
    QCOMPARE(committedPlaneProperty(rig.get(), QStringLiteral("SRC_W")), std::optional<uint64_t>(uint64_t(3840) << 16));
    QCOMPARE(committedPlaneProperty(rig.get(), QStringLiteral("CRTC_W")), std::optional<uint64_t>(3840));
    QCOMPARE(committedPlaneProperty(rig.get(), QStringLiteral("CRTC_H")), std::optional<uint64_t>(1080));
    // nothing was applied
    QVERIFY(!rig->mockGpu->findCrtc(rig->crtc->id())->getProp(QStringLiteral("ACTIVE")));
    // a 2D mode is never sent through the frame sequential test
    QVERIFY(!rig->connector->testFrameSequential(rig->crtc.get(), base, frame));
}

void MockDisplayPortSequentialTest::testScanout()
{
    const auto rig = makeRig([](MockConnector *connector) {
        connector->addMode(1920, 1080, 120, true);
    }, true);
    QVERIFY(rig);
    const auto base = rig->connector->modes().front();
    const auto twin = std::make_shared<DrmConnectorMode>(base, OutputModeline::Flag::Stereo3DSequentialDisplayPort);
    auto pipeline = std::make_unique<DrmPipeline>(rig->connector.get());
    pipeline->setCrtc(rig->crtc.get());
    pipeline->setMode(twin);
    pipeline->setEnable(true);
    pipeline->setActive(true);
    const auto frame = makeFramebuffer(rig.get(), 3840, 1080);
    auto layer = std::make_unique<TestLayer>(rig->plane.get(), frame);
    layer->setEnabled(true);
    layer->setTargetRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    layer->setSourceRect(Rect(QPoint(0, 0), QSize(1920, 1080)));
    pipeline->setLayers({layer.get()});

    // every commit hands the kernel the whole frame of both eyes; the kernel picks the half per refresh
    for (int i = 0; i < 4; i++) {
        rig->mockGpu->atomicCommits.clear();
        QCOMPARE(DrmPipeline::commitPipelines({pipeline.get()}, rig->gpu.get(), DrmPipeline::CommitMode::CommitModeset), DrmPipeline::Error::None);
        QCOMPARE(committedPlaneProperty(rig.get(), QStringLiteral("SRC_X")), std::optional<uint64_t>(0));
        QCOMPARE(committedPlaneProperty(rig.get(), QStringLiteral("SRC_W")), std::optional<uint64_t>(uint64_t(3840) << 16));
        QCOMPARE(committedPlaneProperty(rig.get(), QStringLiteral("CRTC_W")), std::optional<uint64_t>(3840));
        const auto mode = committedMode(rig.get());
        QVERIFY(mode);
        QCOMPARE(mode->flags & DRM_MODE_FLAG_3D_MASK, uint32_t(DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL));
        // flip events come only at pair boundaries, so each one is a new pair, never an eye to toggle
        pipeline->pageFlipped(std::chrono::nanoseconds(i + 1), 100 + 2 * i, std::nullopt);
        QVERIFY(pipeline->frameSequentialNeedsNewFrame());
    }

    // the same mode on a connector that never opted in is refused
    rig->mockConnector->frameSequentialAllowed = false;
    QVERIFY(DrmPipeline::commitPipelines({pipeline.get()}, rig->gpu.get(), DrmPipeline::CommitMode::TestAllowModeset) != DrmPipeline::Error::None);
}

void MockDisplayPortSequentialTest::testLinearFallback_data()
{
    QTest::addColumn<bool>("linearOffered");
    QTest::addRow("plane offers linear") << true;
    QTest::addRow("plane offers no linear") << false;
}

void MockDisplayPortSequentialTest::testLinearFallback()
{
    // amdgpu shows DisplayPort's frame sequential stereo only from a linear framebuffer (edition kernel
    // series 39), and its probe passes with a dumb buffer, which is linear, while the renderer picks a
    // tiled buffer for the output. The configuration's test fails with that buffer, so it is tested
    // again with linear buffers on that output before the mode is given up. A 2D mode on the same
    // output, and the other output, keep the renderer's tiled buffers.
    QFETCH(bool, linearOffered);
    const auto mockGpu = findRenderDevice(2);
    QVERIFY(mockGpu);
    mockGpu->frameSequentialNeedsLinear = true;
    const uint64_t tiled = AMD_FMT_MOD | AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) | AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S_X);
    const QList<uint64_t> modifiers = linearOffered ? QList<uint64_t>{tiled, DRM_FORMAT_MOD_LINEAR} : QList<uint64_t>{tiled};
    for (const auto &plane : std::as_const(mockGpu->planes)) {
        plane->setFormats({{DRM_FORMAT_XRGB8888, modifiers}});
    }
    const auto sequentialConnector = std::make_shared<MockConnector>(mockGpu.get());
    sequentialConnector->modes.clear();
    sequentialConnector->addMode(1920, 1080, 120, true);
    sequentialConnector->frameSequentialAllowed = true;
    const auto flatConnector = std::make_shared<MockConnector>(mockGpu.get());
    mockGpu->connectors.push_back(sequentialConnector);
    mockGpu->connectors.push_back(flatConnector);

    AllocatingRenderBackend renderBackend(mockGpu.get());
    const auto session = Session::create(Session::Type::Noop);
    const auto backend = std::make_unique<DrmBackend>(session.get());
    backend->setRenderBackend(&renderBackend);
    auto gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    QVERIFY(gpu->atomicModeSetting());
    QVERIFY(gpu->updateOutputs());
    gpu->createLayers();
    QCOMPARE(gpu->drmOutputs().size(), 2);
    DrmPipeline *sequential = nullptr;
    DrmPipeline *flat = nullptr;
    for (DrmOutput *output : gpu->drmOutputs()) {
        (output->connector()->id() == sequentialConnector->id ? sequential : flat) = output->pipeline();
    }
    QVERIFY(sequential && flat);
    const auto primaryModifier = [](DrmPipeline *pipeline) {
        return static_cast<AllocatingLayer *>(pipeline->layers().front())->modifier();
    };

    const auto base = sequential->connector()->modes().front();
    const auto twin = std::make_shared<DrmConnectorMode>(base, OutputModeline::Flag::Stereo3DSequentialDisplayPort);
    for (DrmPipeline *pipeline : {sequential, flat}) {
        pipeline->setEnable(true);
        pipeline->setActive(true);
    }
    // both outputs in 2D: the renderer's tiled buffers
    QCOMPARE(gpu->testPendingConfiguration(), DrmPipeline::Error::None);
    QCOMPARE(primaryModifier(sequential), tiled);
    QCOMPARE(primaryModifier(flat), tiled);

    sequential->setMode(twin);
    if (!linearOffered) {
        // nothing the plane offers can show it: the mode is refused, as the kernel refuses it
        QVERIFY(gpu->testPendingConfiguration() != DrmPipeline::Error::None);
        gpu.reset();
        return;
    }
    QCOMPARE(gpu->testPendingConfiguration(), DrmPipeline::Error::None);
    QCOMPARE(primaryModifier(sequential), uint64_t(DRM_FORMAT_MOD_LINEAR));
    QCOMPARE(primaryModifier(flat), tiled);
    QCOMPARE(stereoLayoutForMode(sequential->mode()->flags()), StereoLayout::SequentialDisplayPort);

    // back in 2D the output gets the renderer's choice again
    sequential->setMode(base);
    QCOMPARE(gpu->testPendingConfiguration(), DrmPipeline::Error::None);
    QCOMPARE(primaryModifier(sequential), tiled);
    QCOMPARE(primaryModifier(flat), tiled);
    gpu.reset();
}

QTEST_GUILESS_MAIN(MockDisplayPortSequentialTest)
#include "mockDisplayPortSequentialTest.moc"
