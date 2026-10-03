/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "kwin_wayland_test.h"
#include "backends/virtual/virtual_backend.h"
#include "core/outputconfiguration.h"
#include "outputconfigurationstore.h"
#include "utils/orientationsensor.h"
#include "wayland_server.h"
#include "workspace.h"
#include <KWayland/Client/registry.h>
#include <KWayland/Client/event_queue.h>

namespace KWin
{

class RecordedMode : public QtWayland::kde_output_device_mode_v2
{
public:
    explicit RecordedMode(::kde_output_device_mode_v2 *mode)
        : QtWayland::kde_output_device_mode_v2(mode) {}
    ~RecordedMode() override { kde_output_device_mode_v2_destroy(object()); }
    uint32_t flags = 0;
protected:
    void kde_output_device_mode_v2_flags(uint32_t value) override { flags = value; }
};

class RecordedOutput : public QtWayland::kde_output_device_v2
{
public:
    explicit RecordedOutput(::kde_output_device_v2 *output)
        : QtWayland::kde_output_device_v2(output) {}
    ~RecordedOutput() override { release(); }
    std::vector<std::unique_ptr<RecordedMode>> modes;
    ::kde_output_device_mode_v2 *current = nullptr;
    int doneCount = 0;
    int toggleEvents = 0;
    uint32_t anaglyph = 0;
    uint32_t other = 0;
protected:
    void kde_output_device_v2_mode(::kde_output_device_mode_v2 *mode) override { modes.push_back(std::make_unique<RecordedMode>(mode)); }
    void kde_output_device_v2_current_mode(::kde_output_device_mode_v2 *mode) override { current = mode; }
    void kde_output_device_v2_done() override { ++doneCount; }
    void kde_output_device_v2_stereo_formats(uint32_t a, uint32_t o) override
    {
        ++toggleEvents;
        anaglyph = a;
        other = o;
    }
};

class RecordedRegistry : public QtWayland::kde_output_device_registry_v2
{
public:
    std::vector<std::unique_ptr<RecordedOutput>> outputs;
protected:
    void kde_output_device_registry_v2_output(::kde_output_device_v2 *output) override { outputs.push_back(std::make_unique<RecordedOutput>(output)); }
};

class VirtualStereoProtocolTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(waylandServer()->init(qAppName()));
        kwinApp()->start();
    }
    void init() { QVERIFY(Test::setupWaylandConnection()); }
    void cleanup() { Test::destroyWaylandConnection(); }
    void protocol_data()
    {
        QTest::addColumn<int>("version");
        QTest::newRow("older-21") << 21;
        QTest::newRow("older-23") << 23;
        QTest::newRow("stereo-24") << 24;
    }
    void protocol()
    {
        QFETCH(int, version);
        using Flag = OutputModeline::Flag;
        const OutputModeline base(QSize(1920, 1080), 60000, Flag::Preferred);
        const OutputModeline hdmi(base.size(), 60000, Flag::Stereo3DSideBySideHalf);
        const OutputModeline twin(base.size(), 60000, Flag::VirtualStereo | Flag::Stereo3DSideBySideHalf);
        const OutputModeline rows(base.size(), 60000, Flag::VirtualStereo | Flag::Stereo3DRowsLeftFirst);
        auto backend = qobject_cast<VirtualBackend *>(kwinApp()->outputBackend());
        backend->setVirtualOutputs({VirtualBackend::OutputInfo{
            .size = base.size(),
            .modes = {base, hdmi, twin, rows},
            .edidIdentifierOverride = QByteArrayLiteral("VirtualStereoProtocol"),
        }});
        const auto output = backend->outputs().front();
        workspace()->outputConfigureStore()->clear();
        const auto fresh = workspace()->outputConfigureStore()->queryConfig({output}, false, AccelerometerOrientation::Undefined, false);
        QVERIFY(fresh);
        QCOMPARE(*fresh->first.constChangeSet(output)->currentMode, base);

        KWayland::Client::EventQueue queue;
        queue.setup(Test::waylandConnection());
        KWayland::Client::Registry registry;
        registry.setEventQueue(&queue);
        RecordedRegistry devices;
        std::unique_ptr<Test::WaylandOutputManagementV2> management;
        connect(&registry, &KWayland::Client::Registry::interfaceAnnounced, &registry, [&](const QByteArray &interface, uint32_t name, uint32_t advertised) {
            if (interface == kde_output_device_registry_v2_interface.name) {
                devices.init(registry, name, std::min(uint32_t(version), advertised));
            } else if (interface == kde_output_management_v2_interface.name) {
                management = std::make_unique<Test::WaylandOutputManagementV2>(registry, name, std::min(22u, advertised));
            }
        });
        QSignalSpy announced(&registry, &KWayland::Client::Registry::interfacesAnnounced);
        registry.create(Test::waylandConnection());
        registry.setup();
        QVERIFY(announced.wait());
        QVERIFY(Test::waylandSync());
        QTRY_COMPARE(devices.outputs.size(), 1u);
        auto &device = *devices.outputs.front();
        QTRY_VERIFY(device.doneCount > 0);
        QVERIFY(management);
        QCOMPARE(device.modes.size(), version >= 24 ? 4u : 2u);
        QCOMPARE(device.modes[0]->flags, 0u);
        QCOMPARE(device.modes[1]->flags, 4u);
        QCOMPARE(device.toggleEvents, version >= 24 ? 1 : 0);
        if (version >= 24) {
            QCOMPARE(device.modes[2]->flags, 0x4004u);
            QCOMPARE(device.modes[3]->flags, 0x4100u);
            std::unique_ptr<Test::WaylandOutputConfigurationV2> config(management->createConfiguration());
            QSignalSpy applied(config.get(), &Test::WaylandOutputConfigurationV2::applied);
            config->set_stereo_formats(device.object(), 1, 0);
            config->apply();
            QVERIFY(applied.wait());
            QTRY_COMPARE(device.anaglyph, 1u);
            QCOMPARE(device.other, 0u);
            QVERIFY(output->anaglyph());
            QVERIFY(!output->otherStereoFormats());

            std::unique_ptr<Test::WaylandOutputConfigurationV2> invalid(management->createConfiguration());
            QSignalSpy failed(invalid.get(), &Test::WaylandOutputConfigurationV2::failed);
            invalid->set_stereo_formats(device.object(), 2, 1);
            invalid->apply();
            QVERIFY(failed.wait());
            QVERIFY(output->anaglyph());
            QVERIFY(!output->otherStereoFormats());
        }
        const int doneBefore = device.doneCount;
        OutputConfiguration activate;
        activate.changeSet(output)->currentMode = twin;
        output->applyChanges(activate);
        QVERIFY(Test::waylandSync());
        QTRY_VERIFY(device.doneCount > doneBefore);
        QCOMPARE(device.current, device.modes[version >= 24 ? 2 : 0]->object());
        devices.stop();
    }
};

}

WAYLANDTEST_MAIN(KWin::VirtualStereoProtocolTest)
#include "virtual_stereo_protocol_test.moc"
