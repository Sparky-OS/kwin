/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "effect/effectloader.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/stereodepth.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/output.h>
#include <KWayland/Client/surface.h>

namespace KWin
{

/**
 * After a login KSplash shows its loading screen on every screen: one layer surface in the overlay layer per output, over the
 * whole of it, taking the keyboard as it maps, so only the last one mapped has the focus. On a stereo output each lies at the
 * screen and is awake, so its elements may show depth, whichever has the focus; nothing of the pile can lie over it. A surface of
 * the overlay layer that does not cover its output keeps the role it had, and a 2D output shows nothing in depth.
 */
class StereoDepthLoadingScreenTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testEveryScreenAtTheScreen();
    void testPartialOverlayKeepsItsRole();
    void testTwoDimensionalOutputSleeps();
};

static const QSize s_outputSize(320, 240);

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

static bool stereoOutputs(bool first, bool second)
{
    OutputConfiguration configuration;
    const auto outputs = workspace()->outputs();
    configuration.changeSet(outputs[0]->backendOutput())->currentMode = first ? stereoMode() : plainMode();
    configuration.changeSet(outputs[1]->backendOutput())->currentMode = second ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && outputs[0]->hasStereoEyes() == first && outputs[1]->hasStereoEyes() == second;
}

// the colour of column x, never black and never the same twice on a row of the output
static QImage columns(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < size.width(); ++x) {
        for (int y = 0; y < size.height(); ++y) {
            image.setPixel(x, y, qRgb(x & 0xff, 64 + (x >> 8), 128));
        }
    }
    return image;
}

struct Overlay
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::LayerSurfaceV1> shellSurface;
    Window *window = nullptr;
};

// A surface of the overlay layer on the i-th output, as ksplashqml makes its windows, taking the keyboard: the whole output
// (anchored to every edge), or a block of @a size in its corner
static Overlay createOverlay(int output, const QSize &size = QSize())
{
    Overlay result;
    result.surface = Test::createSurface();
    // the client's wl_output by index: looking it up by position gives the first one before the position events arrive
    result.shellSurface = Test::createLayerSurfaceV1(result.surface.get(), QStringLiteral("ksplashqml"), Test::waylandOutputs().value(output),
                                                     Test::LayerShellV1::layer_overlay);
    // a size of 0 on an axis anchored to both edges is the output's (an empty QSize is -1 x -1)
    result.shellSurface->set_size(size.isEmpty() ? 0 : size.width(), size.isEmpty() ? 0 : size.height());
    result.shellSurface->set_exclusive_zone(-1);
    result.shellSurface->set_keyboard_interactivity(1);
    if (size.isEmpty()) {
        result.shellSurface->set_anchor(Test::LayerSurfaceV1::anchor_bottom | Test::LayerSurfaceV1::anchor_top
                                        | Test::LayerSurfaceV1::anchor_left | Test::LayerSurfaceV1::anchor_right);
    } else {
        result.shellSurface->set_anchor(Test::LayerSurfaceV1::anchor_top | Test::LayerSurfaceV1::anchor_left);
    }
    QSignalSpy configured(result.shellSurface.get(), &Test::LayerSurfaceV1::configureRequested);
    result.surface->commit(KWayland::Client::Surface::CommitFlag::None);
    if (!configured.wait()) {
        return result;
    }
    result.shellSurface->ack_configure(configured.last().at(0).value<quint32>());
    result.window = Test::renderAndWaitForShown(result.surface.get(), columns(configured.last().at(1).toSize()));
    return result;
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
    auto image = manager.takeScreenShot(output, ScreenShotFlags(), std::nullopt);
    if (image) {
        image = image->convertToFormat(QImage::Format_RGB32);
    }
    return image;
}

// the eyes of a capture of a stereo output, side by side, are the same picture: what lies at the screen is not moved
static bool eyesAlike(const QImage &image)
{
    const int width = image.width() / 2;
    return image.copy(0, 0, width, image.height()) == image.copy(width, 0, width, image.height());
}

void StereoDepthLoadingScreenTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));
    // no effects and no motion between the levels: every capture is at the levels themselves
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", 0);
    config->sync();
    kwinApp()->setConfig(config);
    kwinApp()->start();
}

void StereoDepthLoadingScreenTest::init()
{
    Test::setOutputConfig({
        Test::OutputInfo{.geometry = Rect(QPoint(0, 0), s_outputSize), .modes = {plainMode(), stereoMode()}},
        Test::OutputInfo{.geometry = Rect(QPoint(s_outputSize.width(), 0), s_outputSize), .modes = {plainMode(), stereoMode()}},
    });
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::LayerShellV1));
    QTRY_COMPARE(Test::waylandOutputs().count(), 2);
}

void StereoDepthLoadingScreenTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void StereoDepthLoadingScreenTest::testEveryScreenAtTheScreen()
{
    QVERIFY(stereoOutputs(true, true));
    Overlay first = createOverlay(0);
    QVERIFY(first.window);
    Overlay second = createOverlay(1);
    QVERIFY(second.window);
    QCOMPARE(first.window->output(), workspace()->outputs()[0]);
    QCOMPARE(second.window->output(), workspace()->outputs()[1]);
    // the one mapped last has the focus, the other has not: both are at the screen and awake
    QCOMPARE(workspace()->activeWindow(), second.window);
    for (Window *window : {first.window, second.window}) {
        QCOMPARE(StereoDepth::level(window), 0.0);
        QVERIFY(StereoDepth::isAwake(window));
    }
    for (LogicalOutput *output : workspace()->outputs()) {
        const auto image = capture(output);
        QVERIFY(image);
        QCOMPARE(image->width(), 2 * s_outputSize.width());
        QVERIFY2(eyesAlike(*image), qPrintable(output->name()));
    }
}

void StereoDepthLoadingScreenTest::testPartialOverlayKeepsItsRole()
{
    // the control: a block of the overlay layer that does not cover its output, without the focus, is a window of the pile as
    // before, behind the screen and asleep
    QVERIFY(stereoOutputs(true, true));
    Overlay block = createOverlay(0, QSize(100, 100));
    QVERIFY(block.window);
    Overlay full = createOverlay(1);
    QVERIFY(full.window);
    QCOMPARE(workspace()->activeWindow(), full.window);
    QVERIFY(StereoDepth::level(block.window) < 0);
    QVERIFY(!StereoDepth::isAwake(block.window));
    QCOMPARE(StereoDepth::level(full.window), 0.0);
    QVERIFY(StereoDepth::isAwake(full.window));
}

void StereoDepthLoadingScreenTest::testTwoDimensionalOutputSleeps()
{
    // a 2D output beside a stereo one: the loading screen there is at the screen and asleep, the one on the stereo output awake
    QVERIFY(stereoOutputs(false, true));
    Overlay plain = createOverlay(0);
    QVERIFY(plain.window);
    Overlay stereo = createOverlay(1);
    QVERIFY(stereo.window);
    QCOMPARE(StereoDepth::level(plain.window), 0.0);
    QVERIFY(!StereoDepth::isAwake(plain.window));
    QVERIFY(StereoDepth::isAwake(stereo.window));
    const auto image = capture(workspace()->outputs()[0]);
    QVERIFY(image);
    QCOMPARE(image->width(), s_outputSize.width());
}

}

WAYLANDTEST_MAIN(KWin::StereoDepthLoadingScreenTest)
#include "stereo_depth_loading_screen_test.moc"
