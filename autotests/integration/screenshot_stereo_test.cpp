/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/itemrenderer.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

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

static void checkStereoResult(const QImage &image, const QColor &left, const QColor &right)
{
    QCOMPARE(image.size(), QSize(200, 50));
    QCOMPARE(image.pixelColor(20, 20), left);
    QCOMPARE(image.pixelColor(120, 20), right);
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

struct TestWindow {
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

}

WAYLANDTEST_MAIN(KWin::ScreenshotStereoTest)
#include "screenshot_stereo_test.moc"
