/* SPDX-License-Identifier: GPL-2.0-or-later */

// The eye order of an output is its one swap eyes setting, for every 3D mode (ORDER 116). A right eye first twin of
// rows, columns, checkerboard or frame sequential saved by an earlier KWin comes back as its left eye first mode with the swap flipped,
// which draws the same picture; a saved frame sequential mode is restored. (The list of twins is checked in virtualStereoTest.)

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


// the display under test: a 1920x1080 monitor at 60 Hz, the native size, and at 120 Hz
static const QList<ModeSpec> s_modes{{1920, 1080, 60.0}, {1920, 1080, 120.0}};

static OutputModeline::Flags layoutOf(const OutputModeline &mode)
{
    return mode.flags() & OutputModeline::AllStereo3D;
}

// the native 2D mode of the output, at 60 Hz, or the 120 Hz one for frame sequential
static std::shared_ptr<OutputMode> baseMode(BackendOutput *output, bool highRate)
{
    for (const auto &mode : output->modes()) {
        if (!mode->isRemoved() && !(mode->flags() & (OutputModeline::AllStereo3D | Flag::VirtualStereo)) && mode->size() == QSize(1920, 1080)
            && (mode->refreshRate() >= 100000) == highRate) {
            return mode;
        }
    }
    return nullptr;
}

class MockEyeOrderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void aSavedRightEyeFirstTwinComesBackLeftEyeFirstSwapped_data();
    void aSavedRightEyeFirstTwinComesBackLeftEyeFirstSwapped();
    void aSavedLeftEyeFirstModeKeepsItsSwap();
    void aSavedFrameSequentialModeIsRestored();
};

void MockEyeOrderTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void MockEyeOrderTest::init()
{
    GpuManager::s_self = std::make_unique<GpuManager>();
    QFile::remove(configPath());
}

void MockEyeOrderTest::cleanup()
{
    GpuManager::s_self.reset();
}

void MockEyeOrderTest::aSavedRightEyeFirstTwinComesBackLeftEyeFirstSwapped_data()
{
    QTest::addColumn<int>("rightFirst");
    QTest::addColumn<int>("leftFirst");
    QTest::addColumn<bool>("highRate");
    QTest::addColumn<bool>("savedSwap");
    for (const bool swap : {false, true}) {
        const QByteArray suffix = swap ? " with the swap on" : "";
        QTest::newRow(("rows" + suffix).constData()) << int(Flag::Stereo3DRowsRightFirst) << int(Flag::Stereo3DRowsLeftFirst) << false << swap;
        QTest::newRow(("columns" + suffix).constData()) << int(Flag::Stereo3DColumnsRightFirst) << int(Flag::Stereo3DColumnsLeftFirst) << false << swap;
        QTest::newRow(("frame sequential" + suffix).constData()) << int(Flag::Stereo3DSequentialRightFirst) << int(Flag::Stereo3DSequentialLeftFirst) << true << swap;
        QTest::newRow(("checkerboard" + suffix).constData()) << int(Flag::Stereo3DCheckerboardRightFirst) << int(Flag::Stereo3DCheckerboardLeftFirst) << false << swap;
    }
}

void MockEyeOrderTest::aSavedRightEyeFirstTwinComesBackLeftEyeFirstSwapped()
{
    QFETCH(int, rightFirst);
    QFETCH(int, leftFirst);
    QFETCH(bool, highRate);
    QFETCH(bool, savedSwap);
    Rig rig(readEdid(QStringLiteral("Odyssey G5.bin")), s_modes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    const auto base = baseMode(outputs[0], highRate);
    QVERIFY(base);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }
    // what an earlier KWin saved after the person chose the right eye first twin
    QJsonObject mode;
    mode["width"] = base->size().width();
    mode["height"] = base->size().height();
    mode["refreshRate"] = int(base->refreshRate());
    mode["flags"] = int(OutputModeline::Flags((base->flags() & ~OutputModeline::Flags(Flag::Preferred)) | Flag::VirtualStereo | Flag(rightFirst)));
    setSavedOption(QStringLiteral("mode"), mode);
    setSavedOption(QStringLiteral("otherStereoFormats"), true);
    setSavedOption(QStringLiteral("stereoSwapEyes"), savedSwap);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->size(), base->size());
    QCOMPARE(changes->currentMode->refreshRate(), base->refreshRate());
    QVERIFY(changes->currentMode->flags() & Flag::VirtualStereo);
    QCOMPARE(layoutOf(*changes->currentMode), OutputModeline::Flags(Flag(leftFirst)));
    // the old twin drew the right eye first, with the swap on it drew the left eye first: the new mode with the swap flipped draws the same
    QCOMPARE(changes->stereoSwapEyes, std::optional<bool>(!savedSwap));

    outputs[0]->applyChanges(cfg->first);
    QCOMPARE(outputs[0]->stereoSwapEyes(), !savedSwap);
}

void MockEyeOrderTest::aSavedLeftEyeFirstModeKeepsItsSwap()
{
    Rig rig(readEdid(QStringLiteral("Odyssey G5.bin")), s_modes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    const auto base = baseMode(outputs[0], false);
    QVERIFY(base);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }
    QJsonObject mode;
    mode["width"] = base->size().width();
    mode["height"] = base->size().height();
    mode["refreshRate"] = int(base->refreshRate());
    mode["flags"] = int(OutputModeline::Flags((base->flags() & ~OutputModeline::Flags(Flag::Preferred)) | Flag::VirtualStereo | Flag::Stereo3DRowsLeftFirst));
    setSavedOption(QStringLiteral("mode"), mode);
    setSavedOption(QStringLiteral("otherStereoFormats"), true);
    setSavedOption(QStringLiteral("stereoSwapEyes"), true);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(layoutOf(*changes->currentMode), OutputModeline::Flags(Flag::Stereo3DRowsLeftFirst));
    QCOMPARE(changes->stereoSwapEyes, std::optional<bool>(true));
}

void MockEyeOrderTest::aSavedFrameSequentialModeIsRestored()
{
    Rig rig(readEdid(QStringLiteral("Odyssey G5.bin")), s_modes);
    QVERIFY(rig.mockGpu);
    QVERIFY(rig.gpu->updateOutputs());
    const auto outputs = rig.outputs();
    QCOMPARE(outputs.size(), 1);
    const auto base = baseMode(outputs[0], true);
    QVERIFY(base);
    {
        OutputConfigurationStore store;
        const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(cfg);
        outputs[0]->applyChanges(cfg->first);
        store.storeConfig(outputs, false);
    }
    QJsonObject mode;
    mode["width"] = base->size().width();
    mode["height"] = base->size().height();
    mode["refreshRate"] = int(base->refreshRate());
    mode["flags"] = int(OutputModeline::Flags((base->flags() & ~OutputModeline::Flags(Flag::Preferred)) | Flag::VirtualStereo | Flag::Stereo3DSequentialLeftFirst));
    setSavedOption(QStringLiteral("mode"), mode);
    setSavedOption(QStringLiteral("otherStereoFormats"), true);

    OutputConfigurationStore store;
    const auto cfg = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
    QVERIFY(cfg);
    const auto changes = cfg->first.constChangeSet(outputs[0]);
    QVERIFY(changes->currentMode);
    QCOMPARE(changes->currentMode->refreshRate(), base->refreshRate());
    QVERIFY(changes->currentMode->flags() & Flag::VirtualStereo);
    QCOMPARE(layoutOf(*changes->currentMode), OutputModeline::Flags(Flag::Stereo3DSequentialLeftFirst));
}

QTEST_GUILESS_MAIN(MockEyeOrderTest)
#include "mockEyeOrderTest.moc"
