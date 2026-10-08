/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "input.h"

#include "effect/globals.h"
#include "pointer_input.h"
#include "scene/surfaceitem.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/surface.h>
#include <KWayland/Client/subsurface.h>

namespace KWin
{

class StereoContentTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testDefaultValue();
    void testSetContent();
    void testSubsurface();
    void testSetContentInvalidValue();
    void testRemoveOnDestroy();
    void testRecreateAfterDestroy();
    void testDoubleDeclarationError();
    void testNoSurfaceError();
};

void StereoContentTest::initTestCase()
{
    qRegisterMetaType<Window *>();

    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
}

void StereoContentTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::StereoContentV1));

    workspace()->setActiveOutput(QPoint(640, 512));
    input()->pointer()->warp(QPoint(640, 512));
}

void StereoContentTest::cleanup()
{
    Test::destroyWaylandConnection();
}

// Without a declaration, a surface carries no stereo content.
void StereoContentTest::testDefaultValue()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    QCOMPARE(window->surface()->stereoContent(), StereoContentNone);
    QCOMPARE(window->stereoContent(), StereoContentNone);
}

// set_content is double-buffered: it applies on commit and reaches the window.
void StereoContentTest::testSetContent()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));

    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->surface()->stereoContent(), StereoContentNone);

    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committed.wait());
    QCOMPARE(window->surface()->stereoContent(), StereoContentSideBySideFull);
    QCOMPARE(window->stereoContent(), StereoContentSideBySideFull);

    // Declaring none clears it again.
    declaration->set_content(Test::StereoContentV1::content_none);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committed.wait());
    QCOMPARE(window->surface()->stereoContent(), StereoContentNone);
    QCOMPARE(window->stereoContent(), StereoContentNone);
}

void StereoContentTest::testSubsurface()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::Surface> child(Test::createSurface());
    auto subsurface = Test::createSubSurface(child.get(), surface.get());
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*child));
    declaration->set_content(3);
    Test::render(child.get(), QSize(100, 50), Qt::red);
    QVERIFY(Test::waylandSync());
    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committed.wait());
    QCOMPARE(window->stereoContent(), StereoContentNone);
    const auto children = window->surfaceItem()->childItems();
    QCOMPARE(children.size(), 1);
    auto *item = qobject_cast<SurfaceItem *>(children.front());
    QVERIFY(item);
    QCOMPARE(item->stereoContent(), StereoContentSideBySideFull);
    declaration.reset();
    child->commit(KWayland::Client::Surface::CommitFlag::None);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committed.wait());
    QCOMPARE(item->stereoContent(), StereoContentNone);
}

// A value other than 0 or 3 is read as none.
void StereoContentTest::testSetContentInvalidValue()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));

    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
    declaration->set_content(3);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committed.wait());
    QCOMPARE(window->surface()->stereoContent(), StereoContentSideBySideFull);
    declaration->set_content(42);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committed.wait());
    QCOMPARE(window->surface()->stereoContent(), StereoContentNone);
}

// Destroying the declaration resets the surface on the next commit.
void StereoContentTest::testRemoveOnDestroy()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    {
        QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->surface()->stereoContent(), StereoContentSideBySideFull);
    }

    declaration.reset();

    QVERIFY(Test::waylandSync());
    // The reset is double-buffered like the set.
    QCOMPARE(window->surface()->stereoContent(), StereoContentSideBySideFull);
    {
        QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->surface()->stereoContent(), StereoContentNone);
        QCOMPARE(window->stereoContent(), StereoContentNone);
    }
}

// After the declaration is destroyed, the surface can be declared again.
void StereoContentTest::testRecreateAfterDestroy()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    QSignalSpy error(Test::waylandConnection(), &KWayland::Client::ConnectionThread::errorOccurred);

    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    {
        QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->surface()->stereoContent(), StereoContentSideBySideFull);
    }

    declaration.reset();

    auto declaration2 = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration2->set_content(Test::StereoContentV1::content_side_by_side_full);
    {
        QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(committed.wait());
        QCOMPARE(window->surface()->stereoContent(), StereoContentSideBySideFull);
    }

    QVERIFY(error.isEmpty());
}

// A second declaration for the same surface is a protocol error.
void StereoContentTest::testDoubleDeclarationError()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());

    auto first = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    auto second = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));

    QSignalSpy error(Test::waylandConnection(), &KWayland::Client::ConnectionThread::errorOccurred);
    QVERIFY(error.wait());
}

// set_content after the wl_surface is gone is a protocol error.
void StereoContentTest::testNoSurfaceError()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());

    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));

    surface.reset();
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);

    QSignalSpy error(Test::waylandConnection(), &KWayland::Client::ConnectionThread::errorOccurred);
    QVERIFY(error.wait());
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::StereoContentTest)
#include "stereo_content_test.moc"
