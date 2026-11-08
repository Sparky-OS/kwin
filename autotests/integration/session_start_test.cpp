/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "backends/virtual/virtual_backend.h"
#include "core/backendoutput.h"
#include "core/output.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include "cursor.h"

#include <KWayland/Client/output.h>
#include <KWayland/Client/surface.h>

#include <QDir>
#include <vector>
#include <linux/input.h>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace KWin
{

/**
 * The first windows of a session open on the screen marked main in Display Settings, also when Plasma's desktop windows come first
 * (Daniel, 2026-11-08, after rebooting onto kwin +stereo3d50: "we're back and it's not solved"; "I even left the windows that open
 * when I start"). Plasma maps one desktop window per output, in the order its screens come in, and the first one found KWin without
 * an active window, so Workspace::addWaylandWindow() activated it and Workspace::setActiveWindow() moved the active output to the
 * output of that desktop. A program mapped after it, the KDE Wallet prompt or an autostart program, opened on that output, whatever
 * the order of Display Settings said, until the pointer moved. A desktop that is activated only because nothing else is active is
 * not a place the person works in: the active output stays on the main screen. The pointer and the person's own activations move it
 * as before.
 */
class SessionStartTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // first: it needs the state of a session that has just started
    void testDesktopWindowsLeaveTheActiveOutputOnTheMainScreen();
    void testTheDesktopOfAnotherScreenStillMovesItWhenTheyAreClicked();
    void testTheSplashLeavesTheActiveOutputOnTheMainScreen_data();
    void testTheSplashLeavesTheActiveOutputOnTheMainScreen();
};

static const QSize s_outputSize(320, 240);

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

static LogicalOutput *s_first = nullptr;
static LogicalOutput *s_second = nullptr;
static LogicalOutput *s_main = nullptr;

// Plasma's desktop of one output: a layer surface of the background layer that takes the keyboard, anchored on all sides
struct Desktop
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::LayerSurfaceV1> shellSurface;
    Window *window = nullptr;
};

// the client's wl_output of an output: the clients list them in the order of the workspace's outputs (as layershellv1window_test relies on)
static KWayland::Client::Output *clientOutput(LogicalOutput *output)
{
    return Test::waylandOutputs().value(workspace()->outputs().indexOf(output));
}

static Desktop createLayerWindow(LogicalOutput *output, const QString &scope, Test::LayerShellV1::layer layer)
{
    Desktop desktop;
    KWayland::Client::Output *wlOutput = clientOutput(output);
    if (!wlOutput) {
        return desktop;
    }
    desktop.surface = Test::createSurface();
    desktop.shellSurface = Test::createLayerSurfaceV1(desktop.surface.get(), scope, wlOutput, layer);
    desktop.shellSurface->set_keyboard_interactivity(1);
    desktop.shellSurface->set_size(0, 0);
    desktop.shellSurface->set_exclusive_zone(-1);
    desktop.shellSurface->set_anchor(Test::LayerSurfaceV1::anchor_bottom | Test::LayerSurfaceV1::anchor_top | Test::LayerSurfaceV1::anchor_left | Test::LayerSurfaceV1::anchor_right);
    desktop.surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QSignalSpy configureSpy(desktop.shellSurface.get(), &Test::LayerSurfaceV1::configureRequested);
    if (configureSpy.wait()) {
        desktop.window = Test::renderAndWaitForShown(desktop.surface.get(), configureSpy.last().at(1).toSize(), Qt::blue);
    }
    return desktop;
}

static Desktop createDesktop(LogicalOutput *output)
{
    return createLayerWindow(output, QStringLiteral("desktop"), Test::LayerShellV1::layer_background);
}

void SessionStartTest::initTestCase()
{
    // the person's order: the third output is marked main (priority 1 of Display Settings), the backend lists it last
    const QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json");
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(s_config);
    file.close();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
    s_first = outputNamed(0);
    s_second = outputNamed(1);
    s_main = outputNamed(2);
}

void SessionStartTest::cleanupTestCase()
{
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json"));
}

void SessionStartTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::LayerShellV1 | Test::AdditionalWaylandInterface::Seat));
}

void SessionStartTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void SessionStartTest::testDesktopWindowsLeaveTheActiveOutputOnTheMainScreen()
{
    QVERIFY(s_first && s_second && s_main);
    QCOMPARE(workspace()->outputs().front(), s_first);
    QCOMPARE(workspace()->outputOrder().front(), s_main);
    QCOMPARE(workspace()->activeOutput(), s_main);
    QVERIFY(!workspace()->activeWindow());

    // Plasma maps its desktops in the order of the backend's outputs, the main screen's last
    Desktop first = createDesktop(s_first);
    QVERIFY(first.window);
    QVERIFY(first.window->isDesktop());
    QCOMPARE(first.window->output(), s_first);
    // nothing else is active, so KWin gives this desktop the focus, as always
    QCOMPARE(workspace()->activeWindow(), first.window);
    Desktop second = createDesktop(s_second);
    QVERIFY(second.window);
    Desktop third = createDesktop(s_main);
    QVERIFY(third.window);
    QCOMPARE(third.window->output(), s_main);
    // but the active output stays on the screen marked main
    QCOMPARE(workspace()->activeOutput(), s_main);

    // the KDE Wallet prompt, an autostart program: mapped before the pointer has moved, it opens on the main screen
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> toplevel(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::red);
    QVERIFY(window);
    QCOMPARE(window->output(), s_main);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(workspace()->activeOutput(), s_main);
}

void SessionStartTest::testTheDesktopOfAnotherScreenStillMovesItWhenTheyAreClicked()
{
    // the person's own action keeps working: the pointer moving to another screen takes the active output there, and so does a click
    // on the desktop of that screen
    Desktop first = createDesktop(s_first);
    QVERIFY(first.window);
    Test::pointerMotion(s_first->geometry().center(), 1);
    QCOMPARE(workspace()->activeOutput(), s_first);
    Test::pointerMotion(s_second->geometry().center(), 2);
    QCOMPARE(workspace()->activeOutput(), s_second);
    Desktop second = createDesktop(s_second);
    QVERIFY(second.window);
    quint32 timestamp = 3;
    Test::pointerButtonPressed(BTN_LEFT, timestamp++);
    Test::pointerButtonReleased(BTN_LEFT, timestamp++);
    QCOMPARE(workspace()->activeOutput(), s_second);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> toplevel(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::green);
    QVERIFY(window);
    QCOMPARE(window->output(), s_second);
}


void SessionStartTest::testTheSplashLeavesTheActiveOutputOnTheMainScreen_data()
{
    QTest::addColumn<int>("mainPosition");
    // the splash makes one window per screen in the order Qt lists the screens, and each is activated as it maps: the last one
    // took the active output, so the main screen was only right when the splash happened to serve it last
    QTest::newRow("main first") << 0;
    QTest::newRow("main in the middle") << 1;
    QTest::newRow("main last") << 2;
}

void SessionStartTest::testTheSplashLeavesTheActiveOutputOnTheMainScreen()
{
    QFETCH(int, mainPosition);
    // the state of a session that has just started: nothing active, the active output on the screen marked main
    QVERIFY(!workspace()->activeWindow());
    workspace()->setActiveOutput(s_main);
    QCOMPARE(workspace()->activeOutput(), s_main);

    // ksplashqml: an overlay layer surface with the exclusive keyboard on every screen, the main screen at the given position
    QList<LogicalOutput *> order{s_first, s_second};
    order.insert(mainPosition, s_main);
    std::vector<Desktop> splash;
    for (LogicalOutput *output : std::as_const(order)) {
        Desktop window = createLayerWindow(output, QStringLiteral("ksplashqml"), Test::LayerShellV1::layer_overlay);
        QVERIFY(window.window);
        QCOMPARE(window.window->output(), output);
        splash.push_back(std::move(window));
    }
    QCOMPARE(workspace()->activeOutput(), s_main);

    // an autostart program maps while the splash is up
    {
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        std::unique_ptr<Test::XdgToplevel> toplevel(Test::createXdgToplevelSurface(surface.get()));
        Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::red);
        QVERIFY(window);
        QCOMPARE(window->output(), s_main);
        QCOMPARE(workspace()->activeOutput(), s_main);
    }

    // the splash closes and the next window still opens on the main screen
    splash.clear();
    QTRY_VERIFY(workspace()->windows().isEmpty());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> toplevel(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(window->output(), s_main);
    QCOMPARE(workspace()->activeOutput(), s_main);
}

}

// the three outputs exist when KWin starts, as the connectors of a session do
WAYLANDTEST_MAIN_OPT(KWin::SessionStartTest, false, ([]() {
    auto backend = std::make_unique<KWin::VirtualBackend>();
    QList<KWin::VirtualBackend::OutputInfo> infos;
    for (int i = 0; i < 3; ++i) {
        infos.push_back(KWin::VirtualBackend::OutputInfo{.size = KWin::s_outputSize, .connectorName = QStringLiteral("Virtual-%1").arg(i)});
    }
    backend->setVirtualOutputs(infos);
    return std::unique_ptr<KWin::OutputBackend>(std::move(backend));
}))
#include "session_start_test.moc"
