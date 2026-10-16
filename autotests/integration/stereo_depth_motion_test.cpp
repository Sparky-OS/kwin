/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "cursor.h"
#include "effect/effectloader.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/stereodepth.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/pointer.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/shm_pool.h>
#include <KWayland/Client/surface.h>

#include <QFile>

using namespace std::chrono_literals;

namespace KWin
{

/**
 * A window or the pointer that changes its depth level reaches the new level in a short eased
 * motion: towards the viewer in 80 ms, away from it in 150 ms at the default animation speed,
 * easing out, and at once when animations are off. The depth is read from captures of the stereo
 * output: the parallax of a window is the horizontal distance of its colour in the left eye's half
 * to the same colour in the right eye's half, and each capture is taken at a known time.
 */
class StereoDepthMotionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testMotionCurve();
    void testWindowsEase();
    void testPointerEases();
    void testInstantWithoutAnimations();
    void testTwoDimensionalOutputIsUnchanged();
};

static const QSize s_outputSize(320, 240);
// limits in pixels at 1920 wide, scaled to the 320 wide output: sunk 32 and popped 16 pixels
static constexpr qreal s_sunkLimit = 192;
static constexpr qreal s_poppedLimit = 96;
static constexpr int s_sunk = -32;
// the animations run this many times slower, so that captures land along the motions
static constexpr qreal s_slow = 10;

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

struct TestWindow
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shellSurface;
    Window *window = nullptr;
    QColor color;
    QPoint position;
};

static std::optional<TestWindow> s_background;

static TestWindow createWindow(const QColor &color, const QPoint &position, const QSize &size = QSize(80, 80))
{
    TestWindow result;
    result.color = color;
    result.position = position;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createXdgToplevelSurface(result.surface.get());
    result.window = Test::renderAndWaitForShown(result.surface.get(), size, color);
    if (result.window) {
        result.window->move(position);
    }
    return result;
}

// the x of the first pixel of the colour on the row at y of the half that starts at xOffset, or -1
static int firstX(const QImage &image, int xOffset, int y, const QColor &color)
{
    for (int x = 0; x < s_outputSize.width(); ++x) {
        if (image.pixelColor(xOffset + x, y) == color) {
            return x;
        }
    }
    return -1;
}

struct Seen
{
    int left = -1;
    int right = -1;
    int parallax() const
    {
        return left - right;
    }
};

static Seen seenOn(const QImage &image, int y, const QColor &color)
{
    return Seen{firstX(image, 0, y, color), firstX(image, s_outputSize.width(), y, color)};
}

// a capture of the scene as it is now, at the time returned with it
static std::optional<std::pair<QImage, std::chrono::nanoseconds>> captureNow(bool includeCursor = false)
{
    ScreenShotManager manager;
    const auto time = std::chrono::steady_clock::now().time_since_epoch();
    const auto image = manager.takeScreenShot(workspace()->outputs().front(), includeCursor ? ScreenShotFlags(ScreenShotIncludeCursor) : ScreenShotFlags(), std::nullopt);
    if (!image) {
        return std::nullopt;
    }
    return std::make_pair(*image, time);
}

// a capture once every motion has ended
static std::optional<QImage> captureSettled(bool includeCursor = false)
{
    QTest::qWait(150 * s_slow + 200);
    const auto capture = captureNow(includeCursor);
    return capture ? std::optional(capture->first) : std::nullopt;
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

static void setAnimationSpeed(qreal factor)
{
    auto config = kwinApp()->config();
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", factor);
    config->sync();
    options->loadConfig();
    options->setStereoSunkLimit(s_sunkLimit);
    options->setStereoPoppedLimit(s_poppedLimit);
}

// the eased motion from @a from to @a to after @a elapsed of @a duration, the closed form of an out-cubic
static qreal expectedDepth(qreal from, qreal to, std::chrono::nanoseconds elapsed, std::chrono::nanoseconds duration)
{
    const qreal t = std::clamp(qreal(elapsed.count()) / duration.count(), qreal(0), qreal(1));
    return from + (to - from) * (1 - std::pow(1 - t, 3));
}

// the same in pixels of parallax on the output
static int expectedParallax(int from, int to, std::chrono::nanoseconds elapsed, std::chrono::nanoseconds duration)
{
    return std::lround(expectedDepth(from, to, elapsed, duration));
}

static QFile *s_samples = nullptr;

static void writeSample(const char *what, std::chrono::nanoseconds elapsed, const Seen &seen, int expected, int x)
{
    if (s_samples) {
        s_samples->write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
                             .arg(QLatin1StringView(what))
                             .arg(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0)
                             .arg(seen.left - x)
                             .arg(seen.right - x)
                             .arg(seen.parallax())
                             .arg(expected)
                             .arg(x)
                             .toUtf8());
        s_samples->flush();
    }
}

void StereoDepthMotionTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));

    // no effects: the captures must hold exactly what the windows drew
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    KConfigGroup depth(config, QStringLiteral("StereoDepth"));
    depth.writeEntry("SunkLimit", s_sunkLimit);
    depth.writeEntry("PoppedLimit", s_poppedLimit);
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", s_slow);
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();

    const QString directory = qEnvironmentVariable("KWIN_DEPTH_MOTION_PROOF");
    if (!directory.isEmpty()) {
        s_samples = new QFile(directory + QStringLiteral("/samples.csv"));
        QVERIFY(s_samples->open(QIODevice::WriteOnly | QIODevice::Truncate));
        s_samples->write("what,ms,left eye shift,right eye shift,parallax,expected,x\n");
    }
}

void StereoDepthMotionTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat));
    QVERIFY(Test::waitForWaylandPointer());
    QVERIFY(stereoOutput(true));
    setAnimationSpeed(s_slow);
    s_background = createWindow(Qt::black, QPoint(-32, -32), s_outputSize + QSize(64, 64));
    QVERIFY(s_background->window);
    s_background->window->setKeepBelow(true);
}

void StereoDepthMotionTest::cleanup()
{
    s_background.reset();
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void StereoDepthMotionTest::testMotionCurve()
{
    // the durations at the default speed, and as they follow the animation speed
    setAnimationSpeed(1);
    QCOMPARE(StereoDepth::Motion::riseDuration(), std::chrono::nanoseconds(80ms));
    QCOMPARE(StereoDepth::Motion::sinkDuration(), std::chrono::nanoseconds(150ms));
    setAnimationSpeed(2);
    QCOMPARE(StereoDepth::Motion::riseDuration(), std::chrono::nanoseconds(160ms));
    QCOMPARE(StereoDepth::Motion::sinkDuration(), std::chrono::nanoseconds(300ms));

    setAnimationSpeed(1);
    const std::chrono::nanoseconds start = 1000s;
    StereoDepth::Motion motion;
    // the first depth is taken at once
    motion.setTarget(-10, start);
    QCOMPARE(motion.depth(start), -10);
    QVERIFY(!motion.isMoving(start));

    // towards the viewer, out-cubic over 80 ms
    motion.setTarget(4, start);
    for (const auto elapsed : {0ms, 8ms, 20ms, 40ms, 60ms, 79ms, 80ms, 200ms}) {
        QVERIFY(qAbs(motion.depth(start + elapsed) - expectedDepth(-10, 4, elapsed, 80ms)) < 1e-9);
    }
    QVERIFY(motion.isMoving(start + 79ms));
    QVERIFY(!motion.isMoving(start + 80ms));
    // ease out: most of the way in the first quarter
    QVERIFY(motion.depth(start + 20ms) > -10 + 0.5 * 14);

    // away from the viewer, out-cubic over 150 ms
    const std::chrono::nanoseconds sink = start + 1s;
    motion.setTarget(-6, sink);
    for (const auto elapsed : {0ms, 15ms, 37ms, 75ms, 112ms, 150ms}) {
        QVERIFY(qAbs(motion.depth(sink + elapsed) - expectedDepth(4, -6, elapsed, 150ms)) < 1e-9);
    }

    // a new level on the way starts from where the motion is, without a jump
    const std::chrono::nanoseconds turn = sink + 50ms;
    const qreal there = motion.depth(turn);
    motion.setTarget(8, turn);
    QCOMPARE(motion.depth(turn), there);
    QVERIFY(qAbs(motion.depth(turn + 40ms) - expectedDepth(there, 8, 40ms, 80ms)) < 1e-9);
    QCOMPARE(motion.depth(turn + 80ms), 8);

    // animations off: at once
    setAnimationSpeed(0);
    motion.setTarget(-12, turn + 1s);
    QVERIFY(!motion.isMoving(turn + 1s));
    QCOMPARE(motion.depth(turn + 1s), -12);
}

void StereoDepthMotionTest::testWindowsEase()
{
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(180, 20));
    QVERIFY(a.window && b.window);
    workspace()->activateWindow(a.window);
    auto settled = captureSettled();
    QVERIFY(settled);
    QCOMPARE(seenOn(*settled, 60, a.color).parallax(), 0);
    QCOMPARE(seenOn(*settled, 60, b.color).parallax(), s_sunk);

    // b comes to the screen quickly, a goes to the bottom of the pile gently
    workspace()->activateWindow(b.window);
    const auto first = captureNow();
    QVERIFY(first);
    const std::chrono::nanoseconds start = first->second;
    const auto rise = StereoDepth::Motion::riseDuration();
    const auto sink = StereoDepth::Motion::sinkDuration();
    QCOMPARE(rise, std::chrono::nanoseconds(800ms));
    std::optional<QImage> image = first->first;
    std::chrono::nanoseconds elapsed = 0ns;
    int middle = 0;
    while (true) {
        const Seen seenA = seenOn(*image, 60, a.color);
        const Seen seenB = seenOn(*image, 60, b.color);
        const int expectedA = expectedParallax(0, s_sunk, elapsed, sink);
        const int expectedB = expectedParallax(s_sunk, 0, elapsed, rise);
        writeSample("sinking window", elapsed, seenA, expectedA, a.position.x());
        writeSample("rising window", elapsed, seenB, expectedB, b.position.x());
        qInfo() << "measured at" << elapsed.count() / 1000000 << "ms: sinking" << seenA.parallax() << "expected" << expectedA
                << "rising" << seenB.parallax() << "expected" << expectedB;
        // the capture's time is known to a fraction of a millisecond, the parallax to the rounding
        QVERIFY(std::abs(seenA.parallax() - expectedA) <= 1);
        QVERIFY(std::abs(seenB.parallax() - expectedB) <= 1);
        // each eye moves half of it, the left one to the right when nearer
        QCOMPARE(seenA.left - a.position.x(), StereoDepth::eyeShift(seenA.parallax(), StereoEye::Left));
        QCOMPARE(seenA.right - a.position.x(), StereoDepth::eyeShift(seenA.parallax(), StereoEye::Right));
        QCOMPARE(seenB.left - b.position.x(), StereoDepth::eyeShift(seenB.parallax(), StereoEye::Left));
        if (seenB.parallax() != 0 && seenB.parallax() != s_sunk) {
            middle++;
        }
        if (elapsed > sink + 100ms) {
            QCOMPARE(seenA.parallax(), s_sunk);
            QCOMPARE(seenB.parallax(), 0);
            break;
        }
        const QString directory = qEnvironmentVariable("KWIN_DEPTH_MOTION_PROOF");
        if (!directory.isEmpty() && elapsed >= 200ms && elapsed < 350ms) {
            QVERIFY(image->save(directory + QStringLiteral("/windows-mid.png")));
        }
        QTest::qWait(100);
        const auto next = captureNow();
        QVERIFY(next);
        image = next->first;
        elapsed = next->second - start;
    }
    // the motion was seen on its way, not as a jump
    QVERIFY(middle >= 3);
}

void StereoDepthMotionTest::testPointerEases()
{
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(180, 20));
    QVERIFY(a.window && b.window);
    workspace()->activateWindow(b.window);

    std::unique_ptr<KWayland::Client::Pointer> pointer(Test::waylandSeat()->createPointer());
    QSignalSpy entered(pointer.get(), &KWayland::Client::Pointer::entered);
    quint32 time = 0;
    Test::pointerMotion(QPointF(60, 60), ++time);
    QVERIFY(entered.wait());
    auto cursorSurface = Test::createSurface();
    QImage green(QSize(10, 10), QImage::Format_ARGB32_Premultiplied);
    green.fill(Qt::green);
    cursorSurface->attachBuffer(Test::waylandShmPool()->createBuffer(green));
    cursorSurface->damage(green.rect());
    cursorSurface->commit();
    pointer->setCursor(cursorSurface.get(), QPoint(5, 5));
    Test::flushWaylandConnection();
    QTRY_COMPARE(kwinApp()->cursorImage().image().convertToFormat(green.format()), green);

    // over the sunk window the pointer is sunk with it
    auto settled = captureSettled(true);
    QVERIFY(settled);
    QCOMPARE(seenOn(*settled, 60, Qt::green).parallax(), s_sunk);

    // onto the active window: up to the screen in a quick motion, then back down gently
    for (const bool up : {true, false}) {
        const QPointF position = up ? QPointF(220, 60) : QPointF(60, 60);
        Test::pointerMotion(position, ++time);
        const auto first = captureNow(true);
        QVERIFY(first);
        const std::chrono::nanoseconds start = first->second;
        const auto duration = up ? StereoDepth::Motion::riseDuration() : StereoDepth::Motion::sinkDuration();
        const int from = up ? s_sunk : 0;
        const int to = up ? 0 : s_sunk;
        std::optional<QImage> image = first->first;
        std::chrono::nanoseconds elapsed = 0ns;
        int middle = 0;
        while (true) {
            const Seen seen = seenOn(*image, 60, Qt::green);
            const int expected = expectedParallax(from, to, elapsed, duration);
            writeSample(up ? "pointer rising" : "pointer sinking", elapsed, seen, expected, position.x() - 5);
            qInfo() << "measured pointer at" << elapsed.count() / 1000000 << "ms:" << seen.parallax() << "expected" << expected;
            QVERIFY(std::abs(seen.parallax() - expected) <= 1);
            middle += seen.parallax() != from && seen.parallax() != to;
            if (elapsed > duration + 100ms) {
                QCOMPARE(seen.parallax(), to);
                break;
            }
            QTest::qWait(up ? 60 : 100);
            const auto next = captureNow(true);
            QVERIFY(next);
            image = next->first;
            elapsed = next->second - start;
        }
        QVERIFY(middle >= 3);
    }
}

void StereoDepthMotionTest::testInstantWithoutAnimations()
{
    setAnimationSpeed(0);
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(180, 20));
    QVERIFY(a.window && b.window);
    workspace()->activateWindow(a.window);
    QVERIFY(captureSettled());

    workspace()->activateWindow(b.window);
    const auto capture = captureNow();
    QVERIFY(capture);
    QCOMPARE(seenOn(capture->first, 60, a.color).parallax(), s_sunk);
    QCOMPARE(seenOn(capture->first, 60, b.color).parallax(), 0);
}

void StereoDepthMotionTest::testTwoDimensionalOutputIsUnchanged()
{
    QVERIFY(stereoOutput(false));
    const TestWindow a = createWindow(Qt::red, QPoint(20, 20));
    const TestWindow b = createWindow(Qt::blue, QPoint(180, 20));
    QVERIFY(a.window && b.window);
    workspace()->activateWindow(a.window);
    QVERIFY(captureSettled());

    // along what would be a motion on a stereo output, the windows stay exactly where they are
    workspace()->activateWindow(b.window);
    for (int i = 0; i < 5; ++i) {
        const auto capture = captureNow();
        QVERIFY(capture);
        QCOMPARE(capture->first.size(), s_outputSize);
        QCOMPARE(firstX(capture->first, 0, 60, a.color), 20);
        QCOMPARE(firstX(capture->first, 0, 60, b.color), 180);
        QTest::qWait(100);
    }
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthMotionTest)
#include "stereo_depth_motion_test.moc"
