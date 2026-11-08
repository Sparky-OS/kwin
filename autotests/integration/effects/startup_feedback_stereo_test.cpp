/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "effect/effecthandler.h"
#include "effect/effectloader.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/surface.h>

#include <QIcon>
#include <QPixmap>

#include <cmath>

namespace KWin
{

/**
 * The launch feedback (the busy cursor) on a stereo output. The icon is part of the pointer: it takes the pointer's plane (the screen's
 * over the active window, the sunk limit over a window beneath it, the popped limit over a menu), and the bounce goes in depth only,
 * between that plane and the popped limit above it, while the icon keeps its place on the screen (Daniel, 2026-11-08: "only bounce on
 * depth - alternating the effect following the 3D enabled in any screen"). On an output that does not show 3D the bounce is KDE's own,
 * up and down on the screen with no parallax, and it follows the pointer from one output to the other and when 3D is switched. The icon
 * is a red square: the red of each eye's half of a capture is the icon, and the horizontal distance between its centre in the two halves
 * is its parallax; the bounce is held at a frame of the animation (the highest, 0, and the lowest, 9).
 */
class StartupFeedbackStereoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testBounceOverTheActiveWindow();
    void testBounceOverAWindowBeneath();
    void testBounceOverAMenu();
    void testTwoDimensionalOutputBouncesOnTheScreen();
    void testTheEffectFollowsThePointerBetweenOutputs();
    void testTheEffectFollowsThreeDimensionsSwitchedOnTheOutput();
};

static const QSize s_outputSize(320, 240);
// limits in pixels at 1920 wide, scaled to the 320 wide output: sunk 16 and popped 10 pixels
static constexpr qreal s_sunkLimit = 96;
static constexpr qreal s_poppedLimit = 60;
static constexpr int s_layer = 3;
static constexpr int s_popped = 10;
// the frames of the bounce: the icon is highest at 0 and lowest from 8 to 11
static constexpr int s_highest = 0;
static constexpr int s_lowest = 9;

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
    std::unique_ptr<Test::XdgPopup> popup;
    Window *window = nullptr;

    Test::XdgSurface *xdgSurface() const
    {
        return toplevel ? toplevel->xdgSurface() : popup->xdgSurface();
    }
};

static std::unique_ptr<TestBox> createWindow(const QColor &color, const QPoint &position, const QSize &size)
{
    auto result = std::make_unique<TestBox>();
    result->surface = Test::createSurface();
    result->toplevel = Test::createXdgToplevelSurface(result->surface.get());
    result->window = Test::renderAndWaitForShown(result->surface.get(), size, color);
    if (result->window) {
        result->window->move(QPointF(position));
    }
    return result;
}

static std::unique_ptr<TestBox> createMenu(const TestBox &parent, const QColor &color, const QPoint &position, const QSize &size)
{
    auto result = std::make_unique<TestBox>();
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

static Effect *s_effect = nullptr;
static quint32 s_time = 0;

static void pointerTo(const QPointF &position)
{
    Test::pointerMotion(position, ++s_time);
    QVERIFY(Test::waylandSync());
}

static std::optional<QImage> capture(LogicalOutput *output)
{
    QSignalSpy rendered(kwinApp()->scene(), &WorkspaceScene::frameRendered);
    kwinApp()->scene()->addRepaintFull();
    if (!rendered.wait()) {
        return std::nullopt;
    }
    QTest::qWait(100);
    ScreenShotManager manager;
    return manager.takeScreenShot(output, ScreenShotFlags(), std::nullopt);
}

struct Icon
{
    int pixels = 0;
    qreal x = 0;
    qreal y = 0;
};

// the red of the part of @a image that starts at @a xOffset: the icon
static Icon redOf(const QImage &image, int xOffset)
{
    Icon icon;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < s_outputSize.width() && xOffset + x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(xOffset + x, y);
            if (pixel.red() > 180 && pixel.green() < 90 && pixel.blue() < 90) {
                icon.pixels++;
                icon.x += x;
                icon.y += y;
            }
        }
    }
    if (icon.pixels) {
        icon.x /= icon.pixels;
        icon.y /= icon.pixels;
    }
    return icon;
}

static void holdFrame(int frame)
{
    s_effect->setProperty("frame", frame);
    QTest::qWait(50);
}

static LogicalOutput *outputAt(int x)
{
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        if (output->geometry().contains(QPoint(x, 10))) {
            return output;
        }
    }
    return nullptr;
}

static bool stereoOutput(LogicalOutput *output, bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(output->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && output->hasStereoEyes() == enabled;
}

static void startFeedback()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::red);
    Q_EMIT effects->startupAdded(QStringLiteral("test"), QIcon(pixmap));
    QTest::qWait(100);
}

// the icon's parallax in the stereo output's capture with the bounce held at @a frame: left eye's centre minus right eye's
static bool measure(int frame, qreal *parallax, qreal *y)
{
    holdFrame(frame);
    const auto image = capture(outputAt(10));
    if (!image || image->width() != 2 * s_outputSize.width()) {
        return false;
    }
    const Icon left = redOf(*image, 0);
    const Icon right = redOf(*image, s_outputSize.width());
    if (!left.pixels || !right.pixels || std::abs(left.y - right.y) > 0.75) {
        return false;
    }
    *parallax = left.x - right.x;
    *y = left.y;
    return true;
}

void StartupFeedbackStereoTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    qputenv("KWIN_EFFECTS_FORCE_ANIMATIONS", QByteArrayLiteral("1"));
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
    // no motion between the levels: the pointer is at the level of what it is over at once
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", 0);
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();
}

void StartupFeedbackStereoTest::init()
{
    Test::setOutputConfig({
        Test::OutputInfo{.geometry = Rect(QPoint(), s_outputSize), .modes = {plainMode(), stereoMode()}},
        Test::OutputInfo{.geometry = Rect(QPoint(s_outputSize.width(), 0), s_outputSize), .modes = {plainMode()}},
    });
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat));
    QVERIFY(stereoOutput(outputAt(10), true));
    options->setStereoSunkLimit(s_sunkLimit);
    options->setStereoPoppedLimit(s_poppedLimit);
    QVERIFY(effects->loadEffect(QStringLiteral("startupfeedback")));
    s_effect = effects->findEffect(QStringLiteral("startupfeedback"));
    QVERIFY(s_effect);
}

void StartupFeedbackStereoTest::cleanup()
{
    if (s_effect) {
        s_effect->setProperty("frame", -1);
    }
    effects->unloadAllEffects();
    s_effect = nullptr;
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

// the icon at the highest and at the lowest frame of the bounce with the pointer at @a position: it moves in depth only, from the
// pointer's plane up to the popped limit above it
static void checkBounce(const QPointF &position, int plane)
{
    pointerTo(position);
    startFeedback();
    qreal highest = 0, lowest = 0, highestY = 0, lowestY = 0;
    QVERIFY(measure(s_highest, &highest, &highestY));
    QVERIFY(measure(s_lowest, &lowest, &lowestY));
    QVERIFY2(std::abs(highest - (plane + s_popped)) < 0.75, qPrintable(QStringLiteral("highest %1, expected %2").arg(highest).arg(plane + s_popped)));
    QVERIFY2(std::abs(lowest - plane) < 0.75, qPrintable(QStringLiteral("lowest %1, expected %2").arg(lowest).arg(plane)));
    // it does not move on the screen
    QVERIFY2(std::abs(highestY - lowestY) < 0.75, qPrintable(QStringLiteral("y %1 and %2").arg(highestY).arg(lowestY)));
    // and never beyond the pointer's plane plus the popped limit
    for (int frame = 0; frame < 20; frame += 3) {
        qreal parallax = 0, y = 0;
        QVERIFY(measure(frame, &parallax, &y));
        QVERIFY2(parallax > plane - 0.75 && parallax < plane + s_popped + 0.75, qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(parallax)));
    }
}

void StartupFeedbackStereoTest::testBounceOverTheActiveWindow()
{
    auto active = createWindow(Qt::blue, QPoint(170, 20), QSize(140, 160));
    QVERIFY(active->window);
    workspace()->activateWindow(active->window);
    checkBounce(QPointF(250, 120), 0);
}

void StartupFeedbackStereoTest::testBounceOverAWindowBeneath()
{
    // the window beneath the active one is a layer behind the screen (a fifth of the sunk limit), and the icon bounces up from it: never to the screen
    auto beneath = createWindow(Qt::green, QPoint(10, 20), QSize(140, 160));
    auto active = createWindow(Qt::blue, QPoint(170, 20), QSize(140, 160));
    QVERIFY(beneath->window && active->window);
    workspace()->activateWindow(active->window);
    QVERIFY(Test::waylandSync());
    checkBounce(QPointF(40, 80), -s_layer);
}

void StartupFeedbackStereoTest::testBounceOverAMenu()
{
    auto active = createWindow(Qt::blue, QPoint(170, 20), QSize(140, 160));
    QVERIFY(active->window);
    workspace()->activateWindow(active->window);
    auto menu = createMenu(*active, Qt::gray, QPoint(10, 10), QSize(60, 40));
    QVERIFY(menu->window);
    checkBounce(QPointF(190, 40), s_popped);
}

void StartupFeedbackStereoTest::testTwoDimensionalOutputBouncesOnTheScreen()
{
    // a control: on an output that does not show 3D the bounce is KDE's, up and down on the screen with no parallax
    QVERIFY(stereoOutput(outputAt(10), false));
    pointerTo(QPointF(100, 100));
    startFeedback();
    holdFrame(s_highest);
    const auto top = capture(outputAt(10));
    holdFrame(s_lowest);
    const auto bottom = capture(outputAt(10));
    QVERIFY(top && bottom);
    QCOMPARE(top->width(), s_outputSize.width());
    const Icon a = redOf(*top, 0);
    const Icon b = redOf(*bottom, 0);
    QVERIFY(a.pixels && b.pixels);
    // from -5 to 15 pixels: 20 down
    QVERIFY2(b.y - a.y > 15, qPrintable(QStringLiteral("the icon moved %1 pixels").arg(b.y - a.y)));
    QVERIFY(std::abs(b.x - a.x) < 1.5);
}

void StartupFeedbackStereoTest::testTheEffectFollowsThePointerBetweenOutputs()
{
    // 3D on the first output, none on the second: the depth bounce on the first, the 2D bounce on the second, and back
    startFeedback();
    qreal highest = 0, lowest = 0, highestY = 0, lowestY = 0;
    pointerTo(QPointF(100, 100));
    QVERIFY(measure(s_highest, &highest, &highestY));
    QVERIFY(measure(s_lowest, &lowest, &lowestY));
    QVERIFY(std::abs(highest - s_popped) < 0.75 && std::abs(lowest) < 0.75 && std::abs(highestY - lowestY) < 0.75);

    pointerTo(QPointF(s_outputSize.width() + 100, 100));
    holdFrame(s_highest);
    const auto top = capture(outputAt(s_outputSize.width() + 10));
    holdFrame(s_lowest);
    const auto bottom = capture(outputAt(s_outputSize.width() + 10));
    QVERIFY(top && bottom);
    QCOMPARE(top->width(), s_outputSize.width());
    QVERIFY(redOf(*top, 0).pixels && redOf(*bottom, 0).pixels);
    QVERIFY(redOf(*bottom, 0).y - redOf(*top, 0).y > 15);
    // nothing of it is left on the first output
    const auto first = capture(outputAt(10));
    QVERIFY(first);
    QCOMPARE(redOf(*first, 0).pixels, 0);

    pointerTo(QPointF(100, 100));
    QVERIFY(measure(s_highest, &highest, &highestY));
    QVERIFY(measure(s_lowest, &lowest, &lowestY));
    QVERIFY(std::abs(highest - s_popped) < 0.75 && std::abs(lowest) < 0.75 && std::abs(highestY - lowestY) < 0.75);
}

void StartupFeedbackStereoTest::testTheEffectFollowsThreeDimensionsSwitchedOnTheOutput()
{
    pointerTo(QPointF(100, 100));
    startFeedback();
    qreal highest = 0, highestY = 0, lowest = 0, lowestY = 0;
    QVERIFY(measure(s_highest, &highest, &highestY));
    QVERIFY(std::abs(highest - s_popped) < 0.75);
    // 3D switched off under the pointer: the 2D bounce
    QVERIFY(stereoOutput(outputAt(10), false));
    holdFrame(s_highest);
    const auto top = capture(outputAt(10));
    holdFrame(s_lowest);
    const auto bottom = capture(outputAt(10));
    QVERIFY(top && bottom);
    QCOMPARE(top->width(), s_outputSize.width());
    QVERIFY(redOf(*bottom, 0).y - redOf(*top, 0).y > 15);
    // and on again: the depth bounce
    QVERIFY(stereoOutput(outputAt(10), true));
    QVERIFY(measure(s_highest, &highest, &highestY));
    QVERIFY(measure(s_lowest, &lowest, &lowestY));
    QVERIFY(std::abs(highest - s_popped) < 0.75 && std::abs(lowest) < 0.75 && std::abs(highestY - lowestY) < 0.75);
}

}

WAYLANDTEST_MAIN(KWin::StartupFeedbackStereoTest)
#include "startup_feedback_stereo_test.moc"
