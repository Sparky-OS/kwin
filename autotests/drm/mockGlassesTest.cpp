/* SPDX-License-Identifier: GPL-2.0-or-later */

// USB-C 3D glasses on a DRM connector: known glasses, by their EDID identity, come up in the side by
// side (full) twin of their two-eyes-wide 2D mode, a 1920x1080 desktop copied into each half; a user
// option, on by default for known glasses, off for every other display. The EDIDs are the XREAL One Pro's own,
// read from Windows' registry on an owner's glasses on 24 Oct 2026 (autotests/integration/data/xreal-one-pro-*.bin,
// decoded beside them): EDID 1.3, PnP ID MRG, product 0x4100, serial 0, week 49 of 2024, name "XREAL One Pro",
// the same identity in every mode. 2D lists 1920x1080 at 90 (preferred), 60 and 120 Hz; the half side by side
// mode announces the same bytes as 2D; full side by side one 3840x1080 at 60 Hz; the glasses' own Real 3D
// (a 2D to 3D conversion inside them) one 1920x1080 at 60 Hz, which is 2D to the computer.

#include "mock_drm.h"

#include "core/gpumanager.h"
#include "core/outputconfiguration.h"
#include "core/session.h"
#include "drm_backend.h"
#include "drm_connector.h"
#include "drm_gpu.h"
#include "drm_output.h"
#include "outputconfigurationstore.h"
#include "utils/edid.h"
#include "utils/orientationsensor.h"
#include "qpainter/qpainterbackend.h"

#include <QFile>
#include <QJsonArray>
#include <QScopeGuard>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTest>

#include <cstdlib>
#include <fcntl.h>

using namespace KWin;
using Flag = OutputModeline::Flag;

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

static QByteArray readEdid(const QString &name)
{
    QFile file(QFINDTESTDATA("../integration/data/" + name));
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

static void setEdid(MockGpu *gpu, MockConnector *connector, const QByteArray &edid)
{
    auto blob = std::make_unique<MockPropertyBlob>(gpu, edid.constData(), edid.size());
    connector->setProp(QStringLiteral("EDID"), blob->id);
    gpu->propertyBlobs.push_back(std::move(blob));
}

struct ModeSpec
{
    int width;
    int height;
    float refreshRate;
};

static void setModes(MockConnector *connector, const QList<ModeSpec> &modes)
{
    connector->modes.clear();
    for (int i = 0; i < modes.size(); ++i) {
        connector->addMode(modes[i].width, modes[i].height, modes[i].refreshRate, i == 0);
    }
}

// the glasses in their 3D mode: one 3840x1080 picture at 60 Hz, the only rate they offer, the halves to the eyes
static const QList<ModeSpec> s_sideBySideModes{{3840, 1080, 60.0}};
// the glasses in their 2D mode: both eyes show the same 1920x1080 picture, at 90 (the EDID's preferred timing), 60 or 120 Hz
static const QList<ModeSpec> s_flatModes{{1920, 1080, 90.0}, {1920, 1080, 60.0}, {1920, 1080, 120.0}};
// the glasses' Real 3D mode: one 1920x1080 picture at 60 Hz, which they turn into 3D themselves
static const QList<ModeSpec> s_realDModes{{1920, 1080, 60.0}};

static OutputModeline::Flags stereoFlags(const OutputModeline &mode)
{
    return mode.flags() & (OutputModeline::AllStereo3D | Flag::VirtualStereo);
}

// libxcvt times the mock's modes a little under the asked rate (59.963 Hz for 60)
static bool nearRate(const OutputModeline &mode, int milliHertz)
{
    return std::abs(int(mode.refreshRate()) - milliHertz) < 100;
}

// a mode the connector announces now; one that went away stays listed as removed
static bool hasMode(BackendOutput *output, const QSize &size)
{
    const auto modes = output->modes();
    return std::ranges::any_of(modes, [&size](const auto &mode) {
        return !mode->isRemoved() && mode->size() == size;
    });
}

static QString configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json");
}

// sets a saved per-output key on every output in kwinoutputconfig.json, as a user's choice would be saved
static void setSavedOption(const QString &key, const QJsonValue &value)
{
    QFile file(configPath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto array = QJsonDocument::fromJson(file.readAll()).array();
    file.close();
    for (int i = 0; i < array.size(); ++i) {
        auto object = array[i].toObject();
        if (object["name"].toString() != QLatin1String("outputs")) {
            continue;
        }
        auto outputs = object["data"].toArray();
        for (int j = 0; j < outputs.size(); ++j) {
            auto output = outputs[j].toObject();
            output[key] = value;
            outputs[j] = output;
        }
        object["data"] = outputs;
        array[i] = object;
    }
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(array).toJson());
}

// a GPU with one connector, the display under test
class Rig
{
public:
    explicit Rig(const QByteArray &edid, const QList<ModeSpec> &modes)
        : mockGpu(findRenderDevice(1))
        , connector(std::make_shared<MockConnector>(mockGpu.get()))
        , session(Session::create(Session::Type::Noop))
        , backend(std::make_unique<DrmBackend>(session.get()))
        , renderBackend(backend->createQPainterBackend())
    {
        setEdid(mockGpu.get(), connector.get(), edid);
        setModes(connector.get(), modes);
        mockGpu->connectors.push_back(connector);
        gpu = std::make_unique<DrmGpu>(backend.get(), mockGpu->fd, DrmDevice::open(mockGpu->devNode));
    }

    QList<BackendOutput *> outputs() const
    {
        QList<BackendOutput *> ret;
        for (DrmOutput *output : gpu->drmOutputs()) {
            ret.append(output);
        }
        return ret;
    }

    // the connector re-announces itself with another EDID and mode list, as the glasses do when their mode is switched
    void announce(const QByteArray &edid, const QList<ModeSpec> &modes)
    {
        setEdid(mockGpu.get(), connector.get(), edid);
        setModes(connector.get(), modes);
    }

    std::unique_ptr<MockGpu> mockGpu;
    std::shared_ptr<MockConnector> connector;
    std::unique_ptr<Session> session;
    std::unique_ptr<DrmBackend> backend;
    std::unique_ptr<RenderBackend> renderBackend;
    std::unique_ptr<DrmGpu> gpu;
};

class MockGlassesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void knownGlassesStartInSideBySideFull();
    void flatModesStayFlat();
    void unknownDisplayStaysFlat();
    void theOptionTurnsItOnForAnyDisplay();
    void theOptionTurnedOffKeepsTheFlatMode();
    void followsTheGlassesAcrossTheirModes();
    void followsTheGlassesAcrossAReconnect();
    void realDStaysFlat();
    void halfAnnouncesNothing();
};

void MockGlassesTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void MockGlassesTest::init()
{
    GpuManager::s_self = std::make_unique<GpuManager>();
    QFile::remove(configPath());
}

void MockGlassesTest::cleanup()
{
    GpuManager::s_self.reset();
}

void MockGlassesTest::knownGlassesStartInSideBySideFull()
{
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-full.bin")), s_sideBySideModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    QCOMPARE(outputs[0]->edid().eisaId(), QByteArrayLiteral("MRG"));
    QCOMPARE(int(outputs[0]->edid().productCode()), 0x4100);
    QCOMPARE(outputs[0]->edid().monitorName(), QByteArrayLiteral("XREAL One Pro"));

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    // the twin of the 3840x1080 mode: the frame is the mode, the desktop is one eye
    QCOMPARE(changes->currentMode->size(), QSize(3840, 1080));
    QVERIFY(nearRate(*changes->currentMode, 60000));
    QCOMPARE(stereoFlags(*changes->currentMode), Flag::VirtualStereo | Flag::Stereo3DSideBySideFull);
    QCOMPARE(outputs[0]->viewSizeForMode(*changes->currentMode), QSize(1920, 1080));
    // the twins come with the other stereo formats, so the toggle is turned on with it
    QVERIFY(changes->otherStereoFormats);
    QVERIFY(*changes->otherStereoFormats);
    QVERIFY(changes->enabled.value_or(true));
}

void MockGlassesTest::flatModesStayFlat()
{
    // in their 2D mode the glasses are an ordinary 1920x1080 monitor, at their highest rate
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-2d.bin")), s_flatModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(1920, 1080));
    QVERIFY(nearRate(*changes->currentMode, 120000));
    QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
    QCOMPARE(outputs[0]->viewSizeForMode(*changes->currentMode), QSize(1920, 1080));
}

void MockGlassesTest::unknownDisplayStaysFlat()
{
    // a 32:9 monitor is two eyes wide too, but it is not glasses: 2D, as always
    Rig rig(readEdid(QStringLiteral("Odyssey G5.bin")), s_sideBySideModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(3840, 1080));
    QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
    QVERIFY(!changes->otherStereoFormats.value_or(false));
}

void MockGlassesTest::theOptionTurnsItOnForAnyDisplay()
{
    // the user turns the option on for glasses KWin does not know: their two-eyes-wide mode comes up as the twin
    Rig rig(readEdid(QStringLiteral("Odyssey G5.bin")), s_sideBySideModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }
    setSavedOption(QStringLiteral("stereoGlasses"), true);
    // the saved 2D mode is the user's; the option applies when no saved mode fits
    rig.announce(readEdid(QStringLiteral("Odyssey G5.bin")), {{3840, 1080, 50.0}});
    QVERIFY(rig.gpu->updateOutputs());
    QCOMPARE(rig.outputs(), outputs);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(3840, 1080));
    QVERIFY(nearRate(*changes->currentMode, 50000));
    QCOMPARE(stereoFlags(*changes->currentMode), Flag::VirtualStereo | Flag::Stereo3DSideBySideFull);
    QVERIFY(changes->otherStereoFormats.value_or(false));
}

void MockGlassesTest::theOptionTurnedOffKeepsTheFlatMode()
{
    // the user turns the option off for known glasses: their 3D mode comes up as the plain 3840x1080
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-2d.bin")), s_flatModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }
    setSavedOption(QStringLiteral("stereoGlasses"), false);
    rig.announce(readEdid(QStringLiteral("xreal-one-pro-full.bin")), s_sideBySideModes);
    QVERIFY(rig.gpu->updateOutputs());
    QCOMPARE(rig.outputs(), outputs);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(3840, 1080));
    QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
}

void MockGlassesTest::followsTheGlassesAcrossTheirModes()
{
    // the glasses switch from 2D to Full SBS and back on the same connector; KWin chooses by identity each time
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-2d.bin")), s_flatModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        const auto changes = cfg->first.constChangeSet(outputs[0]);
        QCOMPARE(changes->currentMode->size(), QSize(1920, 1080));
        QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }

    // the cable stays in: the glasses tell the computer they are now a 3840x1080 device (as observed on Windows 11, 23 Oct 2026);
    // the output stays and takes the new EDID and mode list
    const QByteArray sideBySideEdid = readEdid(QStringLiteral("xreal-one-pro-full.bin"));
    rig.announce(sideBySideEdid, s_sideBySideModes);
    QVERIFY(rig.gpu->updateOutputs());
    QCOMPARE(rig.outputs(), outputs);
    QVERIFY(hasMode(outputs[0], QSize(3840, 1080)));
    QVERIFY(!hasMode(outputs[0], QSize(1920, 1080)));
    QCOMPARE(outputs[0]->edid().hash(), Edid(sideBySideEdid).hash());
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        const auto changes = cfg->first.constChangeSet(outputs[0]);
        QVERIFY(changes->currentMode);
        QCOMPARE(changes->currentMode->size(), QSize(3840, 1080));
        QCOMPARE(stereoFlags(*changes->currentMode), Flag::VirtualStereo | Flag::Stereo3DSideBySideFull);
        QVERIFY(changes->otherStereoFormats.value_or(false));
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }

    rig.announce(readEdid(QStringLiteral("xreal-one-pro-2d.bin")), s_flatModes);
    QVERIFY(rig.gpu->updateOutputs());
    QCOMPARE(rig.outputs(), outputs);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        const auto changes = cfg->first.constChangeSet(outputs[0]);
        QVERIFY(changes->currentMode);
        QCOMPARE(changes->currentMode->size(), QSize(1920, 1080));
        QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
    }
}

void MockGlassesTest::followsTheGlassesAcrossAReconnect()
{
    // unplugged in 2D mode, plugged back in Full SBS mode: a new output with the same identity
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-2d.bin")), s_flatModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    {
        const auto outputs = rig.outputs();
        QCOMPARE(outputs.size(), 1);
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }

    rig.connector->connection = DRM_MODE_DISCONNECTED;
    QVERIFY(rig.gpu->updateOutputs());
    QVERIFY(rig.outputs().isEmpty());
    rig.announce(readEdid(QStringLiteral("xreal-one-pro-full.bin")), s_sideBySideModes);
    rig.connector->connection = DRM_MODE_CONNECTED;
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(3840, 1080));
    QCOMPARE(stereoFlags(*changes->currentMode), Flag::VirtualStereo | Flag::Stereo3DSideBySideFull);
    QCOMPARE(outputs[0]->viewSizeForMode(*changes->currentMode), QSize(1920, 1080));
}

void MockGlassesTest::realDStaysFlat()
{
    // the glasses' Real 3D mode: the same identity, one 1920x1080 mode, so no twin; the glasses make the 3D
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-realD.bin")), s_realDModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    QCOMPARE(outputs[0]->edid().eisaId(), QByteArrayLiteral("MRG"));
    QCOMPARE(int(outputs[0]->edid().productCode()), 0x4100);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(1920, 1080));
    QVERIFY(nearRate(*changes->currentMode, 60000));
    QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
    QVERIFY(!changes->otherStereoFormats.value_or(false));
    QVERIFY(changes->stereoGlasses.value_or(false));
}

void MockGlassesTest::halfAnnouncesNothing()
{
    // the half side by side mode sends the 2D EDID byte for byte: the computer sees the 2D monitor, and the
    // user picks side by side (half) by hand under Other stereo formats, as with a 3D television
    QCOMPARE(readEdid(QStringLiteral("xreal-one-pro-half.bin")), readEdid(QStringLiteral("xreal-one-pro-2d.bin")));
    Rig rig(readEdid(QStringLiteral("xreal-one-pro-half.bin")), s_flatModes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), QSize(1920, 1080));
    QCOMPARE(stereoFlags(*changes->currentMode), OutputModeline::Flags());
}

QTEST_GUILESS_MAIN(MockGlassesTest)
#include "mockGlassesTest.moc"
