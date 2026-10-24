/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "wayland_server.h"

namespace KWin
{

class WaylandConnectionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void testConnectedBeforeTheTestWaits();
    void testManyConnections();
};

void WaylandConnectionTest::initTestCase()
{
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
    Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
}

void WaylandConnectionTest::cleanup()
{
    qunsetenv("KWIN_TEST_CONNECT_DELAY_MS");
    Test::destroyWaylandConnection();
}

void WaylandConnectionTest::testConnectedBeforeTheTestWaits()
{
    // This test verifies that a test client that connected while the test was still busy is a
    // connected client. The connection thread emits connected() as soon as it is started; on a
    // loaded machine the test thread is often not back at its wait for that signal yet, and
    // QSignalSpy::wait() waits for a signal after the call, so the one that had come already was
    // lost and init() of a random test failed with "setupWaylandConnection() returned FALSE".
    // The delay makes it the rule instead of the exception.
    qputenv("KWIN_TEST_CONNECT_DELAY_MS", "300");
    for (int i = 0; i < 3; ++i) {
        QVERIFY(Test::setupWaylandConnection());
        Test::destroyWaylandConnection();
    }
}


void WaylandConnectionTest::testManyConnections()
{
    // This test verifies that no connection of the test client is lost without a delay either: the
    // window between starting the connection and waiting for it is a few microseconds, and a machine
    // that is busy with something else hits it now and then (6 of 14 runs of testX11Window).
    for (int i = 0; i < 150; ++i) {
        QVERIFY2(Test::setupWaylandConnection(), qPrintable(QStringLiteral("connection %1").arg(i)));
        Test::destroyWaylandConnection();
    }
}

}

WAYLANDTEST_MAIN(KWin::WaylandConnectionTest)
#include "wayland_connection_test.moc"
