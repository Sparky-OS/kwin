/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "options.h"
#include "wayland_server.h"
#include "workspace.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingReply>
#include <QDBusVariant>

namespace KWin
{

static const QString s_service = QStringLiteral("org.kde.KWin");
static const QString s_path = QStringLiteral("/org/kde/KWin/StereoDepth");
static const QString s_interface = QStringLiteral("org.kde.KWin.StereoDepth");

using Cuts = QMap<QString, int>;

/**
 * Records what KWin announces on its stereo depth object: the changed properties and the outputs'
 * changes.
 */
class Announcements : public QObject
{
    Q_OBJECT

public:
    Announcements()
    {
        QDBusConnection::sessionBus().connect(s_service, s_path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                                              this, SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
        QDBusConnection::sessionBus().connect(s_service, s_path, s_interface, QStringLiteral("OutputsChanged"), this, SIGNAL(outputsChanged()));
    }

Q_SIGNALS:
    void wallpaperCutsChanged(const KWin::Cuts &cuts);
    void outputsChanged();

private Q_SLOTS:
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &)
    {
        if (interface == s_interface && changed.contains(QStringLiteral("WallpaperCuts"))) {
            Q_EMIT wallpaperCutsChanged(qdbus_cast<Cuts>(changed.value(QStringLiteral("WallpaperCuts"))));
        }
    }
};

/**
 * KWin publishes how many columns a sunk wallpaper loses at each side on each output, so the pages
 * that set a wallpaper state KWin's own number. It follows the Background depth and each output's
 * width and mode at once: (|p| + 1) / 2 with p = lround(SunkLimit × W / 1920), on an output in a 3D
 * mode only.
 */
class StereoDepthWallpaperCutsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testCuts_data();
    void testCuts();
    void testTwoDimensionalOutput();
    void testFollowsTheBackgroundDepth();
    void testFollowsTheOutputs();
    void testCutsAtAnotherDepth();
};

static const QSize s_small(1280, 720);
static const QSize s_large(1920, 1080);

static OutputModeline plainMode(const QSize &size)
{
    return OutputModeline(size, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode(const QSize &size)
{
    return OutputModeline(size, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

// both outputs in a 3D mode, or @a twoDimensional left in 2D
static bool setStereo(LogicalOutput *twoDimensional = nullptr)
{
    OutputConfiguration configuration;
    const QList<LogicalOutput *> outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        const QSize size = output->modeSize();
        configuration.changeSet(output->backendOutput())->currentMode = output == twoDimensional ? plainMode(size) : stereoMode(size);
    }
    workspace()->applyOutputConfiguration(configuration);
    if (!Test::waylandSync()) {
        return false;
    }
    for (LogicalOutput *output : outputs) {
        if (output->hasStereoEyes() != (output != twoDimensional)) {
            return false;
        }
    }
    return true;
}

static std::optional<Cuts> wallpaperCuts()
{
    QDBusMessage message = QDBusMessage::createMethodCall(s_service, s_path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    message.setArguments({s_interface, QStringLiteral("WallpaperCuts")});
    QDBusPendingReply<QDBusVariant> reply = QDBusConnection::sessionBus().asyncCall(message);
    reply.waitForFinished();
    if (reply.isError()) {
        qWarning() << reply.error();
        return std::nullopt;
    }
    return qdbus_cast<Cuts>(reply.value().variant());
}

static std::optional<Cuts> wallpaperCutsAt(double backgroundDepth)
{
    QDBusMessage message = QDBusMessage::createMethodCall(s_service, s_path, s_interface, QStringLiteral("WallpaperCutsAt"));
    message.setArguments({backgroundDepth});
    QDBusPendingReply<Cuts> reply = QDBusConnection::sessionBus().asyncCall(message);
    reply.waitForFinished();
    if (reply.isError()) {
        qWarning() << reply.error();
        return std::nullopt;
    }
    return reply.value();
}

static LogicalOutput *small()
{
    return workspace()->outputs().at(0);
}

static LogicalOutput *large()
{
    return workspace()->outputs().at(1);
}

static Cuts cuts(int onSmall, int onLarge)
{
    return Cuts{{small()->name(), onSmall}, {large()->name(), onLarge}};
}

void StereoDepthWallpaperCutsTest::initTestCase()
{
    qDBusRegisterMetaType<Cuts>();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
}

void StereoDepthWallpaperCutsTest::init()
{
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(QPoint(0, 0), s_small),
            .modes = {plainMode(s_small), stereoMode(s_small)},
        },
        Test::OutputInfo{
            .geometry = Rect(QPoint(s_small.width(), 0), s_large),
            .modes = {plainMode(s_large), stereoMode(s_large)},
        },
    });
    QVERIFY(Test::setupWaylandConnection());
    QCOMPARE(workspace()->outputs().size(), 2);
    QCOMPARE(small()->pixelSize(), s_small);
    QCOMPARE(large()->pixelSize(), s_large);
    // the default Background depth
    options->setStereoSunkLimit(10);
    QVERIFY(setStereo());
}

void StereoDepthWallpaperCutsTest::cleanup()
{
    Test::destroyWaylandConnection();
}

void StereoDepthWallpaperCutsTest::testCuts_data()
{
    QTest::addColumn<qreal>("backgroundDepth");
    QTest::addColumn<int>("onSmall");
    QTest::addColumn<int>("onLarge");

    // p = lround(10 × 1280 / 1920) = 7, so 4 at each side; 10 at 1920, so 5 at each side, 10 in all
    QTest::addRow("default") << 10.0 << 4 << 5;
    // 13 at 1280 and 20 at 1920
    QTest::addRow("20") << 20.0 << 7 << 10;
    // 20 at 1280 and 30 at 1920
    QTest::addRow("30") << 30.0 << 10 << 15;
    // 1 at 1280, still a column at each side; 2 at 1920
    QTest::addRow("2") << 2.0 << 1 << 1;
    QTest::addRow("off") << 0.0 << 0 << 0;
}

// Each output states its own number, from its own width.
void StereoDepthWallpaperCutsTest::testCuts()
{
    QFETCH(qreal, backgroundDepth);
    QFETCH(int, onSmall);
    QFETCH(int, onLarge);

    options->setStereoSunkLimit(backgroundDepth);
    QCOMPARE(wallpaperCuts(), std::optional(cuts(onSmall, onLarge)));
}

// An output in 2D shows the wallpaper whole: nothing is lost there, while the other output still
// loses its own columns.
void StereoDepthWallpaperCutsTest::testTwoDimensionalOutput()
{
    QVERIFY(setStereo(large()));
    QCOMPARE(wallpaperCuts(), std::optional(cuts(4, 0)));
    QVERIFY(setStereo(small()));
    QCOMPARE(wallpaperCuts(), std::optional(cuts(0, 5)));
}

// Moving the Background depth changes the numbers at once, and KWin says so.
void StereoDepthWallpaperCutsTest::testFollowsTheBackgroundDepth()
{
    Announcements announcements;
    QSignalSpy changed(&announcements, &Announcements::wallpaperCutsChanged);
    QCOMPARE(wallpaperCuts(), std::optional(cuts(4, 5)));

    options->setStereoSunkLimit(20);
    QCOMPARE(wallpaperCuts(), std::optional(cuts(7, 10)));
    QTRY_COMPARE(changed.count(), 1);
    QCOMPARE(changed.last().at(0).value<Cuts>(), cuts(7, 10));

    options->setStereoSunkLimit(0);
    QTRY_COMPARE(changed.count(), 2);
    QCOMPARE(changed.last().at(0).value<Cuts>(), cuts(0, 0));
    QCOMPARE(wallpaperCuts(), std::optional(cuts(0, 0)));
}

// Leaving a 3D mode or entering it changes that output's number at once.
void StereoDepthWallpaperCutsTest::testFollowsTheOutputs()
{
    Announcements announcements;
    QSignalSpy changed(&announcements, &Announcements::wallpaperCutsChanged);
    QSignalSpy outputsChanged(&announcements, &Announcements::outputsChanged);

    QVERIFY(setStereo(small()));
    QTRY_COMPARE(changed.count(), 1);
    QCOMPARE(changed.last().at(0).value<Cuts>(), cuts(0, 5));
    QTRY_VERIFY(!outputsChanged.isEmpty());

    // with no Background depth nothing is lost anywhere, but a settings page's slider still needs
    // to know that the outputs changed
    options->setStereoSunkLimit(0);
    QTRY_COMPARE(changed.count(), 2);
    outputsChanged.clear();
    QVERIFY(setStereo());
    QTRY_VERIFY(!outputsChanged.isEmpty());
    QCOMPARE(wallpaperCuts(), std::optional(cuts(0, 0)));
    QCOMPARE(wallpaperCutsAt(10), std::optional(cuts(4, 5)));
    QCOMPARE(changed.count(), 2);
}

// A depth the user has not applied yet, such as the one on a settings page's slider, is answered
// for every output without changing what the outputs lose now.
void StereoDepthWallpaperCutsTest::testCutsAtAnotherDepth()
{
    QCOMPARE(wallpaperCutsAt(20), std::optional(cuts(7, 10)));
    QCOMPARE(wallpaperCutsAt(0), std::optional(cuts(0, 0)));
    QCOMPARE(wallpaperCuts(), std::optional(cuts(4, 5)));
    QVERIFY(setStereo(large()));
    QCOMPARE(wallpaperCutsAt(20), std::optional(cuts(7, 0)));
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::StereoDepthWallpaperCutsTest)
#include "stereo_depth_wallpaper_cuts_test.moc"
