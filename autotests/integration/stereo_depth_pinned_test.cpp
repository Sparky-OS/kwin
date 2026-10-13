/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "cursor.h"
#include "core/outputconfiguration.h"
#include "effect/effectloader.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "rules.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/surface.h>
#include <KWayland/Client/pointer.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/shm_pool.h>

#include <QDir>

namespace KWin
{

/**
 * A window pinned above the others (Keep Above, by the user or by a window rule) stays at the popped
 * limit and one pinned below stays at the sunk limit, whatever is active, on a stereo output.
 * The depth is read from a capture of the output: the parallax of a window is the horizontal
 * distance of its colour in the left eye's half to the same colour in the right eye's half.
 */
class StereoDepthPinnedTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testNothingPinned();
    void testCursorFollowsWindowDepth();
    void testKeepAboveStaysPopped();
    void testKeepBelowStaysSunk();
    void testUnpinReturnsToTheNormalRule();
    void testPinnedByWindowRule();
    void testTwoDimensionalOutputIsUnchanged();
};

static const QSize s_outputSize(320, 240);
// limits in pixels at 1920 wide, scaled to the 320 wide output: sunk 16 and popped 8 pixels
static constexpr qreal s_sunkLimit = 96;
static constexpr qreal s_poppedLimit = 48;
static constexpr int s_sunk = -16;
static constexpr int s_popped = 8;

static KSharedConfigPtr s_rulesConfig;

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

struct TestWindow
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shellSurface;
    Window *window = nullptr;
    QColor color;
    QPoint position;
};

static std::optional<TestWindow> s_background;

static TestWindow createWindow(const QColor &color, const QPoint &position, const QString &appId = QString(), const QSize &size = QSize(40, 40))
{
    TestWindow result;
    result.color = color;
    result.position = position;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createXdgToplevelSurface(result.surface.get());
    if (!appId.isEmpty()) {
        result.shellSurface->set_app_id(appId);
    }
    result.window = Test::renderAndWaitForShown(result.surface.get(), size, color);
    if (result.window) {
        result.window->move(position);
    }
    return result;
}

// the x of the first pixel of the colour on the row at y of the half that starts at xOffset, or -1
static int firstX(const QImage &image, int xOffset, int y, const QColor &color)
{
    for (int x = 0; x < s_outputSize.width(); ++x) {
        if (image.pixelColor(xOffset + x, y) == color) {
            return x;
        }
    }
    return -1;
}

// the parallax of the window's colour in the capture of the stereo output, left eye minus right eye
static int parallaxOf(const QImage &image, const TestWindow &window)
{
    const int y = window.position.y() + 20;
    const int left = firstX(image, 0, y, window.color);
    const int right = firstX(image, s_outputSize.width(), y, window.color);
    if (left < 0 || right < 0) {
        return std::numeric_limits<int>::min();
    }
    return left - right;
}

static std::optional<QImage> capture(bool includeCursor = false)
{
    QSignalSpy rendered(kwinApp()->scene(), &WorkspaceScene::frameRendered);
    kwinApp()->scene()->addRepaintFull();
    if (!rendered.wait()) {
        return std::nullopt;
    }
    QTest::qWait(100);
    ScreenShotManager manager;
    return manager.takeScreenShot(workspace()->outputs().front(), includeCursor ? ScreenShotFlags(ScreenShotIncludeCursor) : ScreenShotFlags(), std::nullopt);
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

static void activate(const TestWindow &window)
{
    workspace()->activateWindow(window.window);
    QVERIFY(Test::waylandSync());
    QCOMPARE(workspace()->activeWindow(), window.window);
}

void StereoDepthPinnedTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));

    // no effects: an open animation fades a window in and a close animation keeps it on the
    // screen, and the captures must hold exactly what the windows drew
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    KConfigGroup depth(config, QStringLiteral("StereoDepth"));
    depth.writeEntry("SunkLimit", s_sunkLimit);
    depth.writeEntry("PoppedLimit", s_poppedLimit);
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();
    s_rulesConfig = KSharedConfig::openConfig(QStringLiteral("kwinrulesrc"), KConfig::SimpleConfig);
    workspace()->rulebook()->setConfig(s_rulesConfig);
}

void StereoDepthPinnedTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(qstrcmp(QTest::currentTestFunction(), "testCursorFollowsWindowDepth") == 0 ? Test::AdditionalWaylandInterfaces(Test::AdditionalWaylandInterface::Seat) : Test::AdditionalWaylandInterfaces()));
    QVERIFY(stereoOutput(true));
    options->setStereoSunkLimit(s_sunkLimit);
    options->setStereoPoppedLimit(s_poppedLimit);
    s_background = createWindow(Qt::black, QPoint(), QString(), s_outputSize);
    QVERIFY(s_background->window);
    s_background->window->setKeepBelow(true);
}

void StereoDepthPinnedTest::cleanup()
{
    s_background.reset();
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
    for (const QString &group : s_rulesConfig->groupList()) {
        s_rulesConfig->deleteGroup(group);
    }
    workspace()->slotReconfigure();
}

void StereoDepthPinnedTest::testCursorFollowsWindowDepth()
{
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    QVERIFY(a.window && b.window);
    activate(b);
    std::unique_ptr<KWayland::Client::Pointer> pointer(Test::waylandSeat()->createPointer());
    QSignalSpy entered(pointer.get(), &KWayland::Client::Pointer::entered);
    Test::pointerMotion(QPointF(40, 40), 1);
    QVERIFY(entered.wait());
    auto cursorSurface = Test::createSurface();
    QImage green(QSize(10, 10), QImage::Format_ARGB32_Premultiplied);
    green.fill(Qt::green);
    cursorSurface->attachBuffer(Test::waylandShmPool()->createBuffer(green));
    cursorSurface->damage(green.rect());
    cursorSurface->commit();
    pointer->setCursor(cursorSurface.get(), QPoint(5, 5));
    Test::flushWaylandConnection();
    QTRY_COMPARE(kwinApp()->cursorImage().image().convertToFormat(green.format()), green);
    for (bool above : {false, true}) {
        a.window->setKeepAbove(above);
        QVERIFY(Test::waylandSync());
        QCOMPARE(a.window->keepAbove(), above);
        auto image = capture(true);
        QVERIFY(image);
        const int expected = above ? s_popped : s_sunk;
        QCOMPARE(parallaxOf(*image, a), expected);
        const int left = firstX(*image, 0, 40, Qt::green);
        const int right = firstX(*image, s_outputSize.width(), 40, Qt::green);
        QVERIFY(left >= 0 && right >= 0);
        QCOMPARE(left - right, expected);
        const QString directory = qEnvironmentVariable("KWIN_CURSOR_DEPTH_PROOF");
        if (!directory.isEmpty()) {
            QVERIFY(image->save(directory + (above ? QStringLiteral("/cursor-popped.png") : QStringLiteral("/cursor-sunk.png"))));
        }
    }
}

void StereoDepthPinnedTest::testNothingPinned()
{
    // the normal rule the other tests are measured against: active at the screen, inactive sunk
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    QVERIFY(a.window && b.window);

    activate(b);
    auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, b), 0);
    QCOMPARE(parallaxOf(*image, a), s_sunk);

    activate(a);
    image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, a), 0);
    QCOMPARE(parallaxOf(*image, b), s_sunk);
}

void StereoDepthPinnedTest::testKeepAboveStaysPopped()
{
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    const TestWindow c = createWindow(Qt::green, QPoint(20, 140));
    QVERIFY(a.window && b.window && c.window);

    a.window->setKeepAbove(true);
    QVERIFY(a.window->keepAbove());

    // whichever window is active, the pinned one is at the popped limit and the active one at the screen
    for (const TestWindow *active : {&a, &b, &c, &a, &c}) {
        activate(*active);
        const auto image = capture();
        QVERIFY(image);
        QCOMPARE(parallaxOf(*image, a), s_popped);
        if (active != &a) {
            QCOMPARE(parallaxOf(*image, *active), 0);
        }
    }

    // the inactive windows still share the sunk range beneath the active one
    activate(a);
    const auto image = capture();
    QVERIFY(image);
    QVERIFY(parallaxOf(*image, b) < 0);
    QVERIFY(parallaxOf(*image, c) < 0);
}

void StereoDepthPinnedTest::testKeepBelowStaysSunk()
{
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    QVERIFY(a.window && b.window);

    a.window->setKeepBelow(true);
    QVERIFY(a.window->keepBelow());

    // at the sunk limit even when it is the active window
    for (const TestWindow *active : {&a, &b, &a}) {
        activate(*active);
        const auto image = capture();
        QVERIFY(image);
        QCOMPARE(parallaxOf(*image, a), s_sunk);
    }
    // the other window is the only one that is not pinned: at the screen when active
    activate(b);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, b), 0);
}

void StereoDepthPinnedTest::testUnpinReturnsToTheNormalRule()
{
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    QVERIFY(a.window && b.window);
    activate(b);

    a.window->setKeepAbove(true);
    auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, a), s_popped);

    // unpinned: inactive, so sunk
    a.window->setKeepAbove(false);
    image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, a), s_sunk);
    QCOMPARE(parallaxOf(*image, b), 0);

    // unpinned and active: at the screen
    a.window->setKeepBelow(true);
    activate(a);
    image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, a), s_sunk);
    a.window->setKeepBelow(false);
    image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, a), 0);
    QCOMPARE(parallaxOf(*image, b), s_sunk);
}

void StereoDepthPinnedTest::testPinnedByWindowRule()
{
    // a rule that forces the window above: the user did not touch Keep Above
    const auto config = s_rulesConfig;
    const QString ruleGroupName = QStringLiteral("webcam-rule");
    config->group(QStringLiteral("General")).writeEntry("rules", QStringList{ruleGroupName});
    KConfigGroup group = config->group(ruleGroupName);
    group.writeEntry("above", true);
    group.writeEntry("aboverule", int(Rules::Force));
    group.writeEntry("wmclass", "org.kde.webcam");
    group.writeEntry("wmclasscomplete", false);
    group.writeEntry("wmclassmatch", int(Rules::ExactMatch));
    group.sync();
    workspace()->slotReconfigure();
    // a reconfigure reads the limits from kwinrc again
    options->setStereoSunkLimit(s_sunkLimit);
    options->setStereoPoppedLimit(s_poppedLimit);

    const TestWindow cam = createWindow(Qt::red, QPoint(20, 20), QStringLiteral("org.kde.webcam"));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    QVERIFY(cam.window && b.window);
    QVERIFY(cam.window->keepAbove());

    for (const TestWindow *active : {&b, &cam, &b}) {
        activate(*active);
        const auto image = capture();
        QVERIFY(image);
        QCOMPARE(parallaxOf(*image, cam), s_popped);
    }
    // the rule keeps it pinned: the program cannot unpin itself
    cam.window->setKeepAbove(false);
    QVERIFY(cam.window->keepAbove());
}

void StereoDepthPinnedTest::testTwoDimensionalOutputIsUnchanged()
{
    QVERIFY(stereoOutput(false));
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(140, 20));
    QVERIFY(a.window && b.window);
    a.window->setKeepAbove(true);
    b.window->setKeepBelow(true);
    activate(a);

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    // both exactly where they were put
    QCOMPARE(firstX(*image, 0, 40, Qt::red), 20);
    QCOMPARE(firstX(*image, 0, 40, Qt::blue), 140);
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthPinnedTest)
#include "stereo_depth_pinned_test.moc"
