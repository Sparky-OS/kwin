/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <new>

#include "core/gpumanager.h"
#include "wayland/compositor.h"
#include "wayland/display.h"
#include "wayland/surface.h"

#include "KWayland/Client/compositor.h"
#include "KWayland/Client/connection_thread.h"
#include "KWayland/Client/event_queue.h"
#include "KWayland/Client/registry.h"
#include "KWayland/Client/surface.h"

using namespace KWin;

// BUG 510116: a surface extension that is destroyed and a new one created at the same address. The
// surface must not hand the old extension's state to the new one when it merges the pending state.

static int s_mismatches = 0;
static int s_applied = 0;

struct FirstState : public RawSurfaceAttachedState
{
    int value = 0;

    void mergeInto(RawSurfaceAttachedState *target) override
    {
        auto other = dynamic_cast<FirstState *>(target);
        if (!other) {
            ++s_mismatches;
            return;
        }
        other->value = value;
    }
};

struct SecondState : public RawSurfaceAttachedState
{
    int value = 0;

    void mergeInto(RawSurfaceAttachedState *target) override
    {
        auto other = dynamic_cast<SecondState *>(target);
        if (!other) {
            ++s_mismatches;
            return;
        }
        other->value = value;
    }
};

template<typename State>
class ProbeExtension : public RawSurfaceExtension
{
public:
    explicit ProbeExtension(SurfaceInterface *surface)
        : surface(surface)
    {
        pending = static_cast<State *>(surface->addExtension(this));
    }

    ~ProbeExtension() override
    {
        if (surface) {
            surface->removeExtension(this);
        }
    }

    std::unique_ptr<RawSurfaceAttachedState> createState() override
    {
        return std::make_unique<State>();
    }

    void applyState(RawSurfaceAttachedState *state) override
    {
        if (dynamic_cast<State *>(state)) {
            ++s_applied;
        } else {
            ++s_mismatches;
        }
    }

    QPointer<SurfaceInterface> surface;
    State *pending;
};

class TestSurfaceExtensionInterface : public QObject
{
    Q_OBJECT

public:
    ~TestSurfaceExtensionInterface() override;

private Q_SLOTS:
    void initTestCase();
    void testExtensionAtTheSameAddress();

private:
    KWayland::Client::ConnectionThread *m_connection = nullptr;
    KWayland::Client::EventQueue *m_queue = nullptr;
    KWayland::Client::Compositor *m_clientCompositor = nullptr;
    QThread *m_thread = nullptr;
    KWin::Display m_display;
    CompositorInterface *m_serverCompositor = nullptr;
};

void TestSurfaceExtensionInterface::initTestCase()
{
    GpuManager::s_self = std::make_unique<GpuManager>();
    m_display.addSocketName(qAppName());
    m_display.start();
    QVERIFY(m_display.isRunning());

    m_serverCompositor = new CompositorInterface(&m_display, this);

    m_connection = new KWayland::Client::ConnectionThread;
    QSignalSpy connectedSpy(m_connection, &KWayland::Client::ConnectionThread::connected);
    m_connection->setSocketName(qAppName());

    m_thread = new QThread(this);
    m_connection->moveToThread(m_thread);
    m_thread->start();

    m_connection->initConnection();
    QVERIFY(connectedSpy.wait());

    m_queue = new KWayland::Client::EventQueue(this);
    m_queue->setup(m_connection);
    QVERIFY(m_queue->isValid());

    auto registry = new KWayland::Client::Registry(this);
    QSignalSpy compositorSpy(registry, &KWayland::Client::Registry::compositorAnnounced);
    registry->setEventQueue(m_queue);
    registry->create(m_connection->display());
    QVERIFY(registry->isValid());
    registry->setup();
    QVERIFY(compositorSpy.wait());

    m_clientCompositor = registry->createCompositor(compositorSpy.first().first().value<quint32>(), compositorSpy.first().last().value<quint32>(), this);
    QVERIFY(m_clientCompositor->isValid());
}

TestSurfaceExtensionInterface::~TestSurfaceExtensionInterface()
{
    if (m_queue) {
        delete m_queue;
        m_queue = nullptr;
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }
    KWin::GpuManager::s_self.reset();
}

void TestSurfaceExtensionInterface::testExtensionAtTheSameAddress()
{
    static_assert(sizeof(ProbeExtension<FirstState>) == sizeof(ProbeExtension<SecondState>));

    QSignalSpy serverSurfaceCreatedSpy(m_serverCompositor, &CompositorInterface::surfaceCreated);
    std::unique_ptr<KWayland::Client::Surface> clientSurface(m_clientCompositor->createSurface(this));
    QVERIFY(serverSurfaceCreatedSpy.wait());
    SurfaceInterface *serverSurface = serverSurfaceCreatedSpy.first().first().value<SurfaceInterface *>();
    QVERIFY(serverSurface);
    QSignalSpy committedSpy(serverSurface, &SurfaceInterface::committed);

    alignas(ProbeExtension<FirstState>) char storage[sizeof(ProbeExtension<FirstState>)];

    // The first extension has its state applied on a commit, so the surface's current state keeps it.
    auto first = new (storage) ProbeExtension<FirstState>(serverSurface);
    first->pending->value = 1;
    clientSurface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(s_applied, 1);
    QCOMPARE(s_mismatches, 0);

    // It goes away, and an extension of another type is created at the same address.
    first->~ProbeExtension<FirstState>();
    auto second = new (storage) ProbeExtension<SecondState>(serverSurface);
    second->pending->value = 2;
    clientSurface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());

    // The new extension gets a state of its own type, not the destroyed extension's.
    QCOMPARE(s_mismatches, 0);
    QCOMPARE(s_applied, 2);

    second->~ProbeExtension<SecondState>();
}

QTEST_GUILESS_MAIN(TestSurfaceExtensionInterface)

#include "test_surface_extension_interface.moc"
