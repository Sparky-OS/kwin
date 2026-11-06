/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "options.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
#include "x11window.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>

#include <xcb/xcb.h>

namespace KWin
{

/**
 * The invisible border that keeps popups off the edges of an output that shows stereo 3D (ORDER 418): a popup is
 * placed at least PopupBorder logical pixels inside every edge of the output, wherever it was anchored or put. The
 * positions are read from the windows' frame geometry.
 */
class StereoPopupBorderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testCorners_data();
    void testCorners();
    void testFlipsAwayFromTheEdge();
    void testSubMenu();
    void testInsideUntouched();
    void testNotFittingKeepsStock();
    void testToolTip_data();
    void testToolTip();
    void testOverrideRedirectMenu();
    void testBorderOff();
    void testTwoDimensionalOutputIsUnchanged();
};

static constexpr int s_border = 10;
static const QSize s_output(320, 240);

static OutputModeline plainMode()
{
    return OutputModeline(s_output, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_output, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

void StereoPopupBorderTest::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    QVERIFY(waylandServer()->init(qAppName()));
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup depth(config, QStringLiteral("StereoDepth"));
    depth.writeEntry("PopupBorder", double(s_border));
    config->sync();
    kwinApp()->setConfig(config);
    kwinApp()->start();
}

void StereoPopupBorderTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_output),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::PlasmaShell));
    QVERIFY(stereoOutput(true));
    options->setStereoPopupBorder(s_border);
}

void StereoPopupBorderTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

struct Parent
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> toplevel;
    Window *window = nullptr;
};

// a small window in the middle of the output; popups are anchored in output coordinates through it
static Parent createParent()
{
    Parent parent;
    parent.surface = Test::createSurface();
    parent.toplevel = Test::createXdgToplevelSurface(parent.surface.get());
    parent.window = Test::renderAndWaitForShown(parent.surface.get(), QSize(100, 100), Qt::blue);
    if (parent.window) {
        parent.window->move(QPointF(110, 70));
    }
    return parent;
}

struct Popup
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgPopup> popup;
    Window *window = nullptr;
};

// a popup of @a size whose anchor point is @a point of the output, growing to the bottom right
static Popup createPopup(const Parent &parent, const QPoint &point, const QSize &size, Test::XdgPositioner::constraint_adjustment adjustment,
                         Test::XdgPositioner::anchor anchor = Test::XdgPositioner::anchor_top_left,
                         Test::XdgPositioner::gravity gravity = Test::XdgPositioner::gravity_bottom_right)
{
    Popup result;
    result.surface = Test::createSurface();
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(size.width(), size.height());
    const QPoint inParent = point - QPoint(110, 70);
    positioner->set_anchor_rect(inParent.x(), inParent.y(), 1, 1);
    positioner->set_anchor(anchor);
    positioner->set_gravity(gravity);
    positioner->set_constraint_adjustment(adjustment);
    result.popup = Test::createXdgPopupSurface(result.surface.get(), parent.toplevel->xdgSurface(), positioner.get());
    result.window = Test::renderAndWaitForShown(result.surface.get(), size, Qt::green);
    return result;
}

static constexpr auto s_slide = Test::XdgPositioner::constraint_adjustment(Test::XdgPositioner::constraint_adjustment_slide_x | Test::XdgPositioner::constraint_adjustment_slide_y);

void StereoPopupBorderTest::testCorners_data()
{
    QTest::addColumn<QPoint>("anchor");
    QTest::addColumn<RectF>("expected");

    // a 60x40 menu anchored at each corner and edge of the 320x240 output, sliding: stock KWin puts it against the edge
    QTest::newRow("top left") << QPoint(0, 0) << RectF(10, 10, 60, 40);
    QTest::newRow("top right") << QPoint(319, 0) << RectF(250, 10, 60, 40);
    QTest::newRow("bottom left") << QPoint(0, 239) << RectF(10, 190, 60, 40);
    QTest::newRow("bottom right") << QPoint(319, 239) << RectF(250, 190, 60, 40);
    QTest::newRow("left edge") << QPoint(-4, 100) << RectF(10, 100, 60, 40);
    QTest::newRow("right edge") << QPoint(300, 100) << RectF(250, 100, 60, 40);
    QTest::newRow("top edge") << QPoint(100, -4) << RectF(100, 10, 60, 40);
    QTest::newRow("bottom edge") << QPoint(100, 230) << RectF(100, 190, 60, 40);
}

void StereoPopupBorderTest::testCorners()
{
    QFETCH(QPoint, anchor);
    QFETCH(RectF, expected);
    auto parent = createParent();
    QVERIFY(parent.window);
    auto popup = createPopup(parent, anchor, QSize(60, 40), s_slide);
    QVERIFY(popup.window);
    // the right/bottom anchors grow the popup past the edge: it is slid back to the border
    QCOMPARE(popup.window->frameGeometry(), expected);
}

void StereoPopupBorderTest::testFlipsAwayFromTheEdge()
{
    // flip: a menu opening to the right from x 260 ends at 320, the edge: stock KWin keeps it, but it does not fit inside the
    // border (right edge at 310), so it opens to the left, ending at its anchor (the anchor rectangle is 1 wide)
    auto parent = createParent();
    QVERIFY(parent.window);
    auto popup = createPopup(parent, QPoint(260, 100), QSize(60, 40), Test::XdgPositioner::constraint_adjustment_flip_x,
                             Test::XdgPositioner::anchor_top_left, Test::XdgPositioner::gravity_bottom_right);
    QVERIFY(popup.window);
    QCOMPARE(popup.window->frameGeometry(), RectF(201, 100, 60, 40));
}

void StereoPopupBorderTest::testSubMenu()
{
    // a menu near the right edge and its sub-menu, which would cross the border to the right, flip to the left of it
    auto parent = createParent();
    QVERIFY(parent.window);
    auto menu = createPopup(parent, QPoint(210, 80), QSize(60, 60), s_slide);
    QVERIFY(menu.window);
    QCOMPARE(menu.window->frameGeometry(), RectF(210, 80, 60, 60));

    Popup sub;
    sub.surface = Test::createSurface();
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(50, 40);
    // the sub-menu is anchored at the menu's right edge, in the menu's coordinates
    positioner->set_anchor_rect(60, 10, 1, 1);  // output x 270
    positioner->set_anchor(Test::XdgPositioner::anchor_top_left);
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    positioner->set_constraint_adjustment(Test::XdgPositioner::constraint_adjustment_flip_x | Test::XdgPositioner::constraint_adjustment_slide_y);
    sub.popup = Test::createXdgPopupSurface(sub.surface.get(), menu.popup->xdgSurface(), positioner.get());
    sub.window = Test::renderAndWaitForShown(sub.surface.get(), QSize(50, 40), Qt::red);
    QVERIFY(sub.window);
    // unflipped it would be at x 270 to 320: inside the edge, but inside the border it is not; flipped it ends at the anchor
    QVERIFY(sub.window->frameGeometry().right() <= 310);
    QVERIFY(sub.window->frameGeometry().left() >= s_border);
    QCOMPARE(sub.window->frameGeometry(), RectF(221, 90, 50, 40));
}

void StereoPopupBorderTest::testInsideUntouched()
{
    auto parent = createParent();
    QVERIFY(parent.window);
    auto popup = createPopup(parent, QPoint(120, 80), QSize(60, 40), s_slide);
    QVERIFY(popup.window);
    QCOMPARE(popup.window->frameGeometry(), RectF(120, 80, 60, 40));
}

void StereoPopupBorderTest::testNotFittingKeepsStock()
{
    // 320 wide: wider than the output minus the border; it keeps stock KWin's x (against the edge) and takes the border in y
    auto parent = createParent();
    QVERIFY(parent.window);
    auto popup = createPopup(parent, QPoint(0, 0), QSize(320, 40), s_slide);
    QVERIFY(popup.window);
    QCOMPARE(popup.window->frameGeometry(), RectF(0, 10, 320, 40));
}

void StereoPopupBorderTest::testToolTip_data()
{
    QTest::addColumn<int>("role");
    QTest::addColumn<QPoint>("position");
    QTest::addColumn<QPointF>("expected");

    using Role = KWayland::Client::PlasmaShellSurface::Role;
    QTest::newRow("tooltip bottom right") << int(Role::ToolTip) << QPoint(300, 230) << QPointF(280, 210);
    QTest::newRow("tooltip top left") << int(Role::ToolTip) << QPoint(2, 3) << QPointF(10, 10);
    QTest::newRow("notification corner") << int(Role::Notification) << QPoint(290, 0) << QPointF(280, 10);
    QTest::newRow("on-screen display") << int(Role::OnScreenDisplay) << QPoint(130, 215) << QPointF(130, 210);
}

void StereoPopupBorderTest::testToolTip()
{
    QFETCH(int, role);
    QFETCH(QPoint, position);
    QFETCH(QPointF, expected);

    // a program-placed popup of the panel's shell (30x20): tooltip, notification, on-screen display
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    std::unique_ptr<KWayland::Client::PlasmaShellSurface> plasma(Test::waylandPlasmaShell()->createSurface(surface.get()));
    plasma->setRole(KWayland::Client::PlasmaShellSurface::Role(role));
    plasma->setPosition(position);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(30, 20), Qt::yellow);
    QVERIFY(window);
    QTRY_COMPARE(window->pos(), expected);
}

void StereoPopupBorderTest::testOverrideRedirectMenu()
{
    Test::XcbConnectionPtr c = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(c.get()));
    auto atom = [&](const char *name) {
        auto reply = xcb_intern_atom_reply(c.get(), xcb_intern_atom(c.get(), 0, strlen(name), name), nullptr);
        const xcb_atom_t result = reply->atom;
        free(reply);
        return result;
    };
    const struct
    {
        QPoint at;
        QPoint expected;
    } cases[] = {{QPoint(312, 100), QPoint(250, 100)}, {QPoint(100, 232), QPoint(100, 190)}, {QPoint(120, 80), QPoint(120, 80)}};
    for (const auto &item : cases) {
        const xcb_window_t id = xcb_generate_id(c.get());
        const uint32_t values[] = {1};
        xcb_create_window(c.get(), XCB_COPY_FROM_PARENT, id, kwinApp()->x11RootWindow(), item.at.x(), item.at.y(), 60, 40, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT, values);
        const xcb_atom_t type = atom("_NET_WM_WINDOW_TYPE_POPUP_MENU");
        xcb_change_property(c.get(), XCB_PROP_MODE_REPLACE, id, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32, 1, &type);
        QSignalSpy added(workspace(), &Workspace::windowAdded);
        xcb_map_window(c.get(), id);
        xcb_flush(c.get());
        QVERIFY(added.wait());
        auto *window = added.last().first().value<X11Window *>();
        QVERIFY(window);
        QVERIFY(window->isUnmanaged());
        QCOMPARE(window->frameGeometry().topLeft().toPoint(), item.expected);
        QCOMPARE(window->frameGeometry().size(), QSizeF(60, 40));
        xcb_destroy_window(c.get(), id);
        xcb_flush(c.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
    }
}

void StereoPopupBorderTest::testBorderOff()
{
    options->setStereoPopupBorder(0);
    auto parent = createParent();
    QVERIFY(parent.window);
    auto popup = createPopup(parent, QPoint(0, 0), QSize(60, 40), s_slide);
    QVERIFY(popup.window);
    QCOMPARE(popup.window->frameGeometry(), RectF(0, 0, 60, 40));
}

void StereoPopupBorderTest::testTwoDimensionalOutputIsUnchanged()
{
    QVERIFY(stereoOutput(false));
    auto parent = createParent();
    QVERIFY(parent.window);
    auto popup = createPopup(parent, QPoint(319, 239), QSize(60, 40), s_slide);
    QVERIFY(popup.window);
    QCOMPARE(popup.window->frameGeometry(), RectF(260, 200, 60, 40));
    auto topLeft = createPopup(parent, QPoint(0, 0), QSize(60, 40), s_slide);
    QVERIFY(topLeft.window);
    QCOMPARE(topLeft.window->frameGeometry(), RectF(0, 0, 60, 40));
}

}

WAYLANDTEST_MAIN(KWin::StereoPopupBorderTest)
#include "stereo_popup_border_test.moc"
