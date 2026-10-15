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

#include <QDir>
#include <QFile>
#include <QTextStream>

#include <cmath>

namespace KWin
{

/**
 * Fall Apart in depth on a stereo output. The window closed is a red square on black, so the
 * red of each eye's half of the capture is the pieces, and the horizontal distance between the
 * centre of the red in the left eye's half and in the right eye's half is the parallax of every
 * piece (all of them have the same depth here: no spread, one thing beneath them). It is
 * compared with the curve the effect is specified to follow, at the progress it was at when the
 * capture was made.
 */
class FallApartStereoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testFlyComesTowardsTheViewer();
    void testDiveFallsToTheFloorAndBounces();
    void testDiveStopsAtWhatItCovers();
    void testDiveStopsAtTheEffectsOwnLimit();
    void testTwoDimensionalOutputIgnoresDepth();
};

static const QSize s_outputSize(1280, 720);
// pixels of parallax per pixel of the limits, which are given for a view 1920 wide
static constexpr qreal s_scale = 1280.0 / 1920.0;
// the desktop's limits and the effects' own, in pixels of a view 1920 wide
static constexpr qreal s_sunkLimit = 30;
static constexpr qreal s_poppedLimit = 12;
static constexpr qreal s_effectSunkLimit = 90;
static constexpr qreal s_effectPoppedLimit = 120;

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
};

static TestWindow s_background;
static TestWindow s_subject;
static Effect *s_effect = nullptr;

static TestWindow createWindow(const QColor &color, const QPoint &position, const QSize &size)
{
    TestWindow result;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createXdgToplevelSurface(result.surface.get());
    result.window = Test::renderAndWaitForShown(result.surface.get(), size, color);
    if (result.window) {
        result.window->move(position);
    }
    return result;
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

static void setLimits(qreal sunk, qreal popped, qreal effectSunk, qreal effectPopped)
{
    options->setStereoSunkLimit(sunk);
    options->setStereoPoppedLimit(popped);
    options->setStereoEffectSunkLimit(effectSunk);
    options->setStereoEffectPoppedLimit(effectPopped);
}

static void configureEffect(const QString &mode, int spread)
{
    KConfigGroup group(kwinApp()->config(), QStringLiteral("Effect-fallapart"));
    group.writeEntry("Mode", mode);
    group.writeEntry("BlockSize", 20);
    group.writeEntry("DepthSpread", spread);
    group.sync();
    effects->reconfigureEffect(QStringLiteral("fallapart"));
}

// Closes the red window, so that the effect has its pieces
static bool closeSubject()
{
    QSignalSpy closed(s_subject.window, &Window::closed);
    s_subject.shellSurface.reset();
    s_subject.surface.reset();
    return closed.wait() && s_effect->isActive();
}

static void setProgress(qreal progress)
{
    s_effect->setProperty("progress", progress);
}

static qreal progress()
{
    return s_effect->property("progress").toReal();
}

struct Eye
{
    qreal mass = 0;
    qreal x = 0;
    qreal y = 0;
    QRect bounds;
};

static Eye measure(const QImage &image, int xOffset)
{
    Eye eye;
    qreal sumX = 0;
    qreal sumY = 0;
    int left = std::numeric_limits<int>::max();
    int right = -1;
    int top = std::numeric_limits<int>::max();
    int bottom = -1;
    for (int y = 0; y < s_outputSize.height(); ++y) {
        for (int x = 0; x < s_outputSize.width(); ++x) {
            const int red = qRed(image.pixel(xOffset + x, y));
            if (red > 16) {
                eye.mass += red;
                sumX += red * (x + 0.5);
                sumY += red * (y + 0.5);
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (eye.mass > 0) {
        eye.x = sumX / eye.mass;
        eye.y = sumY / eye.mass;
        eye.bounds = QRect(QPoint(left, top), QPoint(right, bottom));
    }
    return eye;
}

static std::optional<QImage> capture()
{
    ScreenShotManager manager;
    return manager.takeScreenShot(workspace()->outputs().front(), ScreenShotFlags(), std::nullopt);
}

// With KWIN_FALLAPART_PROOF set, every capture is saved there as a full side by side picture, left
// eye first, and its measurement is added to samples.csv
static void record(const QString &name, const QImage &image, qreal progress, qreal parallax, qreal expected)
{
    const QString directory = qEnvironmentVariable("KWIN_FALLAPART_PROOF");
    if (directory.isEmpty()) {
        return;
    }
    QDir().mkpath(directory);
    image.save(QStringLiteral("%1/%2-%3.png").arg(directory, name, QString::number(progress, 'f', 3)));
    QFile csv(directory + QStringLiteral("/samples.csv"));
    if (csv.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&csv) << name << ',' << QString::number(progress, 'f', 4) << ',' << QString::number(parallax, 'f', 3) << ',' << QString::number(expected, 'f', 3) << '\n';
    }
}

// where a piece is on its fall, from 0 at the start to 1 at the floor, for the specified
// fall of the animation: it falls with constant acceleration, then bounces on the floor, each
// bounce half as long and a quarter as high as the one before
static qreal expectedDive(qreal p, qreal fall)
{
    if (p <= fall) {
        return (p / fall) * (p / fall);
    }
    qreal start = fall;
    qreal duration = fall;
    qreal height = 0.25;
    for (;;) {
        if (p <= start + duration) {
            const qreal x = (p - start) / duration;
            return 1 - height * 4 * x * (1 - x);
        }
        start += duration;
        duration /= 2;
        height /= 4;
    }
}

void FallApartStereoTest::initTestCase()
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
    // the animation is so slow that its own clock hardly moves it between captures: the progress
    // is the one the test sets
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", 1000000.0);
    config->sync();
    kwinApp()->setConfig(config);
    kwinApp()->start();
}

void FallApartStereoTest::init()
{
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection());
    QVERIFY(stereoOutput(true));
    setLimits(s_sunkLimit, s_poppedLimit, s_effectSunkLimit, s_effectPoppedLimit);

    // what is beneath everything: a black window kept at the desktop's sunk limit
    s_background = createWindow(Qt::black, QPoint(-32, -32), s_outputSize + QSize(64, 64));
    QVERIFY(s_background.window);
    s_background.window->setKeepBelow(true);

    QVERIFY(effects->loadEffect(QStringLiteral("fallapart")));
    s_effect = effects->findEffect(QStringLiteral("fallapart"));
    QVERIFY(s_effect);

    // the window to be closed, active, at the screen's level
    s_subject = createWindow(Qt::red, QPoint(600, 320), QSize(80, 80));
    QVERIFY(s_subject.window);
    workspace()->activateWindow(s_subject.window);
    QVERIFY(Test::waylandSync());
    QCOMPARE(workspace()->activeWindow(), s_subject.window);
}

void FallApartStereoTest::cleanup()
{
    s_subject = {};
    s_background = {};
    effects->unloadAllEffects();
    s_effect = nullptr;
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void FallApartStereoTest::testFlyComesTowardsTheViewer()
{
    configureEffect(QStringLiteral("Fly"), 0);
    QVERIFY(closeSubject());

    // while all the pieces are well inside the output, away from the edge where they go flat
    int measured = 0;
    qreal previous = -1;
    for (qreal p = 0.05; p < 0.41; p += 0.05) {
        setProgress(p);
        const auto image = capture();
        QVERIFY(image);
        const qreal at = progress();
        const Eye left = measure(*image, 0);
        const Eye right = measure(*image, s_outputSize.width());
        QVERIFY(left.mass > 0 && right.mass > 0);
        const QRect inside = QRect(QPoint(), s_outputSize).adjusted(60, 60, -60, -60);
        QVERIFY2(inside.contains(left.bounds) && inside.contains(right.bounds), qPrintable(QStringLiteral("pieces near the edge at %1").arg(at)));
        const qreal parallax = left.x - right.x;
        const qreal expected = s_scale * s_effectPoppedLimit * at * at * at;
        QVERIFY2(std::abs(parallax - expected) < 0.5, qPrintable(QStringLiteral("progress %1: parallax %2, expected %3").arg(at).arg(parallax).arg(expected)));
        // the eyes see the pieces at the same height, and they are in front of the screen
        QVERIFY(std::abs(left.y - right.y) < 0.75);
        QVERIFY(parallax > previous);
        previous = parallax;
        measured++;
        record(QStringLiteral("fly"), *image, at, parallax, expected);
        qInfo("FLY progress %.4f parallax %.3f expected %.3f", at, parallax, expected);
    }
    QCOMPARE(measured, 8);
    QVERIFY(previous > 4);
}

static void checkDive(const QString &name, qreal floorParallax)
{
    // the pieces dive for 0.4 of the animation, without spread
    int bounced = 0;
    int floorSamples = 0;
    qreal deepest = 0;
    for (qreal p = 0.05; p < 0.96; p += 0.05) {
        setProgress(p);
        const auto image = capture();
        QVERIFY(image);
        const qreal at = progress();
        const Eye left = measure(*image, 0);
        const Eye right = measure(*image, s_outputSize.width());
        QVERIFY(left.mass > 0 && right.mass > 0);
        const qreal parallax = left.x - right.x;
        const qreal expected = floorParallax * expectedDive(at, 0.4);
        QVERIFY2(std::abs(parallax - expected) < 0.5, qPrintable(QStringLiteral("progress %1: parallax %2, expected %3").arg(at).arg(parallax).arg(expected)));
        // never beyond the floor, which is the nearest thing the pieces cover or the user's limit
        QVERIFY(parallax > floorParallax - 0.5);
        QVERIFY(std::abs(left.y - right.y) < 0.75);
        deepest = std::min(deepest, parallax);
        if (at > 0.45 && parallax > floorParallax * 0.8) {
            bounced++;
        }
        if (parallax < floorParallax + 0.5) {
            floorSamples++;
        }
        record(name, *image, at, parallax, expected);
        qInfo("DIVE progress %.4f parallax %.3f expected %.3f", at, parallax, expected);
    }
    QVERIFY(deepest < floorParallax + 0.5);
    QVERIFY(floorSamples >= 1);
    // after the first landing they come up again
    QVERIFY(bounced >= 1);
}

void FallApartStereoTest::testDiveFallsToTheFloorAndBounces()
{
    // the black window is at the sunk limit: -30 pixels of a view 1920 wide
    configureEffect(QStringLiteral("Dive"), 0);
    QVERIFY(closeSubject());
    checkDive(QStringLiteral("dive"), -s_scale * s_sunkLimit);
}

void FallApartStereoTest::testDiveStopsAtWhatItCovers()
{
    // the user lets effects go twice as deep as the background, and the pieces still stop on it
    setLimits(s_sunkLimit, s_poppedLimit, 2 * s_sunkLimit, s_effectPoppedLimit);
    configureEffect(QStringLiteral("Dive"), 0);
    QVERIFY(closeSubject());
    checkDive(QStringLiteral("dive-deep-limit"), -s_scale * s_sunkLimit);
}

void FallApartStereoTest::testDiveStopsAtTheEffectsOwnLimit()
{
    // the user keeps effects shallower than the background: that is the floor
    setLimits(s_sunkLimit, s_poppedLimit, 15, s_effectPoppedLimit);
    configureEffect(QStringLiteral("Dive"), 0);
    QVERIFY(closeSubject());
    checkDive(QStringLiteral("dive-shallow-limit"), -s_scale * 15);
}

void FallApartStereoTest::testTwoDimensionalOutputIgnoresDepth()
{
    QVERIFY(stereoOutput(false));
    for (const QString &mode : {QStringLiteral("Fly"), QStringLiteral("Dive")}) {
        configureEffect(mode, 25);
        // a fresh window for each mode
        if (!s_subject.window) {
            s_subject = createWindow(Qt::red, QPoint(600, 320), QSize(80, 80));
            QVERIFY(s_subject.window);
            workspace()->activateWindow(s_subject.window);
            QVERIFY(Test::waylandSync());
        }
        QVERIFY(closeSubject());
        s_subject = {};
        for (qreal p : {0.1, 0.3, 0.6}) {
            setLimits(s_sunkLimit, s_poppedLimit, s_effectSunkLimit, s_effectPoppedLimit);
            setProgress(p);
            const auto withDepth = capture();
            setLimits(0, 0, 0, 0);
            setProgress(p);
            const auto without = capture();
            QVERIFY(withDepth && without);
            QCOMPARE(withDepth->size(), s_outputSize);
            QVERIFY(measure(*withDepth, 0).mass > 0);
            QVERIFY2(*withDepth == *without, qPrintable(QStringLiteral("%1 at %2 differs").arg(mode).arg(p)));
        }
        QVERIFY(s_effect->isActive());
        setProgress(1);
        QTRY_VERIFY(!s_effect->isActive());
    }
}

}

WAYLANDTEST_MAIN(KWin::FallApartStereoTest)
#include "fallapart_stereo_test.moc"
