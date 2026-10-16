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
#include <KWayland/Client/pointer.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/surface.h>

namespace KWin
{

/**
 * A menu opened from a panel's popup, and a sub-menu opened from that menu, on a stereo output. A
 * sub-menu never sits behind the menu it came from: it is nearer, or at the same level when the
 * menu is already at the popped limit. The depth is read from a capture of the output: the parallax
 * of a popup is the horizontal distance of its colour in the left eye's half to the same colour in
 * the right eye's half.
 */
class StereoDepthPopupsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testSubMenu_data();
    void testSubMenu();
    void testHoverRaise();
    void testDanielsLimit();
    void testTwoDimensionalOutputIsUnchanged();
};

static QSize s_outputSize(320, 240);
// limits in pixels at 1920 wide, scaled to the 320 wide output: sunk 16 and popped 10 pixels
static constexpr qreal s_sunkLimit = 96;
static constexpr qreal s_poppedLimit = 60;
static constexpr int s_popped = 10;
// Daniel's own popped limit, on a 1920 wide output as his
static constexpr qreal s_danielsPoppedLimit = 5;

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

struct TestPopup
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

// the panel's own popup, at a place of its own
static std::unique_ptr<TestPopup> createAppletPopup(const QColor &color, const Rect &geometry)
{
    auto result = std::make_unique<TestPopup>();
    result->color = color;
    result->surface = Test::createSurface();
    result->toplevel = Test::createXdgToplevelSurface(result->surface.get());
    result->plasmaSurface.reset(Test::waylandPlasmaShell()->createSurface(result->surface.get()));
    result->plasmaSurface->setRole(KWayland::Client::PlasmaShellSurface::Role::AppletPopup);
    result->window = Test::renderAndWaitForShown(result->surface.get(), geometry.size(), color);
    if (result->window) {
        result->window->move(geometry.topLeft());
    }
    return result;
}

// a menu at @a position in the parent's own coordinates
static std::unique_ptr<TestPopup> createMenu(const TestPopup &parent, const QColor &color, const QPoint &position, const QSize &size)
{
    auto result = std::make_unique<TestPopup>();
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

// the parallax of the popup's colour on the row at y, left eye minus right eye
static int parallaxOf(const QImage &image, const TestPopup &popup, int y)
{
    const int left = firstX(image, 0, y, popup.color);
    const int right = firstX(image, s_outputSize.width(), y, popup.color);
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
    // the levels are reached in a short motion; animations are off here, so at once
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

void StereoDepthPopupsTest::initTestCase()
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

void StereoDepthPopupsTest::init()
{
    const bool daniels = qstrcmp(QTest::currentTestFunction(), "testDanielsLimit") == 0;
    s_outputSize = daniels ? QSize(1920, 1080) : QSize(320, 240);
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat | Test::AdditionalWaylandInterface::PlasmaShell));
    QVERIFY(Test::waitForWaylandPointer());
    QVERIFY(stereoOutput(true));
    options->setStereoSunkLimit(s_sunkLimit);
    options->setStereoPoppedLimit(daniels ? s_danielsPoppedLimit : s_poppedLimit);
}

void StereoDepthPopupsTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
    options->setFocusPolicy(Options::ClickToFocus);
}

void StereoDepthPopupsTest::testSubMenu_data()
{
    QTest::addColumn<bool>("subMenuOfMenu");
    QTest::addColumn<bool>("raiseMenu");
    QTest::addColumn<int>("overlap");

    // the sub-menu's own parent: the menu, or the panel's popup with the menu beside it
    QTest::newRow("of menu") << true << false << 0;
    QTest::newRow("of menu, menu raised") << true << true << 0;
    QTest::newRow("of menu, overlapping") << true << false << 6;
    QTest::newRow("of popup") << false << false << 0;
    QTest::newRow("of popup, menu raised") << false << true << 0;
    QTest::newRow("of popup, menu raised, overlapping") << false << true << 6;
}

void StereoDepthPopupsTest::testSubMenu()
{
    QFETCH(bool, subMenuOfMenu);
    QFETCH(bool, raiseMenu);
    QFETCH(int, overlap);

    // the panel's popup at 40,40 120x120, the menu at 80,90 60x60, the sub-menu to its right
    auto applet = createAppletPopup(Qt::red, Rect(40, 40, 120, 120));
    QVERIFY(applet->window);
    auto menu = createMenu(*applet, Qt::green, QPoint(40, 50), QSize(60, 60));
    QVERIFY(menu->window);
    QCOMPARE(menu->window->frameGeometry(), RectF(80, 90, 60, 60));
    std::unique_ptr<TestPopup> subMenu;
    if (subMenuOfMenu) {
        subMenu = createMenu(*menu, Qt::blue, QPoint(60 - overlap, 20), QSize(60, 60));
    } else {
        subMenu = createMenu(*applet, Qt::blue, QPoint(100 - overlap, 70), QSize(60, 60));
    }
    QVERIFY(subMenu->window);
    QCOMPARE(subMenu->window->frameGeometry(), RectF(140 - overlap, 110, 60, 60));
    if (raiseMenu) {
        // what an auto-raise does when the pointer rests in the menu
        workspace()->raiseWindow(menu->window);
        QVERIFY(Test::waylandSync());
    }

    const auto image = capture();
    QVERIFY(image);
    const int appletParallax = parallaxOf(*image, *applet, 60);
    const int menuParallax = parallaxOf(*image, *menu, 100);
    const int subMenuParallax = parallaxOf(*image, *subMenu, 160);
    const QList<Window *> stack = workspace()->stackingOrder();
    qInfo() << "measured popup" << appletParallax << "menu" << menuParallax << "sub-menu" << subMenuParallax
            << "menu below sub-menu in the stack:" << (stack.indexOf(menu->window) < stack.indexOf(subMenu->window));
    QVERIFY(appletParallax > 0);
    QVERIFY(menuParallax >= appletParallax);
    // never behind the menu it came from, and at the popped limit at most
    QVERIFY(subMenuParallax >= menuParallax);
    QVERIFY(subMenuParallax <= s_popped);
    QCOMPARE(std::max({appletParallax, menuParallax, subMenuParallax}), s_popped);
}

void StereoDepthPopupsTest::testHoverRaise()
{
    // focus follows the mouse and raises what it rests on, as on Daniel's desktop: resting in the
    // menu raises the menu, the sub-menu stays in front of it
    options->setFocusPolicy(Options::FocusFollowsMouse);
    options->setAutoRaise(true);
    options->setAutoRaiseInterval(50);
    options->setDelayFocusInterval(40);

    auto applet = createAppletPopup(Qt::red, Rect(40, 40, 120, 120));
    QVERIFY(applet->window);
    auto menu = createMenu(*applet, Qt::green, QPoint(40, 50), QSize(60, 60));
    QVERIFY(menu->window);
    auto subMenu = createMenu(*menu, Qt::blue, QPoint(60, 20), QSize(60, 60));
    QVERIFY(subMenu->window);

    quint32 time = 0;
    Test::pointerMotion(QPointF(20, 20), ++time);
    Test::pointerMotion(QPointF(100, 100), ++time);
    Test::pointerMotion(QPointF(101, 101), ++time);
    QTest::qWait(300);

    const auto image = capture();
    QVERIFY(image);
    const int appletParallax = parallaxOf(*image, *applet, 60);
    const int menuParallax = parallaxOf(*image, *menu, 100);
    const int subMenuParallax = parallaxOf(*image, *subMenu, 160);
    qInfo() << "measured after resting in the menu: popup" << appletParallax << "menu" << menuParallax << "sub-menu" << subMenuParallax
            << "active" << workspace()->activeWindow() << "stack" << workspace()->stackingOrder();
    QVERIFY(menuParallax > 0);
    QVERIFY(subMenuParallax >= menuParallax);
}

void StereoDepthPopupsTest::testDanielsLimit()
{
    // his measurement of 15 October: the panel's menu +5, its sub-menu +3, at a popped limit of 5
    auto applet = createAppletPopup(Qt::red, Rect(40, 40, 120, 120));
    QVERIFY(applet->window);
    auto menu = createMenu(*applet, Qt::green, QPoint(40, 50), QSize(60, 60));
    QVERIFY(menu->window);
    auto subMenu = createMenu(*applet, Qt::blue, QPoint(100, 70), QSize(60, 60));
    QVERIFY(subMenu->window);
    workspace()->raiseWindow(menu->window);
    QVERIFY(Test::waylandSync());

    const auto image = capture();
    QVERIFY(image);
    const int appletParallax = parallaxOf(*image, *applet, 60);
    const int menuParallax = parallaxOf(*image, *menu, 100);
    const int subMenuParallax = parallaxOf(*image, *subMenu, 160);
    qInfo() << "measured at Daniel's limit: popup" << appletParallax << "menu" << menuParallax << "sub-menu" << subMenuParallax;
    QCOMPARE(menuParallax, 5);
    QCOMPARE(subMenuParallax, 5);
}

void StereoDepthPopupsTest::testTwoDimensionalOutputIsUnchanged()
{
    QVERIFY(stereoOutput(false));
    auto applet = createAppletPopup(Qt::red, Rect(40, 40, 120, 120));
    QVERIFY(applet->window);
    auto menu = createMenu(*applet, Qt::green, QPoint(40, 50), QSize(60, 60));
    QVERIFY(menu->window);
    auto subMenu = createMenu(*applet, Qt::blue, QPoint(100, 70), QSize(60, 60));
    QVERIFY(subMenu->window);
    workspace()->raiseWindow(menu->window);

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    QCOMPARE(firstX(*image, 0, 60, Qt::red), 40);
    QCOMPARE(firstX(*image, 0, 100, Qt::green), 80);
    QCOMPARE(firstX(*image, 0, 160, Qt::blue), 140);
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthPopupsTest)
#include "stereo_depth_popups_test.moc"
