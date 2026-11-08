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

#include <cmath>

namespace KWin
{

/**
 * The levels of the window stack and of a chain of menus on a stereo output (ORDER 464). The windows beneath the active one step back
 * from the screen in their stacking order a layer each, a layer being a fifth of the sunk limit: the first right behind the
 * screen, and when they fill the range the rest share the limit, never behind the wallpaper (+39 and +46 shared the range among them, so the one
 * window beneath the active one was at the limit, the wallpaper's depth: Daniel, 2026-11-08, "unselected window dropping to the
 * bottom when two windows are present"). A chain of n menus (and a sub-menu larger than the output room) puts level n at n popped
 * limits from its base, the screen for a plain window and the window's own level for one kept above the others, whatever the length
 * of the chain (+39 and +46 shared the limit among the chain, which moved a parent when a sub-menu opened: Daniel, 2026-11-08, "two
 * menu levels will yeld twice the separation from the screen, three levels the same"). The depth is read from a capture: the
 * parallax of a box is the horizontal distance of its colour in the left eye's half to the same colour in the right eye's half.
 */
class StereoDepthStackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testOneWindow();
    void testTwoWindows();
    void testThreeWindows();
    void testSixWindows();
    void testTwelveWindows();
    void testMenuChains_data();
    void testMenuChains();
    void testDialogChains_data();
    void testDialogChains();
    void testSubMenuLargerThanTheRoomKeepsItsLevel();
};

static const QSize s_outputSize(320, 240);
// limits in pixels at 1920 wide, scaled to the 320 wide output: sunk 16 and popped 10 pixels
static constexpr qreal s_sunkLimit = 96;
static constexpr qreal s_poppedLimit = 60;
static constexpr int s_popped = 10;
static constexpr int s_sunk = 16;

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

static int firstX(const QImage &image, int xOffset, int y, const QColor &color)
{
    for (int x = 0; x < s_outputSize.width(); ++x) {
        const QColor pixel = image.pixelColor(xOffset + x, y);
        if (std::abs(pixel.red() - color.red()) <= 3 && std::abs(pixel.green() - color.green()) <= 3 && std::abs(pixel.blue() - color.blue()) <= 3) {
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

static std::unique_ptr<TestBox> createBand(int index, int count, const QColor &color)
{
    // a window of the cascade: the visible band of each is its top rows
    return createWindow(color, QPoint(40 + 4 * index, 2 + (s_outputSize.height() - 40) * index / std::max(1, count - 1)), QSize(140, 36));
}

static QColor colorOf(int index)
{
    return QColor::fromHsv((index * 29) % 360, 255, 255);
}

// the parallax of the window at @a index of a cascade of @a count windows, the last of them active
static void checkStack(int count)
{
    std::vector<std::unique_ptr<TestBox>> windows;
    for (int i = 0; i < count; ++i) {
        windows.push_back(createBand(i, count, colorOf(i)));
        QVERIFY(windows.back()->window);
    }
    workspace()->activateWindow(windows.back()->window);
    QVERIFY(Test::waylandSync());
    QCOMPARE(workspace()->activeWindow(), windows.back()->window);
    const auto image = capture();
    QVERIFY(image);
    for (int i = 0; i < count; ++i) {
        // the windows beneath the active one step back a layer each, a fifth of the limit, the first right behind the screen
        const int rank = count - 1 - i;
        const int expected = rank == 0 ? 0 : -std::lround(qreal(s_sunk) * std::min(rank, 5) / 5);
        const int y = windows[i]->window->frameGeometry().top() + 1;
        QVERIFY2(parallaxOf(*image, *windows[i], y) == expected,
                 qPrintable(QStringLiteral("%1 windows, window %2: parallax %3, expected %4").arg(count).arg(i).arg(parallaxOf(*image, *windows[i], y)).arg(expected)));
    }
}

void StereoDepthStackTest::testOneWindow()
{
    // a control: a window that is alone is at the screen
    auto window = createBand(0, 1, colorOf(0));
    QVERIFY(window->window);
    workspace()->activateWindow(window->window);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(parallaxOf(*image, *window, window->window->frameGeometry().top() + 1), 0);
}

void StereoDepthStackTest::testTwoWindows()
{
    // the window beneath the active one is the whole sunk limit, as in +39
    checkStack(2);
}

void StereoDepthStackTest::testThreeWindows()
{
    checkStack(3);
}

void StereoDepthStackTest::testSixWindows()
{
    checkStack(6);
}

void StereoDepthStackTest::testTwelveWindows()
{
    checkStack(12);
}

void StereoDepthStackTest::testMenuChains_data()
{
    QTest::addColumn<int>("levels");
    QTest::addColumn<bool>("pinned");
    for (int levels = 1; levels <= 4; ++levels) {
        QTest::newRow(qPrintable(QStringLiteral("plain, %1 levels").arg(levels))) << levels << false;
        QTest::newRow(qPrintable(QStringLiteral("kept above, %1 levels").arg(levels))) << levels << true;
    }
}

void StereoDepthStackTest::testMenuChains()
{
    QFETCH(int, levels);
    QFETCH(bool, pinned);
    // Daniel, 2026-11-08: "two menu levels will yeld twice the separation from the screen, three levels the same". Level n is n popped
    // limits from its base, the screen for a plain window and the window's own level for one kept above the others, whatever the length
    // of the chain; a menu opened alone is at the limit; opening a sub-menu moves nothing above it; each level is above its parent.
    auto window = createWindow(Qt::gray, QPoint(10, 10), QSize(200, 200));
    QVERIFY(window->window);
    workspace()->activateWindow(window->window);
    if (pinned) {
        window->window->setKeepAbove(true);
    }
    const int base = pinned ? s_popped : 0;
    std::vector<std::unique_ptr<TestBox>> menus;
    const TestBox *parent = window.get();
    for (int i = 0; i < levels; ++i) {
        menus.push_back(createMenu(*parent, colorOf(i), QPoint(20 + 30 * i, 20 + 6 * i), QSize(28, 24)));
        QVERIFY(menus.back()->window);
        parent = menus.back().get();
        const auto image = capture();
        QVERIFY(image);
        for (int j = 0; j <= i; ++j) {
            const int expected = base + (j + 1) * s_popped;
            const int parallax = parallaxOf(*image, *menus[j], menus[j]->window->frameGeometry().top() + 2);
            QVERIFY2(parallax == expected, qPrintable(QStringLiteral("%1 menus, menu %2 of %3 open: parallax %4, expected %5").arg(levels).arg(j).arg(i + 1).arg(parallax).arg(expected)));
            if (j > 0) {
                const QList<Window *> &stack = workspace()->stackingOrder();
                QVERIFY(stack.indexOf(menus[j]->window) > stack.indexOf(menus[j - 1]->window));
            }
        }
        if (pinned) {
            QCOMPARE(parallaxOf(*image, *window, 190), s_popped);
        }
    }
}

// a dialog of @a parent: a top level with a parent, which pops over the window like a menu
static std::unique_ptr<TestBox> createDialog(const TestBox &parent, const QColor &color, const QPoint &position, const QSize &size)
{
    auto result = std::make_unique<TestBox>();
    result->color = color;
    result->surface = Test::createSurface();
    result->toplevel = Test::createXdgToplevelSurface(result->surface.get());
    result->toplevel->set_parent(parent.toplevel->object());
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, color);
    if (result->window) {
        result->window->move(QPointF(position));
    }
    return result;
}

void StereoDepthStackTest::testDialogChains_data()
{
    QTest::addColumn<int>("menus");
    QTest::addColumn<bool>("pinned");
    for (int menus = 1; menus <= 2; ++menus) {
        QTest::newRow(qPrintable(QStringLiteral("plain, dialog and %1 menu(s)").arg(menus))) << menus << false;
        QTest::newRow(qPrintable(QStringLiteral("kept above, dialog and %1 menu(s)").arg(menus))) << menus << true;
    }
}

void StereoDepthStackTest::testDialogChains()
{
    QFETCH(int, menus);
    QFETCH(bool, pinned);
    // A dialog is a level of the chain, so a combo box's list or a menu opened in it, and the sub-menu of that, are the levels beyond it: the
    // dialog one limit from its base, the list two, the sub-menu three, and not flush with the dialog (the dialog is no popup in
    // isPopup()'s sense but is popped over its window)
    auto window = createWindow(Qt::gray, QPoint(10, 10), QSize(200, 200));
    QVERIFY(window->window);
    workspace()->activateWindow(window->window);
    if (pinned) {
        window->window->setKeepAbove(true);
    }
    const int base = pinned ? s_popped : 0;
    auto dialog = createDialog(*window, colorOf(5), QPoint(40, 60), QSize(120, 100));
    QVERIFY(dialog->window);
    std::vector<std::unique_ptr<TestBox>> chain;
    const TestBox *parent = dialog.get();
    for (int i = 0; i < menus; ++i) {
        chain.push_back(createMenu(*parent, colorOf(i), QPoint(10 + 30 * i, 8 + 6 * i), QSize(28, 24)));
        QVERIFY(chain.back()->window);
        parent = chain.back().get();
    }
    const auto image = capture();
    QVERIFY(image);
    const int dialogParallax = parallaxOf(*image, *dialog, dialog->window->frameGeometry().bottom() - 4);
    QVERIFY2(dialogParallax == base + s_popped, qPrintable(QStringLiteral("dialog: parallax %1, expected %2").arg(dialogParallax).arg(base + s_popped)));
    for (int i = 0; i < menus; ++i) {
        const int expected = base + (i + 2) * s_popped;
        const int parallax = parallaxOf(*image, *chain[i], chain[i]->window->frameGeometry().top() + 2);
        QVERIFY2(parallax == expected, qPrintable(QStringLiteral("menu %1 in the dialog: parallax %2, expected %3").arg(i).arg(parallax).arg(expected)));
        QVERIFY(parallax > dialogParallax);
    }
}

void StereoDepthStackTest::testSubMenuLargerThanTheRoomKeepsItsLevel()
{
    // a sub-menu taller than the output room inside the 10 pixel border cannot fit; it is capped to the border and keeps its level, a
    // step nearer than its menu and above it (Daniel, 2026-11-08: "capped to the same 10px invisible fence ... keeps its depth level"),
    // where the edge rule dropped it to the screen's level, behind its parent
    auto window = createWindow(Qt::gray, QPoint(10, 10), QSize(200, 200));
    QVERIFY(window->window);
    workspace()->activateWindow(window->window);
    auto menu = createMenu(*window, Qt::green, QPoint(20, 20), QSize(60, 40));
    auto sub = createMenu(*menu, Qt::red, QPoint(60, 0), QSize(60, s_outputSize.height()));
    QVERIFY(menu->window && sub->window);
    QVERIFY(sub->window->frameGeometry().height() > s_outputSize.height() - 20);
    const auto image = capture();
    QVERIFY(image);
    const int menuParallax = parallaxOf(*image, *menu, 50);
    const int subParallax = parallaxOf(*image, *sub, 120);
    QCOMPARE(menuParallax, s_popped);
    QCOMPARE(subParallax, 2 * s_popped);
    const QList<Window *> &stack = workspace()->stackingOrder();
    QVERIFY(stack.indexOf(sub->window) > stack.indexOf(menu->window));
}

void StereoDepthStackTest::initTestCase()
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

void StereoDepthStackTest::init()
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

void StereoDepthStackTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthStackTest)
#include "stereo_depth_stack_test.moc"
