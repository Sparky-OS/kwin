/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "backends/virtual/virtual_backend.h"
#include "backends/virtual/virtual_egl_backend.h"
#include "compositor.h"
#include "core/graphicsbuffer.h"
#include "cursor.h"
#include "effect/effecthandler.h"
#include "effect/quickeffect.h"
#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/stereodepth.h"
#include "scene/surfaceitem.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/surface.h>

#include <QDir>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>

#include <map>

namespace KWin
{

class StereoTestLayer : public VirtualEglLayer
{
public:
    StereoTestLayer(BackendOutput *output, VirtualEglBackend *backend)
        : VirtualEglLayer(output, backend)
        , m_backend(backend)
    {
    }

    ~StereoTestLayer() override
    {
        m_backend->openglContext()->makeCurrent();
        m_rightFramebuffer.reset();
        m_rightTexture.reset();
    }

    bool hasStereoEyes() const override
    {
        return m_output->hasStereoEyes();
    }

    std::optional<OutputLayerBeginFrameInfo> beginFrame(OutputFrame *frame) override
    {
        m_rightPainted = false;
        return VirtualEglLayer::beginFrame(frame);
    }

    std::optional<OutputLayerBeginFrameInfo> beginRightEyeFrame() override
    {
        if (!m_rightTexture || m_rightTexture->size() != m_output->modeSize()) {
            m_rightFramebuffer.reset();
            m_rightTexture = GLTexture::allocate(GL_RGBA8, m_output->modeSize());
            if (!m_rightTexture) {
                return std::nullopt;
            }
            m_rightTexture->setContentTransform(OutputTransform::FlipY);
            m_rightFramebuffer = std::make_unique<GLFramebuffer>(m_rightTexture.get());
        }
        if (!m_rightFramebuffer->valid()) {
            return std::nullopt;
        }
        m_rightPainted = true;
        rightFrames++;
        return OutputLayerBeginFrameInfo{
            .renderTarget = RenderTarget(m_rightFramebuffer.get()),
            .repaint = Region::infinite(),
        };
    }

    bool endFrame(const Region &rendered, const Region &damaged, OutputFrame *frame) override
    {
        leftImage = texture()->toImage().convertToFormat(QImage::Format_RGB32);
        rightImage = m_rightPainted ? m_rightTexture->toImage().convertToFormat(QImage::Format_RGB32) : leftImage;
        return VirtualEglLayer::endFrame(rendered, damaged, frame);
    }

    QImage leftImage;
    QImage rightImage;
    int rightFrames = 0;

private:
    VirtualEglBackend *const m_backend;
    std::unique_ptr<GLTexture> m_rightTexture;
    std::unique_ptr<GLFramebuffer> m_rightFramebuffer;
    bool m_rightPainted = false;
};

class StereoTestEglBackend : public VirtualEglBackend
{
public:
    using VirtualEglBackend::VirtualEglBackend;

    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override
    {
        auto &layer = m_layers[output];
        if (!layer) {
            layer = std::make_unique<StereoTestLayer>(output, this);
            connect(output, &QObject::destroyed, this, [this, output]() {
                m_layers.erase(output);
            });
        }
        return {layer.get()};
    }

private:
    std::map<BackendOutput *, std::unique_ptr<StereoTestLayer>> m_layers;
};

class StereoTestBackend : public VirtualBackend
{
public:
    std::unique_ptr<EglBackend> createOpenGLBackend(RenderDevice *renderDevice) override
    {
        return std::make_unique<StereoTestEglBackend>(this, renderDevice);
    }
};

class QuickStereoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void testRendering_data();
    void testRendering();
    void testGeometryAndLimits();
    void testScanout();
    void testPointer();
};

static const QSize s_outputSize(320, 240);

static QByteArray sceneQml()
{
    QByteArray qml = R"(
        import QtQuick
        import org.kde.kwin 3.0
        Rectangle {
            id: root
            color: "red"
            property bool viewStereo: SceneView.stereo
            property real popped: SceneView.stereoPopped
            property real sunk: SceneView.stereoSunk
            Rectangle {
                x: parent.width / 2
                width: parent.width / 2
                height: parent.height
                color: "blue"
                visible: root.viewStereo
            }
            Rectangle { x: 20; y: 20; width: 16; height: 16; color: "white" }
            Rectangle {
                x: parent.width / 2 + 20
                y: 20
                width: 16
                height: 16
                color: "white"
                visible: root.viewStereo
            }
        }
    )";
    if (qEnvironmentVariableIsSet("STEREO_TEST_SWAP_EYES")) {
        qml.replace("\"red\"", "\"swapped\"");
        qml.replace("\"blue\"", "\"red\"");
        qml.replace("\"swapped\"", "\"blue\"");
    }
    return qml;
}

static QImage expectedEye(const QColor &color)
{
    QImage image(s_outputSize, QImage::Format_RGB32);
    image.fill(color);
    for (int y = 20; y < 36; y++) {
        for (int x = 20; x < 36; x++) {
            image.setPixelColor(x, y, Qt::white);
        }
    }
    return image;
}

static void saveEvidence(const QString &name, const QImage &image)
{
    const QString path = qEnvironmentVariable("STEREO_TEST_ARTIFACT_DIR");
    if (!path.isEmpty()) {
        QVERIFY(QDir().mkpath(path));
        QVERIFY(image.save(path + u'/' + name + QStringLiteral(".png")));
    }
}

void QuickStereoTest::initTestCase()
{
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
    Cursors::self()->hideCursor();
}

void QuickStereoTest::init()
{
    options->setStereoPoppedLimit(0);
    options->setStereoSunkLimit(0);
    QVERIFY(!StereoDepth::isEnabled());
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {OutputModeline(s_outputSize, 120000, OutputModeline::Flag::Stereo3DSequentialLeftFirst)},
    }});
    QVERIFY(Test::setupWaylandConnection());
}

void QuickStereoTest::cleanup()
{
    Test::destroyWaylandConnection();
    QCOMPARE(effects->activeFullScreenEffect(), nullptr);
}

void QuickStereoTest::testRendering_data()
{
    QTest::addColumn<bool>("optIn");
    QTest::addColumn<bool>("stereoOutput");
    QTest::newRow("mono") << false << false;
    QTest::newRow("opt-in-mono-output") << true << false;
    QTest::newRow("ordinary-effect-stereo-output") << false << true;
    QTest::newRow("stereo") << true << true;
}

void QuickStereoTest::testRendering()
{
    QFETCH(bool, optIn);
    QFETCH(bool, stereoOutput);
    if (!stereoOutput) {
        Test::setOutputConfig({Rect(QPoint(), s_outputSize)});
    }
    LogicalOutput *output = workspace()->outputs().front();
    QCOMPARE(output->hasStereoEyes(), stereoOutput);
    auto backend = static_cast<StereoTestEglBackend *>(Compositor::self()->backend());
    auto layer = static_cast<StereoTestLayer *>(backend->compatibleOutputLayers(output->backendOutput()).front());
    const int rightFrames = layer->rightFrames;
    QQmlComponent component(effects->qmlEngine());
    component.setData(sceneQml(), QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QuickSceneEffect effect;
    QVERIFY(!effect.isStereo());
    effect.setStereo(optIn);
    effect.setDelegate(&component);
    effect.setRunning(true);
    QVERIFY(effect.isRunning());
    QuickSceneView *view = effect.viewForScreen(output);
    QVERIFY(view);
    const bool stereo = optIn && stereoOutput;
    QCOMPARE(view->isStereo(), stereo);
    QCOMPARE(view->geometry(), output->geometry());
    QCOMPARE(view->size(), s_outputSize);
    QCOMPARE(view->window()->size(), QSize(stereo ? 640 : 320, 240));
    QCOMPARE(view->rootItem()->property("viewStereo").toBool(), stereo);
    QCOMPARE(effect.viewAt(QPoint(319, 239)), view);
    QCOMPARE(effect.viewAt(QPoint(320, 239)), nullptr);
    view->update(nullptr);
    const QImage left = expectedEye(Qt::red);
    const QImage right = expectedEye(stereo ? Qt::blue : Qt::red);
    if (qEnvironmentVariableIsSet("STEREO_TEST_SWAP_EYES")) {
        QTRY_COMPARE_WITH_TIMEOUT(layer->leftImage, right, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(layer->rightImage, left, 30000);
        saveEvidence(QStringLiteral("swapped-live-left"), layer->leftImage);
        saveEvidence(QStringLiteral("swapped-live-right"), layer->rightImage);
        ScreenShotManager manager;
        const auto image = manager.takeScreenShot(output, {}, std::nullopt);
        QVERIFY(image);
        const QImage capture = image->convertToFormat(QImage::Format_RGB32);
        QCOMPARE(capture.size(), QSize(640, 240));
        QCOMPARE(capture.copy(0, 0, 320, 240), right);
        QCOMPARE(capture.copy(320, 0, 320, 240), left);
        saveEvidence(QStringLiteral("swapped-capture"), capture);
        effect.setRunning(false);
        QVERIFY2(layer->leftImage == left, "The live left eye must match the normal red-left image");
    }
    QTRY_COMPARE_WITH_TIMEOUT(layer->leftImage, left, 30000);
    QTRY_COMPARE_WITH_TIMEOUT(layer->rightImage, right, 30000);
    QCOMPARE(layer->rightFrames > rightFrames, stereo);
    saveEvidence(QStringLiteral("%1-live-left").arg(QString::fromLatin1(QTest::currentDataTag())), layer->leftImage);
    saveEvidence(QStringLiteral("%1-live-right").arg(QString::fromLatin1(QTest::currentDataTag())), layer->rightImage);
    ScreenShotManager manager;
    const auto image = manager.takeScreenShot(output, {}, std::nullopt);
    QVERIFY(image);
    const QImage capture = image->convertToFormat(QImage::Format_RGB32);
    QCOMPARE(capture.size(), QSize(stereoOutput ? 640 : 320, 240));
    QCOMPARE(capture.copy(0, 0, 320, 240), left);
    if (stereoOutput) {
        QCOMPARE(capture.copy(320, 0, 320, 240), right);
    }
    saveEvidence(QStringLiteral("%1-capture").arg(QString::fromLatin1(QTest::currentDataTag())), capture);
    effect.setRunning(false);
}

void QuickStereoTest::testGeometryAndLimits()
{
    QuickSceneEffect effect;
    effect.setStereo(true);
    QuickSceneView view(&effect, workspace()->outputs().front());
    QSignalSpy changed(&view, &QuickSceneView::stereoLimitsChanged);
    options->setStereoPoppedLimit(4);
    options->setStereoSunkLimit(10);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(view.stereoPopped(), 4.0 / 6);
    QCOMPARE(view.stereoSunk(), 10.0 / 6);
    view.setGeometry(Rect(40, 20, 96, 64));
    QCOMPARE(view.geometry(), Rect(40, 20, 96, 64));
    QCOMPARE(view.size(), QSize(96, 64));
    QCOMPARE(view.window()->geometry(), QRect(40, 20, 192, 64));
    QCOMPARE(view.contentItem()->size(), QSizeF(192, 64));
    QCOMPARE(view.stereoPopped(), 0.2);
    QCOMPARE(view.stereoSunk(), 0.5);
    QCOMPARE(changed.count(), 3);
    view.setStereo(false);
    QCOMPARE(view.geometry(), Rect(40, 20, 96, 64));
    QCOMPARE(view.window()->geometry(), QRect(40, 20, 96, 64));
}

void QuickStereoTest::testScanout()
{
    LogicalOutput *output = workspace()->outputs().front();
    QuickSceneEffect effect;
    QuickSceneView view(&effect, output);
    QQmlComponent component(effects->qmlEngine());
    component.setData("import QtQuick; Rectangle { color: \"red\" }", QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    view.setRootItem(qobject_cast<QQuickItem *>(component.create()));
    view.update(nullptr);
    SurfaceItem *surfaceItem = nullptr;
    for (Item *item : kwinApp()->scene()->overlayItem()->childItems()) {
        if (auto surface = qobject_cast<SurfaceItem *>(item)) {
            surfaceItem = surface;
        }
    }
    QVERIFY(surfaceItem);
    QVERIFY(surfaceItem->buffer());
    QVERIFY(surfaceItem->buffer()->dmabufAttributes());
    QCOMPARE(surfaceItem->stereoContent(), StereoContentNone);
    SceneView sceneView(kwinApp()->scene(), output, nullptr, nullptr, Compositor::self()->primaryDevice());
    sceneView.setViewport(output->geometryF());
    sceneView.setScale(1);
    sceneView.prePaint();
    QVERIFY(kwinApp()->scene()->layerCandidates(1).contains(surfaceItem));
    sceneView.postPaint();
    view.setStereo(true);
    view.update(nullptr);
    QCOMPARE(surfaceItem->stereoContent(), StereoContentSideBySideFull);
    QCOMPARE(surfaceItem->buffer()->size(), QSize(640, 240));
    sceneView.prePaint();
    QVERIFY(!kwinApp()->scene()->layerCandidates(1).contains(surfaceItem));
    QVERIFY(kwinApp()->scene()->layerCandidates(1).contains(kwinApp()->scene()->containerItem()));
    sceneView.postPaint();
}

void QuickStereoTest::testPointer()
{
    options->setStereoSunkLimit(12);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> toplevel(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    window->move(QPoint(20, 40));
    std::unique_ptr<KWayland::Client::Surface> activeSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> activeToplevel(Test::createXdgToplevelSurface(activeSurface.get()));
    Window *activeWindow = Test::renderAndWaitForShown(activeSurface.get(), QSize(100, 50), Qt::red);
    QVERIFY(activeWindow);
    activeWindow->move(QPoint(180, 40));
    QCOMPARE(workspace()->activeWindow(), activeWindow);
    const QPointF pos(50, 60);
    QCOMPARE(StereoDepth::pointerParallax(pos, 320), -2);
    QuickSceneEffect effect;
    effects->setActiveFullScreenEffect(&effect);
    QCOMPARE(StereoDepth::pointerParallax(pos, 320), 0);
    effects->setActiveFullScreenEffect(nullptr);
    QCOMPARE(StereoDepth::pointerParallax(pos, 320), -2);
    activeSurface.reset();
    QVERIFY(Test::waitForWindowClosed(activeWindow));
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

} // namespace KWin

WAYLANDTEST_MAIN_OPT(KWin::QuickStereoTest, false, [] {
    return std::make_unique<KWin::StereoTestBackend>();
})
#include "quick_stereo_test.moc"
