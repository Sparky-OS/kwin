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
#include "scene/stereodepth.h"
#include "scene/workspacescene.h"
#include "wayland-client/viewporter.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>

#include <QPainter>

namespace KWin
{

/**
 * The wallpaper is the deepest layer, behind the screen, at the sunk limit. On a stereo output its
 * copy moves in each eye and keeps its size: nothing is stretched, and the columns either eye's copy
 * leaves open are cut from both sides, the same columns in both eyes, so what is left is covered in
 * both eyes. With no sunk limit, and on a 2D output, the wallpaper is drawn as it is.
 *
 * Each column of the wallpaper has a colour of its own, so a capture of the output shows, column
 * by column, which column of the wallpaper each eye shows there: a stretched copy shows mixed or
 * repeated columns.
 */
class StereoDepthWallpaperTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testDesktopKeepsItsSize_data();
    void testDesktopKeepsItsSize();
    void testNoSunkLimitNoCut();
    void testTwoDimensionalOutputIsUnchanged();
    // last: the screen locker's connection is the test's own from here on
    void testLockScreenWallpaperSinks_data();
    void testLockScreenWallpaperSinks();
    void testLockScreenNoSunkLimit();
};

static const QSize s_outputSize(320, 240);
// in pixels at 1920 wide, scaled to the 320 wide output
static constexpr qreal s_poppedLimit = 48;

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

// the colour of column x of a wallpaper, never black: blue tells the left view from the right one
static QRgb columnColor(int x, int view = 0)
{
    return qRgb(x & 0xff, 64 + (x >> 8), view ? 192 : 96);
}

static QImage wallpaperImage(const QSize &size, int view = 0)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < size.width(); ++x) {
        for (int y = 0; y < size.height(); ++y) {
            image.setPixel(x, y, columnColor(x, view));
        }
    }
    return image;
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
    auto image = manager.takeScreenShot(workspace()->outputs().front(), ScreenShotFlags(), std::nullopt);
    if (image) {
        image = image->convertToFormat(QImage::Format_RGB32);
        // KWIN_WALLPAPER_DEPTH_PROOF names a folder for the captures, to look at
        if (const QString directory = qEnvironmentVariable("KWIN_WALLPAPER_DEPTH_PROOF"); !directory.isEmpty()) {
            const QString tag = QString::fromLatin1(QTest::currentDataTag()).replace(QLatin1Char(' '), QLatin1Char('-'));
            image->save(directory + QLatin1Char('/') + QLatin1String(QTest::currentTestFunction()) + (tag.isEmpty() ? QString() : QLatin1Char('-') + tag) + QStringLiteral(".png"));
        }
    }
    return image;
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

struct Desktop
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::LayerSurfaceV1> shellSurface;
    Window *window = nullptr;
};

// the desktop behind everything, as the shell draws it: a layer surface in the background
static Desktop createDesktop()
{
    Desktop result;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createLayerSurfaceV1(result.surface.get(), QStringLiteral("desktop"), nullptr, Test::LayerShellV1::layer_background);
    result.shellSurface->set_size(0, 0);
    result.shellSurface->set_exclusive_zone(-1);
    result.shellSurface->set_anchor(Test::LayerSurfaceV1::anchor_bottom | Test::LayerSurfaceV1::anchor_top
                                    | Test::LayerSurfaceV1::anchor_left | Test::LayerSurfaceV1::anchor_right);
    QSignalSpy configured(result.shellSurface.get(), &Test::LayerSurfaceV1::configureRequested);
    result.surface->commit(KWayland::Client::Surface::CommitFlag::None);
    if (!configured.wait()) {
        return result;
    }
    result.shellSurface->ack_configure(configured.last().at(0).value<quint32>());
    result.window = Test::renderAndWaitForShown(result.surface.get(), wallpaperImage(configured.last().at(1).toSize()));
    return result;
}

// the greeter's own interface on the lock screen, at the screen's level: an opaque block, its clock
static const Rect s_clock(140, 100, 40, 40);

struct LockScreen
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::LayerSurfaceV1> shellSurface;
    std::unique_ptr<KWayland::Client::Surface> wallpaper;
    std::unique_ptr<KWayland::Client::SubSurface> subsurface;
    std::unique_ptr<WaylandClient::Viewport> viewport;
    std::unique_ptr<Test::StereoContentV1> declaration;
    Window *window = nullptr;
};

// The lock screen as the greeter makes it: a layer surface over the output with its interface, and
// its wallpaper in a surface placed below it, full side by side for a stereo wallpaper
static LockScreen createLockScreen(bool stereo)
{
    LockScreen result;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createLayerSurfaceV1(result.surface.get(), QStringLiteral("lockscreen"));
    result.shellSurface->set_size(0, 0);
    result.shellSurface->set_exclusive_zone(-1);
    result.shellSurface->set_keyboard_interactivity(1);
    result.shellSurface->set_anchor(Test::LayerSurfaceV1::anchor_bottom | Test::LayerSurfaceV1::anchor_top
                                    | Test::LayerSurfaceV1::anchor_left | Test::LayerSurfaceV1::anchor_right);
    QSignalSpy configured(result.shellSurface.get(), &Test::LayerSurfaceV1::configureRequested);
    result.surface->commit(KWayland::Client::Surface::CommitFlag::None);
    if (!configured.wait()) {
        return result;
    }
    result.shellSurface->ack_configure(configured.last().at(0).value<quint32>());
    const QSize size = configured.last().at(1).toSize();

    result.wallpaper = Test::createSurface();
    result.subsurface = Test::createSubSurface(result.wallpaper.get(), result.surface.get());
    result.subsurface->placeBelow(result.surface.get());
    if (stereo) {
        QImage pair(QSize(2 * size.width(), size.height()), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&pair);
        painter.drawImage(0, 0, wallpaperImage(size, 0));
        painter.drawImage(size.width(), 0, wallpaperImage(size, 1));
        painter.end();
        result.viewport = Test::viewporter()->createViewport(*result.wallpaper);
        result.viewport->setDestination(size);
        result.declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*result.wallpaper));
        result.declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
        Test::render(result.wallpaper.get(), pair);
    } else {
        Test::render(result.wallpaper.get(), wallpaperImage(size));
    }

    QImage interface(size, QImage::Format_ARGB32_Premultiplied);
    interface.fill(Qt::transparent);
    QPainter painter(&interface);
    painter.fillRect(s_clock, Qt::white);
    painter.end();
    result.window = Test::renderAndWaitForShown(result.surface.get(), interface);
    return result;
}

// Compares each eye of @a image, column by column on three rows, with the wallpaper's columns moved
// by that eye's shift, and black in the columns cut from each side. Says where they differ.
static QString compareEyes(const QImage &image, int cut, int leftShift, int rightShift, int leftView = 0, int rightView = 0, const Rect &interface = Rect())
{
    const int width = s_outputSize.width();
    const struct {
        int offset;
        int shift;
        int view;
    } eyes[] = {{0, leftShift, leftView}, {width, rightShift, rightView}};
    for (const auto &eye : eyes) {
        for (int y : {0, s_outputSize.height() / 2, s_outputSize.height() - 1}) {
            for (int x = 0; x < width; ++x) {
                QRgb expected = x < cut || x >= width - cut ? qRgb(0, 0, 0) : columnColor(x - eye.shift, eye.view);
                if (interface.contains(QPoint(x, y))) {
                    // the interface over the wallpaper, at the same place in both eyes
                    expected = qRgb(255, 255, 255);
                }
                const QRgb pixel = image.pixel(eye.offset + x, y);
                if (pixel != expected) {
                    return QStringLiteral("eye at %1, column %2, row %3: %4, expected %5").arg(eye.offset).arg(x).arg(y).arg(QColor(pixel).name(), QColor(expected).name());
                }
            }
        }
    }
    return QString();
}

void StereoDepthWallpaperTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));

    // no effects: the captures must hold exactly what the surfaces drew
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    // no motion between the levels: every capture is at the levels themselves
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", 0);
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();
}

void StereoDepthWallpaperTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    // the lock screen's cases connect as the screen locker's greeter does
    const bool lockScreen = QByteArray(QTest::currentTestFunction()).startsWith("testLockScreen");
    const int socket = lockScreen ? waylandServer()->createScreenLockerConnection() : -1;
    QVERIFY(!lockScreen || socket >= 0);
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::LayerShellV1 | Test::AdditionalWaylandInterface::Viewporter
                                             | Test::AdditionalWaylandInterface::StereoContentV1,
                                         socket));
    QVERIFY(stereoOutput(true));
    options->setStereoPoppedLimit(s_poppedLimit);
}

void StereoDepthWallpaperTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void StereoDepthWallpaperTest::testDesktopKeepsItsSize_data()
{
    QTest::addColumn<qreal>("sunkLimit");
    QTest::addColumn<int>("cut");
    QTest::addColumn<int>("leftShift");
    QTest::addColumn<int>("rightShift");

    // 96 at 1920 is a parallax of 16 at 320: each copy moves 8, and 8 columns are cut from each side
    QTest::addRow("even parallax") << 96.0 << 8 << -8 << 8;
    // 42 at 1920 is 7 at 320: the left copy moves 4 and the right one 3, and 4 columns are cut
    QTest::addRow("odd parallax") << 42.0 << 4 << -4 << 3;
}

void StereoDepthWallpaperTest::testDesktopKeepsItsSize()
{
    QFETCH(qreal, sunkLimit);
    QFETCH(int, cut);
    QFETCH(int, leftShift);
    QFETCH(int, rightShift);

    options->setStereoSunkLimit(sunkLimit);
    const Desktop desktop = createDesktop();
    QVERIFY(desktop.window && desktop.window->isDesktop());
    // the copies are as far apart as the eyes' shifts say, the wallpaper behind the screen
    QCOMPARE(StereoDepth::parallax(desktop.window, s_outputSize.width()), leftShift - rightShift);

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), QSize(2 * s_outputSize.width(), s_outputSize.height()));
    const QString difference = compareEyes(*image, cut, leftShift, rightShift);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// With no sunk limit the wallpaper is at the screen's level, drawn as it is in both eyes.
void StereoDepthWallpaperTest::testNoSunkLimitNoCut()
{
    options->setStereoSunkLimit(0);
    QVERIFY(StereoDepth::isEnabled());
    const Desktop desktop = createDesktop();
    QVERIFY(desktop.window && desktop.window->isDesktop());

    const auto image = capture();
    QVERIFY(image);
    const QString difference = compareEyes(*image, 0, 0, 0);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// On a 2D output nothing moves and nothing is cut.
void StereoDepthWallpaperTest::testTwoDimensionalOutputIsUnchanged()
{
    options->setStereoSunkLimit(96);
    QVERIFY(stereoOutput(false));
    const Desktop desktop = createDesktop();
    QVERIFY(desktop.window && desktop.window->isDesktop());

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    QCOMPARE(*image, wallpaperImage(s_outputSize).convertToFormat(QImage::Format_RGB32));
}

void StereoDepthWallpaperTest::testLockScreenWallpaperSinks_data()
{
    QTest::addColumn<bool>("stereo");

    QTest::addRow("stereo wallpaper") << true;
    QTest::addRow("flat wallpaper") << false;
}

// The lock screen's wallpaper is a wallpaper: at the desktop's level and cut as the desktop's is,
// while the greeter's interface stays at the screen's level, the same place in both eyes.
void StereoDepthWallpaperTest::testLockScreenWallpaperSinks()
{
    QFETCH(bool, stereo);

    options->setStereoSunkLimit(96);
    const LockScreen lockScreen = createLockScreen(stereo);
    QVERIFY(lockScreen.window && lockScreen.window->isLockScreen());
    QCOMPARE(StereoDepth::parallax(lockScreen.window, s_outputSize.width()), 0);

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), QSize(2 * s_outputSize.width(), s_outputSize.height()));
    const QString difference = compareEyes(*image, 8, -8, 8, 0, stereo ? 1 : 0, s_clock);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// With no sunk limit the lock screen is drawn as it is.
void StereoDepthWallpaperTest::testLockScreenNoSunkLimit()
{
    options->setStereoSunkLimit(0);
    QVERIFY(StereoDepth::isEnabled());
    const LockScreen lockScreen = createLockScreen(true);
    QVERIFY(lockScreen.window && lockScreen.window->isLockScreen());

    const auto image = capture();
    QVERIFY(image);
    const QString difference = compareEyes(*image, 0, 0, 0, 0, 1, s_clock);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthWallpaperTest)
#include "stereo_depth_wallpaper_test.moc"
