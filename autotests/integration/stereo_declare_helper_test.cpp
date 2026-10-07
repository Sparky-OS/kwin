/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "kwin_wayland_test.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
#include "x11window.h"
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/surface.h>
#include <QtConcurrentRun>
#include <stereo-declare.h>
#include <X11/Xatom.h>
#undef None

namespace KWin
{
class StereoDeclareHelperTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(waylandServer()->init(qAppName()));
        kwinApp()->start();
    }

    void testWayland()
    {
        QVERIFY(Test::setupWaylandConnection());
        auto *display = Test::waylandConnection()->display();
        auto *queue = wl_display_create_queue(display);
        QVERIFY(queue);
        QCOMPARE(stereo_supported_wayland(), 0u);
        QCOMPARE(stereo_declare_wayland_init(display, queue), 0);
        QCOMPARE(stereo_declare_wayland_init(display, queue), -EBUSY);
        QVERIFY(Test::waylandSync());
        QVERIFY(wl_display_dispatch_queue_pending(display, queue) >= 0);
        QCOMPARE(stereo_supported_wayland(), 1u);
        std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
        std::unique_ptr<Test::XdgToplevel> toplevel(Test::createXdgToplevelSurface(surface.get()));
        Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
        QVERIFY(window);
        QCOMPARE(stereo_declare_wayland(*surface, STEREO_SBS_FULL, STEREO_CLASS_GAME, STEREO_GAME_GL), 0);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->stereoContent(), StereoContentNone);
        QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->stereoContent(), StereoContentSideBySideFull);
        QCOMPARE(window->surface()->contentType(), ContentType::Game);
        QCOMPARE(stereo_declare_wayland(*surface, static_cast<stereo_layout>(1), STEREO_CLASS_SCIENTIFIC, STEREO_SCIENTIFIC_VR), -EINVAL);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->stereoContent(), StereoContentSideBySideFull);
        QCOMPARE(window->surface()->contentType(), ContentType::Game);

        QCOMPARE(stereo_remove_wayland(*surface), 0);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->stereoContent(), StereoContentNone);
        QCOMPARE(window->surface()->contentType(), ContentType::None);
        QCOMPARE(stereo_declare_wayland(*surface, STEREO_SBS_FULL, STEREO_CLASS_VIDEO, STEREO_VIDEO_CURRENT), 0);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->surface()->contentType(), ContentType::Video);
        stereo_remove_wayland(*surface);
        stereo_declare_wayland_finish();
        QCOMPARE(stereo_supported_wayland(), 0u);
        toplevel.reset();
        surface.reset();
        wl_event_queue_destroy(queue);
        Test::destroyWaylandConnection();
    }

    void testX11()
    {
        // Start the harness's lazy Xwayland before opening a second Xlib connection.
        auto bootstrap = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(bootstrap.get()));
        auto future = QtConcurrent::run([] { return XOpenDisplay(nullptr); });
        QTRY_VERIFY(future.isFinished());
        std::unique_ptr<::Display, decltype(&XCloseDisplay)> display(future.result(), XCloseDisplay);
        QVERIFY(display);
        QCOMPARE(stereo_supported_x11(display.get()), 3u);
        const auto support = XInternAtom(display.get(), "_KDE_NET_WM_STEREO_CONTENT_SUPPORTED", False);
        const unsigned long version = 3;
        XDeleteProperty(display.get(), DefaultRootWindow(display.get()), support);
        QCOMPARE(stereo_supported_x11(display.get()), 0u);
        QCOMPARE(stereo_declare_x11(display.get(), DefaultRootWindow(display.get()), STEREO_SBS_FULL, STEREO_CLASS_GAME, 0), -ENOTSUP);
        XChangeProperty(display.get(), DefaultRootWindow(display.get()), support, XA_CARDINAL, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(&version), 1);
        QCOMPARE(stereo_supported_x11(display.get()), 3u);
        const auto id = XCreateSimpleWindow(display.get(), DefaultRootWindow(display.get()), 0, 0, 200, 100, 0, 0, 0);
        QCOMPARE(stereo_declare_x11(display.get(), id, STEREO_SBS_FULL, STEREO_CLASS_GAME, STEREO_GAME_3D), 0);
        QSignalSpy added(workspace(), &Workspace::windowAdded);
        XMapWindow(display.get(), id);
        XFlush(display.get());
        QVERIFY(added.wait());
        auto *window = added.last().first().value<Window *>();
        QVERIFY(window);
        QCOMPARE(window->stereoContent(), StereoContentSideBySideFull);
        QCOMPARE(window->declaredStereoContentClass(), 3);
        QCOMPARE(window->declaredStereoContentSubclass(), 2);
        QCOMPARE(stereo_declare_x11(display.get(), id, static_cast<stereo_layout>(1), STEREO_CLASS_VIDEO, STEREO_VIDEO_LEGACY), -EINVAL);
        QCOMPARE(stereo_remove_x11(display.get(), id), 0);
        QTRY_COMPARE(window->stereoContent(), StereoContentNone);
        XDestroyWindow(display.get(), id);
        XFlush(display.get());
        QVERIFY(Test::waitForWindowClosed(window));
    }
};
}
WAYLANDTEST_MAIN(KWin::StereoDeclareHelperTest)
#include "stereo_declare_helper_test.moc"
