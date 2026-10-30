/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "wayland_server.h"
#include "workspace.h"

#include <QSet>

#include <cstdio>

namespace KWin
{

/**
 * What the workspace showed to the listeners of outputRemoved while it was destroyed. The workspace
 * is destroyed with the test application, after QTest::qExec() has returned, so this is collected
 * then and reported by main() below.
 */
struct Teardown
{
    bool watching = false;
    QSet<const QObject *> freed;
    int removed = 0;
    QStringList failures;
};

static Teardown s_teardown;

/**
 * ~Workspace removes each output the way updateOutputs() removes an output that went away:
 * outputRemoved is emitted for an output that is no longer in outputs() and outputOrder(), and no
 * output is freed before the last outputRemoved has been emitted. A listener that answers by walking
 * the workspace's outputs, as the StereoDepth D-Bus interface does, meets only live outputs.
 */
class WorkspaceTeardownTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testWatchTheTeardown();
};

void WorkspaceTeardownTest::initTestCase()
{
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
        Rect(1280, 0, 1280, 1024),
    });
}

void WorkspaceTeardownTest::testWatchTheTeardown()
{
    const QList<LogicalOutput *> outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 2);

    // The outputs and the workspace are the contexts, since the test object is destroyed before the
    // application.
    for (LogicalOutput *output : outputs) {
        connect(output, &QObject::destroyed, output, [](QObject *object) {
            s_teardown.freed.insert(object);
        });
    }
    // Only pointers are compared here; nothing that may have been freed is read.
    connect(workspace(), &Workspace::outputRemoved, workspace(), [](LogicalOutput *output) {
        if (!s_teardown.watching) {
            return;
        }
        ++s_teardown.removed;
        const QString when = QStringLiteral("at outputRemoved %1: ").arg(s_teardown.removed);
        const QList<LogicalOutput *> listed = workspace()->outputs();
        const QList<LogicalOutput *> ordered = workspace()->outputOrder();
        if (listed.contains(output)) {
            s_teardown.failures.append(when + QStringLiteral("the removed output is still in outputs()"));
        }
        if (ordered.contains(output)) {
            s_teardown.failures.append(when + QStringLiteral("the removed output is still in outputOrder()"));
        }
        for (const LogicalOutput *other : listed) {
            if (s_teardown.freed.contains(other)) {
                s_teardown.failures.append(when + QStringLiteral("outputs() lists an output that has been freed"));
            }
        }
        for (const LogicalOutput *other : ordered) {
            if (s_teardown.freed.contains(other)) {
                s_teardown.failures.append(when + QStringLiteral("outputOrder() lists an output that has been freed"));
            }
        }
        if (!s_teardown.freed.isEmpty()) {
            s_teardown.failures.append(when + QStringLiteral("%1 output(s) freed before the last outputRemoved").arg(s_teardown.freed.size()));
        }
    });
}

} // namespace KWin

// WAYLANDTEST_MAIN, with the application in a scope of its own: the workspace is destroyed with it,
// after the test functions, and what the teardown showed is reported once it is gone.
int main(int argc, char *argv[])
{
    setenv("QT_QPA_PLATFORM", "wayland-org.kde.kwin.qpa", true);
    setenv("QT_QPA_PLATFORM_PLUGIN_PATH", QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath().toLocal8Bit().constData(), true);
    setenv("KWIN_FORCE_OWN_QPA", "1", true);
    qunsetenv("KDE_FULL_SESSION");
    qunsetenv("KDE_SESSION_VERSION");
    qunsetenv("XDG_SESSION_DESKTOP");
    qunsetenv("XDG_CURRENT_DESKTOP");
    int result = 0;
    {
        KWin::WaylandTestApplication app(argc, argv, false);
        qunsetenv("QT_QPA_PLATFORM");
        qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
        qunsetenv("KWIN_FORCE_OWN_QPA");
        app.setAttribute(Qt::AA_Use96Dpi, true);
        KWin::WorkspaceTeardownTest tc;
        result = QTest::qExec(&tc, argc, argv);
        KWin::s_teardown.watching = true;
    }

    const KWin::Teardown &teardown = KWin::s_teardown;
    QStringList failures = teardown.failures;
    if (result == 0 && teardown.removed != 2) {
        failures.append(QStringLiteral("outputRemoved was emitted %1 times for 2 outputs").arg(teardown.removed));
    }
    if (failures.isEmpty()) {
        std::printf("PASS   : KWin::WorkspaceTeardownTest::~Workspace() %d outputs removed, none freed before the last outputRemoved\n", teardown.removed);
    } else {
        for (const QString &failure : std::as_const(failures)) {
            std::printf("FAIL!  : KWin::WorkspaceTeardownTest::~Workspace() %s\n", qPrintable(failure));
        }
        result = 1;
    }
    std::fflush(stdout);
    return result;
}

#include "workspace_teardown_test.moc"
