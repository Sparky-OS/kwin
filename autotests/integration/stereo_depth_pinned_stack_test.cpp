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
 * What opens over a window kept above the others on a stereo output. The pinned window stays at
 * the person's popped limit; its menus, sub-menus and the notifications and on-screen displays
 * shown over it stack further out, one step per layer, the layers sharing one more limit (Daniel,
 * 2026-11-07: "keep the poped max for pinned, anything inside it stacks even more far from the
 * screen"). Everything else keeps its level: the controls here are a pinned window alone, the
 * menus of a window that is not pinned, a notification away from the pinned window, a pinned
 * window on the output's edge and the person's own half limit. The depth is read from a capture
 * of the output: the parallax of a box is the horizontal distance of its colour in the left
 * eye's half to the same colour in the right eye's half.
 */
class StereoDepthPinnedStackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testPinnedAlone();
    void testOneMenu();
    void testMenuAndSubMenu();
    void testMenuSubMenuAndNotification();
    void testNotificationBelowTheMenus();
    void testOnScreenDisplayOverPinned();
    void testNotificationAwayFromPinned();
    void testMenusOfAWindowThatIsNotPinned();
    void testPinnedOnTheEdge();
    void testWithinThePoppedLimit();
    void testUnpinnedMenuReturnsToTheShare();
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
    std::unique_ptr<Test::XdgPopup> popup;
    Window *window = nullptr;
    QColor color;

    Test::XdgSurface *xdgSurface() const
    {
        return toplevel ? toplevel->xdgSurface() : popup->xdgSurface();
    }
};

// a window of the program, at @a position
static std::unique_ptr<TestBox> createWindow(const QColor &color, const QPoint &position, const QSize &size)
{
    auto result = std::make_unique<TestBox>();
    result->color = color;
    result->surface = Test::createSurface();
    result->toplevel = Test::createXdgToplevelSurface(result->surface.get());
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, color);
    if (result->window) {
        result->window->move(QPointF(position));
    }
    return result;
}

// a menu at @a position in the parent's own coordinates
static std::unique_ptr<TestBox> createMenu(const TestBox &parent, const QColor &color, const QPoint &position, const QSize &size)
{
    auto result = std::make_unique<TestBox>();
    result->color = color;
    result->surface = Test::createSurface();
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(size.width(), size.height());
    positioner->set_anchor_rect(position.x(), position.y(), 1, 1);
    positioner->set_anchor(Test::XdgPositioner::anchor_top_left);
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    result->popup = Test::createXdgPopupSurface(result->surface.get(), parent.xdgSurface(), positioner.get());
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, color);
    return result;
}

// a surface of the panel shell, placed by its program at @a position
static std::unique_ptr<TestBox> createShellBox(Role role, const QColor &color, const QPoint &position, const QSize &size = QSize(60, 30))
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

void StereoDepthPinnedStackTest::initTestCase()
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

void StereoDepthPinnedStackTest::init()
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

void StereoDepthPinnedStackTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

// the pinned window of every case: red at 100,90, 120x120, kept above the others
static std::unique_ptr<TestBox> createPinned(const QPoint &position = QPoint(100, 90))
{
    auto pinned = createWindow(Qt::red, position, QSize(120, 120));
    if (pinned->window) {
        pinned->window->setKeepAbove(true);
    }
    return pinned;
}

static int parallaxAt(const TestBox &box, int y)
{
    const auto image = capture();
    return image ? parallaxOf(*image, box, y) : std::numeric_limits<int>::min();
}

void StereoDepthPinnedStackTest::testPinnedAlone()
{
    // a control: the pinned window is at the person's popped limit, as before
    auto pinned = createPinned();
    QVERIFY(pinned->window && pinned->window->keepAbove());
    QCOMPARE(parallaxAt(*pinned, 150), s_popped);
}

void StereoDepthPinnedStackTest::testOneMenu()
{
    // one layer takes the whole span beyond the limit: two limits
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    QVERIFY(pinned->window && menu->window);
    QCOMPARE(menu->window->frameGeometry(), RectF(120, 110, 60, 40));
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *pinned, 200), s_popped);
    QCOMPARE(parallaxOf(*image, *menu, 130), 2 * s_popped);
}

void StereoDepthPinnedStackTest::testMenuAndSubMenu()
{
    // two layers: the menu a half step out, the sub-menu a whole span out
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    auto subMenu = createMenu(*menu, Qt::blue, QPoint(50, 10), QSize(60, 40));
    QVERIFY(pinned->window && menu->window && subMenu->window);
    QCOMPARE(subMenu->window->frameGeometry(), RectF(170, 120, 60, 40));
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *pinned, 200), s_popped);
    QCOMPARE(parallaxOf(*image, *menu, 112), 15);
    QCOMPARE(parallaxOf(*image, *subMenu, 155), 2 * s_popped);
}

void StereoDepthPinnedStackTest::testMenuSubMenuAndNotification()
{
    // three layers, each nearer than the one under it: 1 1/3, 1 2/3 and 2 limits. A critical
    // notification is stacked over the menus
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    auto subMenu = createMenu(*menu, Qt::blue, QPoint(50, 10), QSize(60, 40));
    auto notification = createShellBox(Role::CriticalNotification, Qt::yellow, QPoint(110, 170));
    QVERIFY(pinned->window && menu->window && subMenu->window && notification->window);
    const auto image = capture();
    QVERIFY(image);
    const int pinnedParallax = parallaxOf(*image, *pinned, 205);
    const int menuParallax = parallaxOf(*image, *menu, 112);
    const int subMenuParallax = parallaxOf(*image, *subMenu, 155);
    const int notificationParallax = parallaxOf(*image, *notification, 185);
    QCOMPARE(pinnedParallax, s_popped);
    QCOMPARE(menuParallax, 13);
    QCOMPARE(subMenuParallax, 17);
    QCOMPARE(notificationParallax, 2 * s_popped);
    QVERIFY(pinnedParallax < menuParallax && menuParallax < subMenuParallax && subMenuParallax < notificationParallax);
}

void StereoDepthPinnedStackTest::testNotificationBelowTheMenus()
{
    // a normal notification is stacked under the popups, so depth follows the stacking: it is the
    // first layer over the pinned window and the menus are further out
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    auto subMenu = createMenu(*menu, Qt::blue, QPoint(50, 10), QSize(60, 40));
    auto notification = createShellBox(Role::Notification, Qt::yellow, QPoint(110, 170));
    QVERIFY(pinned->window && menu->window && subMenu->window && notification->window);
    const QList<Window *> stack = workspace()->stackingOrder();
    QVERIFY(stack.indexOf(notification->window) < stack.indexOf(menu->window));
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *notification, 185), 13);
    QCOMPARE(parallaxOf(*image, *menu, 112), 17);
    QCOMPARE(parallaxOf(*image, *subMenu, 155), 2 * s_popped);
}

void StereoDepthPinnedStackTest::testOnScreenDisplayOverPinned()
{
    auto pinned = createPinned();
    auto osd = createShellBox(Role::OnScreenDisplay, Qt::green, QPoint(130, 160));
    QVERIFY(pinned->window && osd->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *pinned, 205), s_popped);
    QCOMPARE(parallaxOf(*image, *osd, 175), 2 * s_popped);
}

void StereoDepthPinnedStackTest::testNotificationAwayFromPinned()
{
    // a control: a notification that is not over the pinned window stays at the limit
    auto pinned = createPinned(QPoint(20, 90));
    auto notification = createShellBox(Role::Notification, Qt::green, QPoint(240, 10));
    QVERIFY(pinned->window && notification->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *pinned, 150), s_popped);
    QCOMPARE(parallaxOf(*image, *notification, 20), s_popped);
}

void StereoDepthPinnedStackTest::testMenusOfAWindowThatIsNotPinned()
{
    // a control: menus of an ordinary window share the popped limit as before, none beyond it
    auto window = createWindow(Qt::red, QPoint(100, 90), QSize(120, 120));
    QVERIFY(window->window);
    workspace()->activateWindow(window->window);
    auto menu = createMenu(*window, Qt::green, QPoint(20, 20), QSize(60, 40));
    auto subMenu = createMenu(*menu, Qt::blue, QPoint(50, 10), QSize(60, 40));
    auto notification = createShellBox(Role::Notification, Qt::yellow, QPoint(110, 170));
    QVERIFY(menu->window && subMenu->window && notification->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *menu, 112), s_popped / 2);
    QCOMPARE(parallaxOf(*image, *subMenu, 155), s_popped);
    QCOMPARE(parallaxOf(*image, *notification, 185), s_popped);
}

void StereoDepthPinnedStackTest::testPinnedOnTheEdge()
{
    // a control: the edge rule holds, the pinned window on the output's edge is at the screen and
    // what opens over it is not stacked beyond the limit
    auto pinned = createPinned(QPoint(0, 90));
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    QVERIFY(pinned->window && menu->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *pinned, 200), 0);
    QVERIFY(parallaxOf(*image, *menu, 130) <= s_popped);
}

void StereoDepthPinnedStackTest::testWithinThePoppedLimit()
{
    // the person's own popped limit rules: half of it, the pinned window at half the parallax
    options->setStereoPoppedLimit(s_poppedLimit / 2);
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    QVERIFY(pinned->window && menu->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *pinned, 200), s_popped / 2);
    QCOMPARE(parallaxOf(*image, *menu, 130), s_popped);
}

void StereoDepthPinnedStackTest::testUnpinnedMenuReturnsToTheShare()
{
    // taking the pin away puts the menu back in the popped share, at the limit
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    QVERIFY(pinned->window && menu->window);
    QCOMPARE(parallaxAt(*menu, 130), 2 * s_popped);
    pinned->window->setKeepAbove(false);
    QCOMPARE(parallaxAt(*menu, 130), s_popped);
}

void StereoDepthPinnedStackTest::testTwoDimensionalOutputIsUnchanged()
{
    QVERIFY(stereoOutput(false));
    auto pinned = createPinned();
    auto menu = createMenu(*pinned, Qt::green, QPoint(20, 20), QSize(60, 40));
    QVERIFY(pinned->window && menu->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    QCOMPARE(firstX(*image, 0, 200, Qt::red), 100);
    QCOMPARE(firstX(*image, 0, 130, Qt::green), 120);
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthPinnedStackTest)
#include "stereo_depth_pinned_stack_test.moc"
