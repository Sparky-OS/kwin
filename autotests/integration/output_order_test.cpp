/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "backends/virtual/virtual_backend.h"
#include "core/backendoutput.h"
#include "core/output.h"
#include "core/outputconfiguration.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include "cursor.h"

#include <KWayland/Client/surface.h>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace KWin
{

/**
 * The output the active output comes back to, and where a window opens before the pointer has moved. The person's order of the
 * outputs (the priorities of Display Settings) decides, not the order the backend lists them in (Daniel, 2026-11-08: "upon restarting
 * the default monitor is actually the one connected to the nvidia card, all the passwords request and new windows end up on it, even
 * though my main monitor is the other sony"; "after started, it follows the mouse as expected"). Three outputs in a row, listed
 * 0, 1, 2 by the backend with the order 2 first, then 1, then 0 (priorities 1, 2, 3 of Display Settings) in the saved configuration: KWin
 * starts with the active output and the pointer on the third, a window mapped before the pointer moves is placed there, and after
 * the pointer moves to another output the next window opens where the pointer is.
 */
class OutputOrderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testStartsOnTheScreenMarkedMain();
    void testNewWindowBeforeAndAfterThePointerMoves();
    void testChangingTheMainScreenMovesTheOrder();
};

static const QSize s_size(320, 240);
static const QSize &s_outputSize = s_size;

// the saved output configuration of a person whose order is the reverse of the backend's: the third output first (priority 1 of Display
// Settings), the second, the first (a KWin session's own file, with the sizes of this test)
static const char s_config[] = R"JSON([{"data":[{"abmLevel":0,"allowDdcCi":true,"allowSdrSoftwareBrightness":true,"anaglyph":false,"autoBrightnessCurve":[0,0,0,0,0,0,0,0,0,0,0],"autoRotation":"InTabletMode","automaticBrightness":false,"brightness":1,"colorPowerTradeoff":"PreferAccuracy","colorProfileSource":"sRGB","connectorName":"Virtual-0","customModes":[],"detectedDdcCi":false,"edrPolicy":"always","hdrColorProfileSource":"EDID","hdrIccProfilePath":"","highDynamicRange":false,"iccProfilePath":"","ized3dMode":0,"ized3dTransfer":0,"maxBitsPerColor":0,"mode":{"flags":1,"height":240,"refreshRate":60000,"width":320},"otherStereoFormats":false,"overscan":0,"rgbRange":"Automatic","scale":1,"sdrBrightness":200,"sdrGamutWideness":0,"sharpness":0,"sideBySideFullTiming":0,"stereoDepthSource":2,"stereoGlasses":false,"stereoPairReflection":"None","stereoPairRole":"Left","stereoSwapEyes":false,"stereoViewingDistance":0,"transform":"Normal","uuid":"fc410271-ffc5-42ec-b40e-d24b84175d35","vrrPolicy":"Automatic","wideColorGamut":false},{"abmLevel":0,"allowDdcCi":true,"allowSdrSoftwareBrightness":true,"anaglyph":false,"autoBrightnessCurve":[0,0,0,0,0,0,0,0,0,0,0],"autoRotation":"InTabletMode","automaticBrightness":false,"brightness":1,"colorPowerTradeoff":"PreferAccuracy","colorProfileSource":"sRGB","connectorName":"Virtual-1","customModes":[],"detectedDdcCi":false,"edrPolicy":"always","hdrColorProfileSource":"EDID","hdrIccProfilePath":"","highDynamicRange":false,"iccProfilePath":"","ized3dMode":0,"ized3dTransfer":0,"maxBitsPerColor":0,"mode":{"flags":1,"height":240,"refreshRate":60000,"width":320},"otherStereoFormats":false,"overscan":0,"rgbRange":"Automatic","scale":1,"sdrBrightness":200,"sdrGamutWideness":0,"sharpness":0,"sideBySideFullTiming":0,"stereoDepthSource":2,"stereoGlasses":false,"stereoPairReflection":"None","stereoPairRole":"Left","stereoSwapEyes":false,"stereoViewingDistance":0,"transform":"Normal","uuid":"b354fe66-78c9-4d1a-b872-8e9db33c021c","vrrPolicy":"Automatic","wideColorGamut":false},{"abmLevel":0,"allowDdcCi":true,"allowSdrSoftwareBrightness":true,"anaglyph":false,"autoBrightnessCurve":[0,0,0,0,0,0,0,0,0,0,0],"autoRotation":"InTabletMode","automaticBrightness":false,"brightness":1,"colorPowerTradeoff":"PreferAccuracy","colorProfileSource":"sRGB","connectorName":"Virtual-2","customModes":[],"detectedDdcCi":false,"edrPolicy":"always","hdrColorProfileSource":"EDID","hdrIccProfilePath":"","highDynamicRange":false,"iccProfilePath":"","ized3dMode":0,"ized3dTransfer":0,"maxBitsPerColor":0,"mode":{"flags":1,"height":240,"refreshRate":60000,"width":320},"otherStereoFormats":false,"overscan":0,"rgbRange":"Automatic","scale":1,"sdrBrightness":200,"sdrGamutWideness":0,"sharpness":0,"sideBySideFullTiming":0,"stereoDepthSource":2,"stereoGlasses":false,"stereoPairReflection":"None","stereoPairRole":"Left","stereoSwapEyes":false,"stereoViewingDistance":0,"transform":"Normal","uuid":"a2aaf526-ebd3-4751-8698-e4d4a12f4003","vrrPolicy":"Automatic","wideColorGamut":false}],"name":"outputs"},{"data":[{"lidClosed":false,"outputs":[{"enabled":true,"outputIndex":0,"position":{"x":0,"y":0},"priority":2,"replicationSource":""},{"enabled":true,"outputIndex":1,"position":{"x":320,"y":0},"priority":1,"replicationSource":""},{"enabled":true,"outputIndex":2,"position":{"x":640,"y":0},"priority":0,"replicationSource":""}]}],"name":"setups"}])JSON";

static LogicalOutput *outputNamed(int index)
{
    const auto list = workspace()->outputs();
    for (LogicalOutput *output : list) {
        if (output->name() == QStringLiteral("Virtual-%1").arg(index)) {
            return output;
        }
    }
    return nullptr;
}

// which output is marked main in Display Settings (priority 1) in the saved configuration: the third unless KWIN_TEST_MAIN_OUTPUT says another
static int mainOutput()
{
    const int index = qEnvironmentVariableIsSet("KWIN_TEST_MAIN_OUTPUT") ? qEnvironmentVariableIntValue("KWIN_TEST_MAIN_OUTPUT") : 2;
    return std::clamp(index, 0, 2);
}

// the saved configuration with the main output's setup priority 0 (priority 1 of Display Settings) and the others after it in the order of the backend
static QByteArray configWithMain(int main)
{
    QJsonArray root = QJsonDocument::fromJson(QByteArray(s_config)).array();
    for (int i = 0; i < root.size(); ++i) {
        QJsonObject section = root[i].toObject();
        if (section[QStringLiteral("name")].toString() != QStringLiteral("setups")) {
            continue;
        }
        QJsonArray setups = section[QStringLiteral("data")].toArray();
        for (int s = 0; s < setups.size(); ++s) {
            QJsonObject setup = setups[s].toObject();
            QJsonArray outputs = setup[QStringLiteral("outputs")].toArray();
            int next = 1;
            for (int o = 0; o < outputs.size(); ++o) {
                QJsonObject output = outputs[o].toObject();
                output[QStringLiteral("priority")] = output[QStringLiteral("outputIndex")].toInt() == main ? 0 : next++;
                outputs[o] = output;
            }
            setup[QStringLiteral("outputs")] = outputs;
            setups[s] = setup;
        }
        section[QStringLiteral("data")] = setups;
        root[i] = section;
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static LogicalOutput *s_first = nullptr;
static LogicalOutput *s_second = nullptr;
static LogicalOutput *s_third = nullptr;
static LogicalOutput *s_main = nullptr;

void OutputOrderTest::initTestCase()
{
    // the person's order is in the output configuration KWin starts with
    const QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json");
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(configWithMain(mainOutput()));
    file.close();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
    s_first = outputNamed(0);
    s_second = outputNamed(1);
    s_third = outputNamed(2);
    s_main = outputNamed(mainOutput());
}

void OutputOrderTest::cleanupTestCase()
{
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json"));
}

void OutputOrderTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat));
}

void OutputOrderTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void OutputOrderTest::testStartsOnTheScreenMarkedMain()
{
    QVERIFY(s_first && s_second && s_third && s_main);
    // the backend lists the first output first; the screen marked main in Display Settings (priority 1) leads the order
    QCOMPARE(workspace()->outputs().front(), s_first);
    QCOMPARE(workspace()->outputOrder().front(), s_main);
    // so the active output starts there, and so does the pointer: the first move of the mouse does not take the focus away
    QCOMPARE(workspace()->activeOutput(), s_main);
    QCOMPARE(workspace()->outputAt(Cursors::self()->mouse()->pos()), s_main);
}

void OutputOrderTest::testNewWindowBeforeAndAfterThePointerMoves()
{
    // a window mapped before any motion of the pointer opens on the screen marked main
    std::unique_ptr<KWayland::Client::Surface> first(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> firstToplevel(Test::createXdgToplevelSurface(first.get()));
    Window *window = Test::renderAndWaitForShown(first.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(window->output(), s_main);

    // after the pointer has moved to another output the next window opens where the pointer is, as before
    LogicalOutput *other = s_main == s_second ? s_third : s_second;
    Test::pointerMotion(other->geometry().center(), 1);
    QCOMPARE(workspace()->activeOutput(), other);
    std::unique_ptr<KWayland::Client::Surface> second(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> secondToplevel(Test::createXdgToplevelSurface(second.get()));
    Window *next = Test::renderAndWaitForShown(second.get(), QSize(100, 50), Qt::red);
    QVERIFY(next);
    QCOMPARE(next->output(), other);
}

void OutputOrderTest::testChangingTheMainScreenMovesTheOrder()
{
    // the main screen is changed through the output configuration that kde_output_management_v2 set_priority builds: the order follows
    OutputConfiguration configuration;
    LogicalOutput *wanted = s_main == s_first ? s_second : s_first;
    uint32_t priority = 1;
    configuration.changeSet(wanted->backendOutput())->priority = 0;
    for (LogicalOutput *output : {s_first, s_second, s_third}) {
        if (output != wanted) {
            configuration.changeSet(output->backendOutput())->priority = priority++;
        }
    }
    QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
    QCOMPARE(workspace()->outputOrder().front(), wanted);
    s_main = wanted;
}

}

// the three outputs exist when KWin starts, as the connectors of a session do
WAYLANDTEST_MAIN_OPT(KWin::OutputOrderTest, false, ([]() {
    auto backend = std::make_unique<KWin::VirtualBackend>();
    QList<KWin::VirtualBackend::OutputInfo> infos;
    for (int i = 0; i < 3; ++i) {
        infos.push_back(KWin::VirtualBackend::OutputInfo{.size = KWin::s_outputSize, .connectorName = QStringLiteral("Virtual-%1").arg(i)});
    }
    backend->setVirtualOutputs(infos);
    return std::unique_ptr<KWin::OutputBackend>(std::move(backend));
}))
#include "output_order_test.moc"
