/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "effect/effectloader.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>

namespace KWin
{

/**
 * The panel shell's notifications and on-screen displays on a stereo output. They are boxes that
 * pop up over the windows, so they sit at the popped level like a menu; a critical one, and any
 * notification while a window is pinned above the others, are the nearest. The depth is read from
 * a capture of the output: the parallax of a box is the horizontal distance of its colour in the
 * left eye's half to the same colour in the right eye's half.
 */
class StereoDepthNotificationsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testEachKindIsPopped_data();
    void testEachKindIsPopped();
    void testSeveralNotifications();
    void testCriticalAndNormal();
    void testWithOnScreenDisplay();
    void testMenusKeepTheirShare();
    void testOverAPinnedWindow();
    void testWithinThePoppedLimit();
    void testTwoDimensionalOutputIsUnchanged();
};

static const QSize s_outputSize(320, 240);
// limits in pixels at 1920 wide, scaled to the 320 wide output: sunk 16 and popped 10 pixels
static constexpr qreal s_sunkLimit = 96;
static constexpr qreal s_poppedLimit = 60;
static constexpr int s_popped = 10;

using Role = KWayland::Client::PlasmaShellSurface::Role;

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

struct TestBox
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> toplevel;
    std::unique_ptr<KWayland::Client::PlasmaShellSurface> plasmaSurface;
    Window *window = nullptr;
    QColor color;
};

// a surface of the panel shell, placed by its program at @a position
static std::unique_ptr<TestBox> createShellBox(Role role, const QColor &color, const QPoint &position, const QSize &size = QSize(60, 40))
{
    auto result = std::make_unique<TestBox>();
    result->color = color;
    result->surface = Test::createSurface();
    result->toplevel = Test::createXdgToplevelSurface(result->surface.get());
    result->plasmaSurface.reset(Test::waylandPlasmaShell()->createSurface(result->surface.get()));
    result->plasmaSurface->setRole(role);
    result->plasmaSurface->setPosition(position);
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, color);
    return result;
}

static int firstX(const QImage &image, int xOffset, int y, const QColor &color)
{
    for (int x = 0; x < s_outputSize.width(); ++x) {
        if (image.pixelColor(xOffset + x, y) == color) {
            return x;
        }
    }
    return -1;
}

static int parallaxOf(const QImage &image, const TestBox &box, int y)
{
    const int left = firstX(image, 0, y, box.color);
    const int right = firstX(image, s_outputSize.width(), y, box.color);
    if (left < 0 || right < 0) {
        return std::numeric_limits<int>::min();
    }
    return left - right;
}

static std::optional<QImage> capture()
{
    QSignalSpy rendered(kwinApp()->scene(), &WorkspaceScene::frameRendered);
    kwinApp()->scene()->addRepaintFull();
    if (!rendered.wait()) {
        return std::nullopt;
    }
    QTest::qWait(100);
    ScreenShotManager manager;
    return manager.takeScreenShot(workspace()->outputs().front(), ScreenShotFlags(), std::nullopt);
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

void StereoDepthNotificationsTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));

    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    KConfigGroup depth(config, QStringLiteral("StereoDepth"));
    depth.writeEntry("SunkLimit", s_sunkLimit);
    depth.writeEntry("PoppedLimit", s_poppedLimit);
    // no motion between the levels: every capture is at the levels themselves
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", 0);
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();
}

void StereoDepthNotificationsTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat | Test::AdditionalWaylandInterface::PlasmaShell));
    QVERIFY(stereoOutput(true));
    options->setStereoSunkLimit(s_sunkLimit);
    options->setStereoPoppedLimit(s_poppedLimit);
}

void StereoDepthNotificationsTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void StereoDepthNotificationsTest::testEachKindIsPopped_data()
{
    QTest::addColumn<int>("role");
    QTest::newRow("notification") << int(Role::Notification);
    QTest::newRow("critical notification") << int(Role::CriticalNotification);
    QTest::newRow("on-screen display") << int(Role::OnScreenDisplay);
}

void StereoDepthNotificationsTest::testEachKindIsPopped()
{
    QFETCH(int, role);
    auto box = createShellBox(Role(role), Qt::red, QPoint(200, 50));
    QVERIFY(box->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *box, 60), s_popped);
}

void StereoDepthNotificationsTest::testSeveralNotifications()
{
    // a stack of notifications, one above the other and never overlapping: each at the popped limit,
    // not a share of it
    auto first = createShellBox(Role::Notification, Qt::red, QPoint(240, 10), QSize(60, 30));
    auto second = createShellBox(Role::Notification, Qt::green, QPoint(240, 50), QSize(60, 30));
    auto third = createShellBox(Role::Notification, Qt::blue, QPoint(240, 90), QSize(60, 30));
    QVERIFY(first->window && second->window && third->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *first, 20), s_popped);
    QCOMPARE(parallaxOf(*image, *second, 60), s_popped);
    QCOMPARE(parallaxOf(*image, *third, 100), s_popped);
}

void StereoDepthNotificationsTest::testCriticalAndNormal()
{
    auto normal = createShellBox(Role::Notification, Qt::red, QPoint(240, 10), QSize(60, 30));
    auto critical = createShellBox(Role::CriticalNotification, Qt::green, QPoint(240, 50), QSize(60, 30));
    QVERIFY(normal->window && critical->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *critical, 60), s_popped);
    QCOMPARE(parallaxOf(*image, *normal, 20), s_popped);
}

void StereoDepthNotificationsTest::testWithOnScreenDisplay()
{
    auto notification = createShellBox(Role::Notification, Qt::red, QPoint(240, 10), QSize(60, 30));
    auto osd = createShellBox(Role::OnScreenDisplay, Qt::green, QPoint(130, 190), QSize(60, 30));
    QVERIFY(notification->window && osd->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *notification, 20), s_popped);
    QCOMPARE(parallaxOf(*image, *osd, 200), s_popped);
}

void StereoDepthNotificationsTest::testMenusKeepTheirShare()
{
    // the panel's popup and its menu on top of it share the popped limit as they did: the popup
    // below, the menu at the limit; a notification takes none of that share and lies at the limit
    auto popup = createShellBox(Role::AppletPopup, Qt::red, QPoint(40, 40), QSize(120, 120));
    QVERIFY(popup->window);
    auto notification = createShellBox(Role::Notification, Qt::yellow, QPoint(240, 10), QSize(60, 30));
    QVERIFY(notification->window);
    const auto alone = [&]() {
        const auto image = capture();
        return image ? std::optional<int>(parallaxOf(*image, *popup, 60)) : std::nullopt;
    };
    const auto withNotification = alone();
    QVERIFY(withNotification);
    notification.reset();
    QTRY_VERIFY(workspace()->windows().size() == 1);
    const auto without = alone();
    QVERIFY(without);
    // the popup is the only one that shares: at the limit with or without the notification
    QCOMPARE(*without, s_popped);
    QCOMPARE(*withNotification, s_popped);
}

void StereoDepthNotificationsTest::testOverAPinnedWindow()
{
    // a window kept above the others is at the popped limit; a notification shown over it is
    // further out, one step beyond the limit (the pinned stack, stereo_depth_pinned_stack_test.cpp)
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(120, 120), Qt::red);
    QVERIFY(window);
    window->move(QPointF(100, 90));
    window->setKeepAbove(true);
    auto notification = createShellBox(Role::Notification, Qt::green, QPoint(110, 100), QSize(60, 30));
    QVERIFY(notification->window);
    const auto image = capture();
    QVERIFY(image);
    TestBox pinned;
    pinned.color = Qt::red;
    QCOMPARE(parallaxOf(*image, pinned, 190), s_popped);
    QCOMPARE(parallaxOf(*image, *notification, 110), 2 * s_popped);
}

void StereoDepthNotificationsTest::testWithinThePoppedLimit()
{
    // the person's own popped limit rules: half of it, half the parallax
    options->setStereoPoppedLimit(s_poppedLimit / 2);
    auto notification = createShellBox(Role::Notification, Qt::red, QPoint(200, 50));
    QVERIFY(notification->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *notification, 60), s_popped / 2);
}

void StereoDepthNotificationsTest::testTwoDimensionalOutputIsUnchanged()
{
    QVERIFY(stereoOutput(false));
    auto first = createShellBox(Role::Notification, Qt::red, QPoint(240, 10), QSize(60, 30));
    auto osd = createShellBox(Role::OnScreenDisplay, Qt::green, QPoint(130, 190), QSize(60, 30));
    QVERIFY(first->window && osd->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    QCOMPARE(firstX(*image, 0, 20, Qt::red), 240);
    QCOMPARE(firstX(*image, 0, 200, Qt::green), 130);
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthNotificationsTest)
#include "stereo_depth_notifications_test.moc"
