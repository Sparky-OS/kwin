/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "effect/globals.h"
#include "options.h"
#include "pointer_input.h"
#include "scene/stereodepth.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/surface.h>

#include <functional>

namespace KWin
{

/**
 * kde_stereo_depth_v1: a program follows a surface's desktop depth. Stereo KWin says whether the
 * elements drawn inside the surface may be shown in depth now (awake, on a stereo output) and which
 * of its edges lie on its output's edge; the program marks its pair as the desktop's own depth and
 * declares where its popups sit.
 */
class StereoDepthExchangeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testActiveWindowIsAwake();
    void testPileIsAsleep();
    void testTwoDimensionalOutputIsAsleep();
    void testOutputEdges();
    void testFollowedBeforeShown();
    void testManyFollowers();
    void testPopupIsAwake();
    void testDesktopDepthIsNotStereoContent();
    void testDeclaredPlace();
    void testInertWithoutSurface();
};

static const QSize s_outputSize(1280, 1024);

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

struct TestWindow
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> toplevel;
    std::unique_ptr<Test::XdgPopup> popup;
    Window *window = nullptr;
};

static std::unique_ptr<TestWindow> createWindow(const QPointF &position, const QSize &size = QSize(100, 50))
{
    auto result = std::make_unique<TestWindow>();
    result->surface = Test::createSurface();
    result->toplevel = Test::createXdgToplevelSurface(result->surface.get());
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, Qt::blue);
    if (result->window) {
        result->window->move(position);
    }
    return result;
}

static std::unique_ptr<TestWindow> createPopup(Test::XdgSurface *parent, const QPoint &position, const QSize &size = QSize(60, 60))
{
    auto result = std::make_unique<TestWindow>();
    result->surface = Test::createSurface();
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(size.width(), size.height());
    positioner->set_anchor_rect(position.x(), position.y(), 1, 1);
    positioner->set_anchor(Test::XdgPositioner::anchor_top_left);
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    result->popup = Test::createXdgPopupSurface(result->surface.get(), parent, positioner.get());
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, Qt::green);
    return result;
}

// KWin flushes what it sends to clients when its event loop is about to block, so the wait runs
// an event loop rather than polling.
static bool waitFor(Test::StereoDepthV1 *depth, const std::function<bool()> &condition)
{
    for (int i = 0; i < 20 && !condition(); ++i) {
        QSignalSpy received(depth, &Test::StereoDepthV1::stateReceived);
        received.wait(250);
    }
    return condition();
}

static std::unique_ptr<Test::StereoDepthV1> follow(KWayland::Client::Surface *surface)
{
    return std::make_unique<Test::StereoDepthV1>(Test::stereoContentManager()->get_depth(*surface));
}

static bool commit(KWayland::Client::Surface *surface, Window *window)
{
    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    return committed.wait();
}

void StereoDepthExchangeTest::initTestCase()
{
    qRegisterMetaType<Window *>();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
}

void StereoDepthExchangeTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::StereoContentV1));
    QCOMPARE(Test::stereoContentManager()->version(), 3u);
    QVERIFY(stereoOutput(true));
    options->setStereoSunkLimit(10);
    options->setStereoPoppedLimit(4);
    workspace()->setActiveOutput(QPoint(640, 512));
    input()->pointer()->warp(QPoint(640, 512));
}

void StereoDepthExchangeTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

// The active window on a stereo output may show depth inside it, away from the output's edges.
void StereoDepthExchangeTest::testActiveWindowIsAwake()
{
    auto window = createWindow(QPointF(100, 100));
    QVERIFY(window->window);
    QCOMPARE(workspace()->activeWindow(), window->window);

    auto depth = follow(window->surface.get());
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->states >= 1;
    }));
    QVERIFY(depth->awake);
    QCOMPARE(depth->outputEdges, 0u);
    QVERIFY(StereoDepth::isAwake(window->window));
}

// A window falling behind the active one lies flat in the pile, and wakes when it comes back.
void StereoDepthExchangeTest::testPileIsAsleep()
{
    auto first = createWindow(QPointF(100, 100));
    QVERIFY(first->window);
    auto firstDepth = follow(first->surface.get());
    QVERIFY(waitFor(firstDepth.get(), [&] {
        return firstDepth->awake;
    }));

    auto second = createWindow(QPointF(400, 400));
    QVERIFY(second->window);
    QCOMPARE(workspace()->activeWindow(), second->window);
    auto secondDepth = follow(second->surface.get());
    QVERIFY(waitFor(secondDepth.get(), [&] {
        return secondDepth->awake;
    }));
    QVERIFY(waitFor(firstDepth.get(), [&] {
        return !firstDepth->awake;
    }));

    workspace()->activateWindow(first->window);
    workspace()->raiseWindow(first->window);
    QVERIFY(waitFor(firstDepth.get(), [&] {
        return firstDepth->awake;
    }));
    QVERIFY(waitFor(secondDepth.get(), [&] {
        return !secondDepth->awake;
    }));
}

// On a 2D output nothing shows depth inside it.
void StereoDepthExchangeTest::testTwoDimensionalOutputIsAsleep()
{
    auto window = createWindow(QPointF(100, 100));
    QVERIFY(window->window);
    auto depth = follow(window->surface.get());
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->awake;
    }));

    QVERIFY(stereoOutput(false));
    QVERIFY(waitFor(depth.get(), [&] {
        return !depth->awake;
    }));
    QVERIFY(!StereoDepth::isAwake(window->window));

    QVERIFY(stereoOutput(true));
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->awake;
    }));
}

// The edges of the frame that touch the output's edges, which change as the window moves.
void StereoDepthExchangeTest::testOutputEdges()
{
    auto window = createWindow(QPointF(100, 100));
    QVERIFY(window->window);
    auto depth = follow(window->surface.get());
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->states >= 1;
    }));
    QCOMPARE(depth->outputEdges, 0u);

    window->window->move(QPointF(0, 0));
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->outputEdges == uint32_t(Test::StereoDepthV1::edge_left | Test::StereoDepthV1::edge_top);
    }));
    QCOMPARE(StereoDepth::outputEdges(window->window), Qt::LeftEdge | Qt::TopEdge);

    window->window->move(QPointF(s_outputSize.width() - 100, s_outputSize.height() - 50));
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->outputEdges == uint32_t(Test::StereoDepthV1::edge_right | Test::StereoDepthV1::edge_bottom);
    }));

    // Only changes are sent.
    const int states = depth->states;
    window->window->move(QPointF(s_outputSize.width() - 100, s_outputSize.height() - 50));
    QVERIFY(Test::waylandSync());
    QCOMPARE(depth->states, states);
}

// A surface followed before its window is shown hears once it is.
void StereoDepthExchangeTest::testFollowedBeforeShown()
{
    std::unique_ptr<KWayland::Client::Surface> surface = Test::createSurface();
    auto depth = follow(surface.get());
    QVERIFY(Test::waylandSync());
    QCOMPARE(depth->states, 0);

    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->states >= 1;
    }));
    QVERIFY(depth->awake);
}

// A toolkit and its style may follow the same surface; each hears the same, a late one at once.
void StereoDepthExchangeTest::testManyFollowers()
{
    auto window = createWindow(QPointF(100, 100));
    QVERIFY(window->window);
    auto first = follow(window->surface.get());
    auto second = follow(window->surface.get());
    QVERIFY(waitFor(first.get(), [&] {
        return first->states >= 1 && second->states >= 1;
    }));
    QVERIFY(first->awake && second->awake);

    auto late = follow(window->surface.get());
    QVERIFY(Test::waylandSync());
    QCOMPARE(late->states, 1);
    QVERIFY(late->awake);
}

// What pops up over the active window is awake.
void StereoDepthExchangeTest::testPopupIsAwake()
{
    auto parent = createWindow(QPointF(100, 100), QSize(300, 300));
    QVERIFY(parent->window);
    auto popup = createPopup(parent->toplevel->xdgSurface(), QPoint(50, 50));
    QVERIFY(popup->window);
    QVERIFY(popup->window->isPopupWindow());

    auto depth = follow(popup->surface.get());
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->states >= 1;
    }));
    QVERIFY(depth->awake);
}

// A pair the program marks as the desktop's own depth is still drawn per eye, but is not stereo
// content; the mark is the surface's while any of its objects sets it, and goes with the object.
void StereoDepthExchangeTest::testDesktopDepthIsNotStereoContent()
{
    auto window = createWindow(QPointF(100, 100));
    QVERIFY(window->window);
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*window->surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    QVERIFY(commit(window->surface.get(), window->window));
    QVERIFY(window->window->holdsStereoContent());

    auto marking = follow(window->surface.get());
    auto other = follow(window->surface.get());
    marking->set_desktop_depth(1);
    QVERIFY(Test::waylandSync());
    // double-buffered
    QVERIFY(!window->window->surface()->isStereoDesktopDepth());
    QSignalSpy depthChanged(window->window->surface(), &SurfaceInterface::stereoDepthChanged);
    QVERIFY(commit(window->surface.get(), window->window));
    QCOMPARE(depthChanged.count(), 1);
    QVERIFY(window->window->surface()->isStereoDesktopDepth());
    QVERIFY(!window->window->holdsStereoContent());
    // still drawn as a pair
    QCOMPARE(window->window->surface()->stereoContent(), StereoContentSideBySideFull);
    QCOMPARE(window->window->stereoContent(), StereoContentSideBySideFull);

    // another object that does not mark it leaves the mark
    other->set_desktop_depth(0);
    QVERIFY(commit(window->surface.get(), window->window));
    QVERIFY(window->window->surface()->isStereoDesktopDepth());

    // the mark goes with the object that set it
    marking.reset();
    QVERIFY(commit(window->surface.get(), window->window));
    QVERIFY(!window->window->surface()->isStereoDesktopDepth());
    QVERIFY(window->window->holdsStereoContent());
}

// A popup's declared place, under KWin's rules: never behind the popup it was opened from, and
// nothing for a window that is not a popup.
void StereoDepthExchangeTest::testDeclaredPlace()
{
    auto parent = createWindow(QPointF(100, 100), QSize(400, 400));
    QVERIFY(parent->window);
    auto menu = createPopup(parent->toplevel->xdgSurface(), QPoint(50, 50));
    QVERIFY(menu->window);
    QCOMPARE(StereoDepth::level(menu->window), 1.0);

    auto menuDepth = follow(menu->surface.get());
    menuDepth->set_place(wl_fixed_from_double(0.5));
    QVERIFY(commit(menu->surface.get(), menu->window));
    QCOMPARE(menu->window->surface()->stereoPlace(), 0.5);
    QCOMPARE(StereoDepth::level(menu->window), 0.5);
    QCOMPARE(StereoDepth::levelDepth(menu->window), 2.0);

    // a sub-menu declared behind it stays at its level
    auto subMenu = createPopup(menu->popup->xdgSurface(), QPoint(30, 30));
    QVERIFY(subMenu->window);
    auto subMenuDepth = follow(subMenu->surface.get());
    subMenuDepth->set_place(wl_fixed_from_double(-1.0));
    QVERIFY(commit(subMenu->surface.get(), subMenu->window));
    QCOMPARE(StereoDepth::level(subMenu->window), 0.5);

    // beyond the range is limited to it
    subMenuDepth->set_place(wl_fixed_from_double(3.0));
    QVERIFY(commit(subMenu->surface.get(), subMenu->window));
    QCOMPARE(subMenu->window->surface()->stereoPlace(), 1.0);
    QCOMPARE(StereoDepth::level(subMenu->window), 1.0);

    // withdrawn, the menu takes a share of the popped limit again, the only popup without a place
    menuDepth->unset_place();
    QVERIFY(commit(menu->surface.get(), menu->window));
    QVERIFY(!menu->window->surface()->stereoPlace());
    QCOMPARE(StereoDepth::level(menu->window), 1.0);

    // a window that is not a popup keeps its place in the stack
    auto parentDepth = follow(parent->surface.get());
    parentDepth->set_place(wl_fixed_from_double(1.0));
    QVERIFY(commit(parent->surface.get(), parent->window));
    QCOMPARE(StereoDepth::level(parent->window), 0.0);
}

// Once its surface is gone, an object's requests are ignored.
void StereoDepthExchangeTest::testInertWithoutSurface()
{
    auto window = createWindow(QPointF(100, 100));
    QVERIFY(window->window);
    auto depth = follow(window->surface.get());
    QVERIFY(waitFor(depth.get(), [&] {
        return depth->states >= 1;
    }));

    window->toplevel.reset();
    window->surface.reset();
    QVERIFY(Test::waylandSync());
    depth->set_desktop_depth(1);
    depth->set_place(wl_fixed_from_double(0.5));
    depth->unset_place();
    QVERIFY(Test::waylandSync());
    depth.reset();
    QVERIFY(Test::waylandSync());
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthExchangeTest)
#include "stereo_depth_exchange_test.moc"
