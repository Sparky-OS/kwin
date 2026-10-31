/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "compositor.h"
#include "core/output.h"
#include "core/outputconfiguration.h"
#include "effect/effectloader.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "plugins/screenshot/screenshotlayer.h"
#include "scene/itemrenderer.h"
#include "scene/scene.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KWayland/Client/compositor.h>
#include <KWayland/Client/region.h>
#include <KWayland/Client/surface.h>

#include <QDir>

namespace KWin
{

class ScreenshotStereoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testDeclaredWindowOnTwoDimensionalOutput();
    void testDeclaredWindowOnFrameSequentialOutput();
    void testAnaglyphOutputKeepsRawEyes();
    void testStereoPairOutputsKeepRawEyes();
    void testThreeDimensionalOutputWithoutDeclaredWindow();
    void testUncoveredPixelsAreTransparentInBothEyes();
    void testTranslucentPixelsUnderOpaqueRegion_data();
    void testTranslucentPixelsUnderOpaqueRegion();
    void testEyePassOverEarlierPicture_data();
    void testEyePassOverEarlierPicture();
    void testOrdinaryCapture();
    void testWorkspaceStereoOutputOnLeft();
    void testWorkspaceStereoOutputOnRight();
    void testWorkspaceWithoutStereoIsUnchanged();
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

static OutputModeline anaglyphMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DAnaglyphModern);
}

static OutputModeline frameSequentialMode()
{
    return OutputModeline(s_outputSize, 120000, OutputModeline::Flag::Stereo3DSequentialLeftFirst);
}

static QImage stereoSurface()
{
    QImage image(QSize(100, 50), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = image.width() / 2; x < image.width(); ++x) {
            image.setPixelColor(x, y, Qt::blue);
        }
    }
    return image;
}

// the declared surface is one eye wide, 50x50: the window capture holds both eyes side by side
static void checkStereoResult(const QImage &image, const QColor &left, const QColor &right)
{
    QCOMPARE(image.size(), QSize(100, 50));
    QCOMPARE(image.pixelColor(20, 20), left);
    QCOMPARE(image.pixelColor(70, 20), right);
}

static void saveEvidence(const QString &name, const QImage &image)
{
    QVERIFY(QDir::current().mkpath(QStringLiteral("evidence")));
    QVERIFY2(image.save(QDir::current().filePath(QStringLiteral("evidence/%1.png").arg(name))), qPrintable(name));
}

void ScreenshotStereoTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));

    // disable all effects: an open animation fades a window in and a close animation
    // keeps it on the screen, and the captures must hold exactly what the windows drew
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();
}

void ScreenshotStereoTest::init()
{
    OutputConfiguration clearPair;
    for (LogicalOutput *output : workspace()->outputs()) {
        const auto change = clearPair.changeSet(output->backendOutput());
        change->stereoPartner = QString();
        change->stereoPairMode = StereoPairMode::None;
        change->stereoPairRole = StereoPairRole::Left;
        change->stereoPairReflection = StereoPairReflection::None;
    }
    QVERIFY(workspace()->applyOutputConfiguration(clearPair) == OutputConfigurationError::None);
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode(), anaglyphMode(), frameSequentialMode()},
    }});
    OutputConfiguration resetMode;
    for (LogicalOutput *output : workspace()->outputs()) {
        resetMode.changeSet(output->backendOutput())->currentMode = plainMode();
    }
    QVERIFY(workspace()->applyOutputConfiguration(resetMode) == OutputConfigurationError::None);
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::StereoContentV1));
}

void ScreenshotStereoTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void ScreenshotStereoTest::testDeclaredWindowOnTwoDimensionalOutput()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), stereoSurface());
    QVERIFY(window);
    window->move(QPoint(100, 80));
    QVERIFY(Test::waylandSync());

    ScreenShotManager manager;
    const auto windowImage = manager.takeScreenShot(window);
    QVERIFY(windowImage);
    saveEvidence(QStringLiteral("declared-window"), *windowImage);
    checkStereoResult(*windowImage, Qt::red, Qt::blue);
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);

    const auto screenImage = manager.takeScreenShot(workspace()->outputs().front(), {}, std::nullopt);
    QVERIFY(screenImage);
    saveEvidence(QStringLiteral("declared-screen"), *screenImage);
    QCOMPARE(screenImage->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
    QCOMPARE(screenImage->pixelColor(110, 90), Qt::red);
    QCOMPARE(screenImage->pixelColor(s_outputSize.width() + 110, 90), Qt::blue);
    QCOMPARE(screenImage->pixelColor(10, 10), screenImage->pixelColor(s_outputSize.width() + 10, 10));
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);
}

void ScreenshotStereoTest::testThreeDimensionalOutputWithoutDeclaredWindow()
{
    const auto output = workspace()->outputs().front();
    OutputConfiguration configuration;
    configuration.changeSet(output->backendOutput())->currentMode = stereoMode();
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(output->hasStereoEyes());

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(40, 40), Qt::green);
    QVERIFY(window);

    ScreenShotManager manager;
    const auto image = manager.takeScreenShot(output, {}, std::nullopt);
    QVERIFY(image);
    saveEvidence(QStringLiteral("stereo-output"), *image);
    QCOMPARE(image->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
    QCOMPARE(image->pixelColor(10, 10), image->pixelColor(s_outputSize.width() + 10, 10));
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);
}

// What no window covers is transparent black in both eyes of a stereo capture, as in the frame the
// eyes are rendered into, whatever the memory the joined picture is made in held before.
void ScreenshotStereoTest::testUncoveredPixelsAreTransparentInBothEyes()
{
    const auto output = workspace()->outputs().front();
    OutputConfiguration configuration;
    configuration.changeSet(output->backendOutput())->currentMode = stereoMode();
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(output->hasStereoEyes());

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(40, 40), Qt::green);
    QVERIFY(window);
    window->move(QPoint(100, 80));
    QVERIFY(Test::waylandSync());

    ScreenShotManager manager;
    for (int round = 0; round < 3; ++round) {
        // memory of the joined picture's size, freed with white in it, as an earlier picture leaves it
        {
            QImage used(QSize(s_outputSize.width() * 2, s_outputSize.height()), QImage::Format_RGBA8888_Premultiplied);
            used.fill(Qt::white);
            QImage usedArgb(QSize(s_outputSize.width() * 2, s_outputSize.height()), QImage::Format_ARGB32_Premultiplied);
            usedArgb.fill(Qt::white);
        }
        const auto image = manager.takeScreenShot(output, {}, std::nullopt);
        QVERIFY(image);
        QCOMPARE(image->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
        for (int eye = 0; eye < 2; ++eye) {
            for (int y = 0; y < s_outputSize.height(); ++y) {
                for (int x = 0; x < s_outputSize.width(); ++x) {
                    const QRgb pixel = image->pixel(eye * s_outputSize.width() + x, y);
                    const bool covered = x >= 100 && x < 140 && y >= 80 && y < 120;
                    QVERIFY2(covered ? pixel == qRgb(0, 255, 0) : pixel == qRgba(0, 0, 0, 0),
                             qPrintable(QStringLiteral("round %1, eye %2, %3,%4: %5").arg(round).arg(eye).arg(x).arg(y).arg(pixel, 8, 16, QLatin1Char('0'))));
                }
            }
        }
    }
}

void ScreenshotStereoTest::testDeclaredWindowOnFrameSequentialOutput()
{
    const auto output = workspace()->outputs().front();
    OutputConfiguration configuration;
    configuration.changeSet(output->backendOutput())->currentMode = frameSequentialMode();
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(output->hasStereoEyes());

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), stereoSurface());
    QVERIFY(window);
    QVERIFY(Test::waylandSync());

    ScreenShotManager manager;
    const auto image = manager.takeScreenShot(window);
    QVERIFY(image);
    saveEvidence(QStringLiteral("declared-window-frame-sequential"), *image);
    checkStereoResult(*image, Qt::red, Qt::blue);
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);
}

void ScreenshotStereoTest::testAnaglyphOutputKeepsRawEyes()
{
    const auto output = workspace()->outputs().front();
    OutputConfiguration configuration;
    configuration.changeSet(output->backendOutput())->currentMode = anaglyphMode();
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(output->hasStereoEyes());

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), stereoSurface());
    QVERIFY(window);
    window->move(QPoint(100, 80));
    QVERIFY(Test::waylandSync());

    ScreenShotManager manager;
    const auto image = manager.takeScreenShot(output, {}, std::nullopt);
    QVERIFY(image);
    saveEvidence(QStringLiteral("anaglyph-output"), *image);
    QCOMPARE(image->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
    QCOMPARE(image->pixelColor(110, 90), QColor(Qt::red));
    QCOMPARE(image->pixelColor(s_outputSize.width() + 110, 90), QColor(Qt::blue));
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);
}

void ScreenshotStereoTest::testStereoPairOutputsKeepRawEyes()
{
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(QPoint(), s_outputSize),
            .modes = {plainMode()},
        },
        Test::OutputInfo{
            .geometry = Rect(QPoint(s_outputSize.width(), 0), s_outputSize),
            .modes = {plainMode()},
        },
    });

    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 2);
    OutputConfiguration configuration;
    const auto setPair = [&](LogicalOutput *output, const QString &uuid, const QString &partner, StereoPairRole role) {
        const auto change = configuration.changeSet(output->backendOutput());
        change->uuid = uuid;
        change->stereoPartner = partner;
        change->stereoPairMode = StereoPairMode::DualProjection;
        change->stereoPairRole = role;
    };
    setPair(outputs.front(), QStringLiteral("stereo-left"), QStringLiteral("stereo-right"), StereoPairRole::Left);
    setPair(outputs.back(), QStringLiteral("stereo-right"), QStringLiteral("stereo-left"), StereoPairRole::Right);
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(outputs.front()->hasStereoEyes());
    QVERIFY(outputs.back()->hasStereoEyes());

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), stereoSurface());
    QVERIFY(window);
    window->move(QPoint(100, 80));
    std::unique_ptr<KWayland::Client::Surface> secondSurface(Test::createSurface());
    auto secondDeclaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*secondSurface));
    secondDeclaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    std::unique_ptr<Test::XdgToplevel> secondShellSurface(Test::createXdgToplevelSurface(secondSurface.get()));
    Window *secondWindow = Test::renderAndWaitForShown(secondSurface.get(), stereoSurface());
    QVERIFY(secondWindow);
    secondWindow->move(QPoint(s_outputSize.width() + 100, 80));
    QVERIFY(Test::waylandSync());

    ScreenShotManager manager;
    for (int i = 0; i < outputs.size(); ++i) {
        const auto image = manager.takeScreenShot(outputs[i], {}, std::nullopt);
        QVERIFY(image);
        saveEvidence(QStringLiteral("stereo-pair-%1").arg(i), *image);
        QCOMPARE(image->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
        QCOMPARE(image->pixelColor(110, 90), QColor(Qt::red));
        QCOMPARE(image->pixelColor(s_outputSize.width() + 110, 90), QColor(Qt::blue));
    }
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);

    OutputConfiguration clearPair;
    for (LogicalOutput *output : outputs) {
        const auto change = clearPair.changeSet(output->backendOutput());
        change->stereoPartner = QString();
        change->stereoPairMode = StereoPairMode::None;
        change->stereoPairRole = StereoPairRole::Left;
        change->stereoPairReflection = StereoPairReflection::None;
    }
    QCOMPARE(workspace()->applyOutputConfiguration(clearPair), OutputConfigurationError::None);
}

void ScreenshotStereoTest::testOrdinaryCapture()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(40, 40), Qt::green);
    QVERIFY(window);

    ScreenShotManager manager;
    const auto image = manager.takeScreenShot(workspace()->outputs().front(), {}, std::nullopt);
    QVERIFY(image);
    saveEvidence(QStringLiteral("ordinary-screen"), *image);
    QCOMPARE(image->size(), s_outputSize);
    QCOMPARE(kwinApp()->scene()->renderer()->stereoEye(), StereoEye::None);
}

static void configureTwoOutputs(bool leftStereo)
{
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(QPoint(), s_outputSize),
            .modes = {plainMode(), stereoMode()},
        },
        Test::OutputInfo{
            .geometry = Rect(QPoint(s_outputSize.width(), 0), s_outputSize),
            .modes = {plainMode(), stereoMode()},
        },
    });

    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 2);
    LogicalOutput *stereoOutput = leftStereo ? outputs.front() : outputs.back();
    OutputConfiguration configuration;
    configuration.changeSet(stereoOutput->backendOutput())->currentMode = stereoMode();
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(stereoOutput->hasStereoEyes());
}

struct TestWindow
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shellSurface;
    Window *window;
};

static TestWindow createWindow(const QColor &color, const QPoint &position)
{
    TestWindow result;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createXdgToplevelSurface(result.surface.get());
    result.window = Test::renderAndWaitForShown(result.surface.get(), QSize(40, 40), color);
    if (result.window) {
        result.window->move(position);
    }
    return result;
}

void ScreenshotStereoTest::testWorkspaceStereoOutputOnLeft()
{
    configureTwoOutputs(true);

    std::unique_ptr<KWayland::Client::Surface> stereoSurfaceClient(Test::createSurface());
    auto declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*stereoSurfaceClient));
    declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    std::unique_ptr<Test::XdgToplevel> stereoShell(Test::createXdgToplevelSurface(stereoSurfaceClient.get()));
    Window *stereoWindow = Test::renderAndWaitForShown(stereoSurfaceClient.get(), stereoSurface());
    QVERIFY(stereoWindow);
    stereoWindow->move(QPoint(100, 80));
    QVERIFY(Test::waylandSync());

    const auto flat = createWindow(Qt::green, QPoint(s_outputSize.width() + 100, 80));
    QVERIFY(flat.window);

    ScreenShotManager manager;
    const auto image = manager.takeScreenShotWorkspace({}, std::nullopt);
    QVERIFY(image);
    saveEvidence(QStringLiteral("workspace-stereo-left"), *image);
    QCOMPARE(image->size(), QSize(s_outputSize.width() * 3, s_outputSize.height()));
    QCOMPARE(image->pixelColor(110, 90), QColor(Qt::red));
    QCOMPARE(image->pixelColor(s_outputSize.width() + 110, 90), QColor(Qt::blue));
    QCOMPARE(image->pixelColor(s_outputSize.width() * 2 + 110, 90), QColor(Qt::green));
}

void ScreenshotStereoTest::testWorkspaceStereoOutputOnRight()
{
    configureTwoOutputs(false);

    const auto left = createWindow(Qt::green, QPoint(100, 80));
    QVERIFY(left.window);
    const auto right = createWindow(Qt::green, QPoint(s_outputSize.width() + 100, 80));
    QVERIFY(right.window);

    ScreenShotManager manager;
    const auto image = manager.takeScreenShotWorkspace({}, std::nullopt);
    QVERIFY(image);
    saveEvidence(QStringLiteral("workspace-stereo-right"), *image);
    QCOMPARE(image->size(), QSize(s_outputSize.width() * 3, s_outputSize.height()));
    QCOMPARE(image->pixelColor(110, 90), QColor(Qt::green));
    QCOMPARE(image->pixelColor(s_outputSize.width() + 110, 90), QColor(Qt::green));
    QCOMPARE(image->pixelColor(s_outputSize.width() * 2 + 110, 90), QColor(Qt::green));
}

void ScreenshotStereoTest::testWorkspaceWithoutStereoIsUnchanged()
{
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(QPoint(), s_outputSize),
            .modes = {plainMode()},
        },
        Test::OutputInfo{
            .geometry = Rect(QPoint(s_outputSize.width(), 0), s_outputSize),
            .modes = {plainMode()},
        },
    });
    QVERIFY(Test::waylandSync());

    const auto left = createWindow(Qt::red, QPoint(100, 80));
    QVERIFY(left.window);
    const auto right = createWindow(Qt::blue, QPoint(s_outputSize.width() + 100, 80));
    QVERIFY(right.window);

    ScreenShotManager manager;
    const Rect area(QPoint(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
    const auto expected = manager.takeScreenShot(area, {}, std::nullopt);
    const auto image = manager.takeScreenShotWorkspace({}, std::nullopt);
    QVERIFY(expected);
    QVERIFY(image);
    saveEvidence(QStringLiteral("workspace-ordinary"), *image);
    QCOMPARE(image->size(), expected->size());
    QCOMPARE(image->pixelColor(110, 90), expected->pixelColor(110, 90));
    QCOMPARE(image->pixelColor(s_outputSize.width() + 110, 90), expected->pixelColor(s_outputSize.width() + 110, 90));
    QCOMPARE(*image, *expected);
}

// Windows that say they are opaque where their pixels are not ("marking transparent content as opaque
// will result in repaint artifacts", wl_surface.set_opaque_region): the left half of one is at alpha
// one half, (0.8, 0.2, 0.2, 0.5) as a client's GL clear writes it, over a window its opaque region
// hides; beside it a surface without an alpha channel, drawn without blending. Desktop depth is off, so
// that the 2D windows are one picture in both eyes.
struct OpaqueRegionWindows
{
    OpaqueRegionWindows()
        : sunkLimit(options->stereoSunkLimit())
        , poppedLimit(options->stereoPoppedLimit())
        , effectSunkLimit(options->stereoEffectSunkLimit())
        , effectPoppedLimit(options->stereoEffectPoppedLimit())
    {
        options->setStereoSunkLimit(0);
        options->setStereoPoppedLimit(0);
        options->setStereoEffectSunkLimit(0);
        options->setStereoEffectPoppedLimit(0);
    }

    ~OpaqueRegionWindows()
    {
        options->setStereoSunkLimit(sunkLimit);
        options->setStereoPoppedLimit(poppedLimit);
        options->setStereoEffectSunkLimit(effectSunkLimit);
        options->setStereoEffectPoppedLimit(effectPoppedLimit);
    }

    bool create()
    {
        below = createWindow(Qt::green, QPoint(90, 70));
        if (!below.window) {
            return false;
        }

        QImage image(QSize(40, 40), QImage::Format_ARGB32_Premultiplied);
        image.fill(0xffcc3333);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width() / 2; ++x) {
                image.setPixel(x, y, 0x80cc3333);
            }
        }
        translucent.surface = Test::createSurface();
        translucent.shellSurface = Test::createXdgToplevelSurface(translucent.surface.get());
        const auto opaqueRegion = Test::waylandCompositor()->createRegion(QRegion(QRect(QPoint(), image.size())));
        translucent.surface->setOpaqueRegion(opaqueRegion.get());
        translucent.window = Test::renderAndWaitForShown(translucent.surface.get(), image);
        if (!translucent.window) {
            return false;
        }
        translucent.window->move(QPoint(100, 80));

        QImage solidImage(QSize(40, 40), QImage::Format_RGB32);
        solidImage.fill(0xff2266aa);
        solid.surface = Test::createSurface();
        solid.shellSurface = Test::createXdgToplevelSurface(solid.surface.get());
        solid.window = Test::renderAndWaitForShown(solid.surface.get(), solidImage);
        if (!solid.window) {
            return false;
        }
        solid.window->move(QPoint(130, 100));
        return Test::waylandSync();
    }

    // the 2D picture: the translucent half over transparent black, not over the window below it
    static void checkFlat(const QImage &flat)
    {
        QCOMPARE(flat.size(), s_outputSize);
        QCOMPARE(flat.pixel(105, 85), 0x80cc3333);
        QCOMPARE(flat.pixel(125, 85), 0xffcc3333);
    }

    const qreal sunkLimit;
    const qreal poppedLimit;
    const qreal effectSunkLimit;
    const qreal effectPoppedLimit;
    TestWindow below;
    TestWindow translucent;
    TestWindow solid;
};

static int countDiffering(const QImage &flat, const QImage &eye, int eyeOffset, const Rect &skip, const char *name)
{
    int differing = 0;
    for (int y = 0; y < s_outputSize.height(); ++y) {
        for (int x = 0; x < s_outputSize.width(); ++x) {
            if (skip.contains(QPoint(x, y))) {
                continue;
            }
            const QRgb expected = flat.pixel(x, y);
            const QRgb actual = eye.pixel(eyeOffset + x, y);
            if (actual != expected && ++differing <= 4) {
                qWarning("%s %d,%d: 2D %08x, eye %08x", name, x, y, expected, actual);
            }
        }
    }
    return differing;
}

// Both eyes of a stereo capture are drawn into one target, the right eye after the left: the right eye
// is the same picture as the left and as a 2D capture, never the window over the left eye's picture,
// and what lies below the opaque region is still not drawn.
void ScreenshotStereoTest::testTranslucentPixelsUnderOpaqueRegion_data()
{
    QTest::addColumn<bool>("stereoOutput");

    QTest::newRow("declared window on a 2D output") << false;
    QTest::newRow("3D output") << true;
}

void ScreenshotStereoTest::testTranslucentPixelsUnderOpaqueRegion()
{
    QFETCH(bool, stereoOutput);

    OpaqueRegionWindows windows;
    QVERIFY(windows.create());

    const auto output = workspace()->outputs().front();
    ScreenShotManager manager;
    const auto flat = manager.takeScreenShot(output, {}, std::nullopt);
    QVERIFY(flat);
    saveEvidence(QStringLiteral("opaque-region-2d"), *flat);
    OpaqueRegionWindows::checkFlat(*flat);
    if (QTest::currentTestFailed()) {
        return;
    }

    Rect stereoPlace;
    std::unique_ptr<KWayland::Client::Surface> stereoClient;
    std::unique_ptr<Test::StereoContentV1> declaration;
    std::unique_ptr<Test::XdgToplevel> stereoShell;
    if (stereoOutput) {
        OutputConfiguration configuration;
        configuration.changeSet(output->backendOutput())->currentMode = stereoMode();
        workspace()->applyOutputConfiguration(configuration);
        QVERIFY(Test::waylandSync());
        QVERIFY(output->hasStereoEyes());
    } else {
        stereoClient = Test::createSurface();
        declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*stereoClient));
        declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
        stereoShell = Test::createXdgToplevelSurface(stereoClient.get());
        Window *stereoWindow = Test::renderAndWaitForShown(stereoClient.get(), stereoSurface());
        QVERIFY(stereoWindow);
        stereoWindow->move(QPoint(240, 160));
        QVERIFY(Test::waylandSync());
        stereoPlace = stereoWindow->frameGeometry().toRect();
    }

    for (int round = 0; round < 3; ++round) {
        const auto image = manager.takeScreenShot(output, {}, std::nullopt);
        QVERIFY(image);
        if (round == 0) {
            saveEvidence(stereoOutput ? QStringLiteral("opaque-region-3d-output") : QStringLiteral("opaque-region-declared"), *image);
        }
        QCOMPARE(image->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
        QCOMPARE(countDiffering(*flat, *image, 0, stereoPlace, "left"), 0);
        QCOMPARE(countDiffering(*flat, *image, s_outputSize.width(), stereoPlace, "right"), 0);
    }
}

// Each eye of a 3D output is drawn into buffers of its own, which hold that eye's earlier frames: an eye
// pass into a target that holds another picture is the same picture as a 2D capture.
void ScreenshotStereoTest::testEyePassOverEarlierPicture_data()
{
    QTest::addColumn<bool>("rightEye");

    QTest::newRow("left") << false;
    QTest::newRow("right") << true;
}

void ScreenshotStereoTest::testEyePassOverEarlierPicture()
{
    QFETCH(bool, rightEye);
    const StereoEye eye = rightEye ? StereoEye::Right : StereoEye::Left;

    OpaqueRegionWindows windows;
    QVERIFY(windows.create());

    const auto output = workspace()->outputs().front();
    ScreenShotManager manager;
    const auto flat = manager.takeScreenShot(output, {}, std::nullopt);
    QVERIFY(flat);
    OpaqueRegionWindows::checkFlat(*flat);
    if (QTest::currentTestFailed()) {
        return;
    }

    OutputConfiguration configuration;
    configuration.changeSet(output->backendOutput())->currentMode = stereoMode();
    workspace()->applyOutputConfiguration(configuration);
    QVERIFY(Test::waylandSync());
    QVERIFY(output->hasStereoEyes());

    const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
    QVERIFY(eglBackend);
    EglContext *context = eglBackend->openglContext();
    QVERIFY(context && context->makeCurrent());
    const auto texture = GLTexture::allocate(GL_RGBA8, s_outputSize);
    QVERIFY(texture);
    GLFramebuffer target(texture.get());
    QVERIFY(target.valid());
    // the earlier picture
    GLFramebuffer::pushFramebuffer(&target);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLFramebuffer::popFramebuffer();

    ScreenshotLayer layer(output, &target);
    QVERIFY(layer.preparePresentationTest());
    const auto beginInfo = layer.beginFrame();
    QVERIFY(beginInfo);
    SceneView view(kwinApp()->scene(), output, nullptr, &layer);
    ItemTreeView cursorView(&view, kwinApp()->scene()->cursorItem(), output, nullptr, nullptr);
    cursorView.setExclusive(true);
    const Rect whole(QPoint(), s_outputSize);
    view.setViewport(output->geometryF());
    view.setScale(1);
    view.prePaint();
    ItemRenderer *renderer = kwinApp()->scene()->renderer();
    renderer->setStereoEye(eye);
    view.paint(beginInfo->renderTarget, QPoint(), whole);
    renderer->setStereoEye(StereoEye::None);
    QImage picture(s_outputSize, QImage::Format_RGBA8888_Premultiplied);
    GLFramebuffer::pushFramebuffer(&target);
    glReadPixels(0, 0, picture.width(), picture.height(), GL_RGBA, GL_UNSIGNED_BYTE, picture.bits());
    GLFramebuffer::popFramebuffer();
    view.postPaint();
    QVERIFY(layer.endFrame(whole, whole, nullptr));
    picture = picture.flipped(Qt::Vertical);
    saveEvidence(rightEye ? QStringLiteral("opaque-region-right-over-white") : QStringLiteral("opaque-region-left-over-white"), picture);

    QCOMPARE(countDiffering(*flat, picture, 0, Rect(), rightEye ? "right" : "left"), 0);
}

}

WAYLANDTEST_MAIN(KWin::ScreenshotStereoTest)
#include "screenshot_stereo_test.moc"
