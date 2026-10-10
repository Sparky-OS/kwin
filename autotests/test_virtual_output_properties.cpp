// SPDX-License-Identifier: GPL-2.0-or-later

#include "backends/drm/drm_backend.h"
#include "backends/drm/drm_layer.h"
#include "backends/drm/drm_render_backend.h"
#include "backends/virtual/virtual_backend.h"
#include "core/backendoutput.h"
#include "core/outputbackend.h"
#include "core/output.h"
#include "core/outputconfiguration.h"
#include "utils/orientationsensor.h"
#include "core/renderloop.h"
#include "outputconfigurationstore.h"

#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>

#include <limits>

using namespace KWin;

class NoScanoutBackend : public DrmRenderBackend
{
public:
    std::unique_ptr<DrmPipelineLayer> createDrmPlaneLayer(DrmPlane *) override { return nullptr; }
    std::unique_ptr<DrmPipelineLayer> createDrmPlaneLayer(DrmGpu *, DrmPlane::TypeIndex) override { return nullptr; }
    std::unique_ptr<DrmOutputLayer> createLayer(DrmVirtualOutput *) override { return nullptr; }
};

class TestLogicalOutput : public LogicalOutput
{
public:
    using LogicalOutput::LogicalOutput;
    using LogicalOutput::copyInfoFrom;
};

class VirtualOutputPropertiesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void creation_data()
    {
        QTest::addColumn<bool>("drm");
        QTest::addColumn<bool>("splitEye");
        QTest::newRow("headless-headset") << false << true;
        QTest::newRow("drm-headset") << true << true;
        QTest::newRow("headless-monitor") << false << false;
        QTest::newRow("drm-monitor") << true << false;
    }

    void creation()
    {
        QFETCH(bool, drm);
        QFETCH(bool, splitEye);
        NoScanoutBackend renderer;
        std::unique_ptr<OutputBackend> backend;
        if (drm) {
            auto drmBackend = std::make_unique<DrmBackend>(nullptr);
            drmBackend->setRenderBackend(&renderer);
            backend = std::move(drmBackend);
        } else {
            backend = std::make_unique<VirtualBackend>();
        }
        const VirtualOutputProperties properties{
            .name = QStringLiteral("persistent-phone-identity"),
            .description = QStringLiteral("Flat Headset (test panel)"),
            .pixelSize = QSize(2400, 1080),
            .refreshRate = 59940,
            .scale = 1.25,
            .physicalSize = QSize(150, 68),
            .splitEye = splitEye,
        };
        int announcements = 0;
        connect(backend.get(), &OutputBackend::outputAdded, this, [&](BackendOutput *output) {
            ++announcements;
            QCOMPARE(output->name(), QStringLiteral("Virtual-persistent-phone-identity"));
            QCOMPARE(output->model(), properties.description);
            QCOMPARE(output->edid().identifier(), QByteArray("persistent-phone-identity"));
            QVERIFY(output->edid().raw().isEmpty());
            QCOMPARE(output->modeSize(), QSize(2400, 1080));
            QCOMPARE(output->viewSize(), QSize(splitEye ? 1200 : 2400, 1080));
            QCOMPARE(output->refreshRate(), 59940);
            QCOMPARE(output->renderLoop()->refreshRate(), 59940);
            QCOMPARE(output->scale(), 1.25);
            QCOMPARE(output->physicalSize(), QSize(150, 68));
            QCOMPARE(output->isSplitEyeDisplay(), splitEye);
            QCOMPARE(output->hasStereoEyes(), splitEye);
            QCOMPARE(output->otherStereoFormats(), splitEye);
            QCOMPARE(output->currentMode()->flags().testFlag(OutputModeline::Flag::VirtualStereo), splitEye);
            QCOMPARE(output->currentMode()->flags().testFlag(OutputModeline::Flag::Stereo3DSideBySideHalf), splitEye);
            QCOMPARE(output->currentMode()->flags().testFlag(OutputModeline::Flag::Preferred), !splitEye);
        });
        BackendOutput *output = backend->createVirtualOutput(properties);
        QVERIFY(output);
        QCOMPARE(announcements, 1);
        QCOMPARE(backend->outputs(), QList<BackendOutput *>({output}));
        QSignalSpy removed(backend.get(), &OutputBackend::outputRemoved);
        backend->removeVirtualOutput(output);
        QCOMPARE(removed.count(), 1);
        QVERIFY(backend->outputs().isEmpty());

        auto invalid = properties;
        invalid.refreshRate = 0;
        QVERIFY(!backend->createVirtualOutput(invalid));
        QCOMPARE(announcements, 1);
        QVERIFY(backend->outputs().isEmpty());
    }

    void requestedModeOverridesSavedMode()
    {
        QTemporaryDir configHome;
        QVERIFY(configHome.isValid());
        qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
        OutputConfigurationStore store;
        store.clear();
        VirtualBackend backend;
        const VirtualOutputProperties properties{.name = QStringLiteral("reconnecting-phone"), .pixelSize = QSize(2400, 1080), .refreshRate = 59940, .scale = 1.25, .splitEye = true};
        auto output = backend.createVirtualOutput(properties);
        auto queried = store.queryConfig({output}, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(queried);
        output->applyChanges(queried->first);
        QVERIFY(!output->initialVirtualMode());
        QCOMPARE(output->currentMode()->size(), QSize(2400, 1080));
        QVERIFY(output->hasStereoEyes());
        OutputConfiguration mono;
        mono.changeSet(output)->currentMode = output->modes().back()->modeline();
        mono.changeSet(output)->scaleSetting = 1.5;
        mono.changeSet(output)->pos = QPoint(80, 40);
        output->applyChanges(mono);
        QVERIFY(!output->hasStereoEyes());
        store.storeConfig({output}, false);
        backend.removeVirtualOutput(output);
        output = backend.createVirtualOutput(properties);
        queried = store.queryConfig({output}, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(queried);
        const auto changes = queried->first.constChangeSet(output);
        QVERIFY(changes->currentMode->flags().testFlag(OutputModeline::Flag::Stereo3DSideBySideHalf));
        QCOMPARE(changes->currentMode->refreshRate(), 59940);
        QCOMPARE(changes->scaleSetting.value(), 1.5);
        QCOMPARE(changes->pos.value(), QPoint(80, 40));
        output->applyChanges(queried->first);
        QVERIFY(output->hasStereoEyes());
        backend.removeVirtualOutput(output);
    }

    void nativeViewportGeometry()
    {
        VirtualBackend backend;
        const VirtualOutputProperties properties{.name = QStringLiteral("native-viewport"), .pixelSize = QSize(2400, 1080), .refreshRate = 59940, .scale = 1.25, .splitEye = true};
        auto output = backend.createVirtualOutput(properties);
        {
            TestLogicalOutput logical(output);
            logical.copyInfoFrom(output);
            QCOMPARE(logical.pixelSize(), QSize(1200, 1080));
            QCOMPARE(logical.geometryF().size(), QSizeF(960, 864));
            QCOMPARE(output->modeSize(), QSize(2400, 1080));
            OutputConfiguration rotated;
            rotated.changeSet(output)->transform = OutputTransform::Rotate90;
            output->applyChanges(rotated);
            logical.copyInfoFrom(output);
            QCOMPARE(logical.geometryF().size(), QSizeF(864, 960));
            OutputConfiguration mono;
            mono.changeSet(output)->currentMode = output->modes().back()->modeline();
            output->applyChanges(mono);
            logical.copyInfoFrom(output);
            QCOMPARE(logical.geometryF().size(), QSizeF(864, 1920));
        }
        backend.removeVirtualOutput(output);
    }

    void invalidProperties()
    {
        VirtualOutputProperties properties{.name = QStringLiteral("phone"), .pixelSize = QSize(2400, 1080), .splitEye = true};
        QVERIFY(properties.isValid());
        auto invalid = properties;
        invalid.name.clear();
        QVERIFY(!invalid.isValid());
        invalid = properties;
        invalid.pixelSize.setWidth(2399);
        QVERIFY(!invalid.isValid());
        invalid = properties;
        invalid.pixelSize.setHeight(0);
        QVERIFY(!invalid.isValid());
        invalid = properties;
        invalid.scale = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!invalid.isValid());
        invalid = properties;
        invalid.scale = 0;
        QVERIFY(!invalid.isValid());
        invalid = properties;
        invalid.refreshRate = std::numeric_limits<uint32_t>::max();
        QVERIFY(!invalid.isValid());
        invalid = properties;
        invalid.physicalSize = QSize(150, 0);
        QVERIFY(!invalid.isValid());
    }
};

QTEST_GUILESS_MAIN(VirtualOutputPropertiesTest)
#include "test_virtual_output_properties.moc"
