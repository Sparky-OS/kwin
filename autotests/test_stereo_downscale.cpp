/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QElapsedTimer>
#include <QTest>
#include <cmath>

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "scene/itemrenderer_opengl.h"
#include "scene/opengl/texture.h"
#include "scene/surfaceitem.h"

using namespace KWin;

class ImageSurface : public SurfaceItem
{
public:
    ImageSurface(ItemRendererOpenGL &renderer, const QImage &image, const QSize &destination, StereoContent layout)
    {
        setBufferSize(image.size());
        setBufferSourceBox(RectF(QPointF(), image.size()));
        setDestinationSize(destination);
        setStereoContent(layout);
        m_texture = renderer.createTexture(image);
    }
    RegionF shape() const override { return RegionF{rect()}; }
    void preprocess() override {}
};

class StereoDownscaleTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void downscale_data()
    {
        QTest::addColumn<double>("factor");
        QTest::addColumn<int>("layout");
        for (double factor : {1.0, 2.0, 3.5, 4.0, 8.0}) {
            for (int layout = 1; layout <= 8; ++layout) {
                QTest::addRow("%.1fx-layout-%d", factor, layout) << factor << layout;
            }
        }
    }

    void downscale()
    {
        QFETCH(double, factor);
        QFETCH(int, layout);
        const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY), nullptr);
        QVERIFY(display);
        const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, nullptr);
        QVERIFY(context);
        QVERIFY(context->makeCurrent());
        const QSize destination(32, 24);
        const QSize eyeSize = destination * factor;
        const auto content = StereoContent(layout);
        const bool sbs = isSideBySideStereoContent(content);
        QImage input(sbs ? QSize(eyeSize.width() * 2, eyeSize.height()) : QSize(eyeSize.width(), eyeSize.height() * 2), QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < input.height(); ++y) {
            for (int x = 0; x < input.width(); ++x) {
                const bool second = sbs ? x >= eyeSize.width() : y >= eyeSize.height();
                const int value = (((x % eyeSize.width()) % 4 == 0) != ((y % eyeSize.height()) % 4 == 0)) ? 255 : 0;
                input.setPixel(x, y, second ? qRgb(0, value, 0) : qRgb(value, 0, 0));
            }
        }
        ItemRendererOpenGL renderer(display.get());
        // the surface has no window on an output with eyes, take the view of each eye as a capture does
        renderer.setStereoCapture(true);
        ImageSurface surface(renderer, input, destination, content);
        QVERIFY(surface.texture());
        auto output = GLTexture::allocate(GL_RGBA8, destination);
        QVERIFY(output);
        GLFramebuffer framebuffer(output.get());
        QVERIFY(framebuffer.valid());
        RenderTarget target(&framebuffer);
        RenderViewport viewport(RectF(QPointF(), destination), 1, target, QPoint());
        WindowPaintData paint;
        for (auto eye : {StereoEye::Left, StereoEye::Right}) {
            renderer.setStereoEye(eye);
            context->pushFramebuffer(&framebuffer);
            renderer.renderItem(target, viewport, &surface, 0, Region::infinite(), paint, {}, {});
            context->popFramebuffer();
            const QImage actual = output->toImage().flipped(Qt::Vertical);
            QImage reference(destination, QImage::Format_RGBA8888_Premultiplied);
            const bool second = eye == StereoEye::Right;
            const QPoint offset = second ? (sbs ? QPoint(eyeSize.width(), 0) : QPoint(0, eyeSize.height())) : QPoint();
            for (int y = 0; y < destination.height(); ++y) {
                for (int x = 0; x < destination.width(); ++x) {
                    double sumR = 0, sumG = 0;
                    const double left = x * factor;
                    const double top = y * factor;
                    for (int sy = std::floor(top); sy < std::ceil(top + factor); ++sy) {
                        for (int sx = std::floor(left); sx < std::ceil(left + factor); ++sx) {
                            const double wx = std::min(double(sx + 1), left + factor) - std::max(double(sx), left);
                            const double wy = std::min(double(sy + 1), top + factor) - std::max(double(sy), top);
                            const QRgb pixel = input.pixel(offset.x() + sx, offset.y() + sy);
                            sumR += qRed(pixel) * wx * wy;
                            sumG += qGreen(pixel) * wx * wy;
                        }
                    }
                    reference.setPixel(x, y, qRgb(qRound(sumR / (factor * factor)), qRound(sumG / (factor * factor)), 0));
                    const QRgb pixel = actual.pixel(x, y);
                    QVERIFY2(std::abs(qRed(pixel) - qRound(double(sumR) / (factor * factor))) <= 1, "red differs from reference area average");
                    QVERIFY2(std::abs(qGreen(pixel) - qRound(double(sumG) / (factor * factor))) <= 1, "green differs from reference area average");
                    QCOMPARE(qBlue(pixel), 0);
                }
            }
            const QString artifacts = qEnvironmentVariable("STEREO_TEST_ARTIFACT_DIR");
            if (!artifacts.isEmpty() && layout == 3 && (factor == 4 || factor == 8)) {
                const QString stem = artifacts + QStringLiteral("/%1x-eye-%2").arg(factor).arg(int(eye));
                QVERIFY(input.save(stem + QStringLiteral("-input.png")));
                QVERIFY(actual.save(stem + QStringLiteral("-actual.png")));
                QVERIFY(reference.save(stem + QStringLiteral("-reference.png")));
            }
        }
        QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
    }

    void cost_data()
    {
        QTest::addColumn<int>("factor");
        QTest::addRow("4x") << 4;
        QTest::addRow("8x") << 8;
    }

    void cost()
    {
        QFETCH(int, factor);
        const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY), nullptr);
        QVERIFY(display);
        const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, nullptr);
        QVERIFY(context);
        QVERIFY(context->makeCurrent());
        const QSize destination(320, 180);
        QImage input(QSize(destination.width() * factor * 2, destination.height() * factor), QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < input.height(); ++y) {
            for (int x = 0; x < input.width(); ++x) {
                input.setPixel(x, y, ((x % 4 == 0) != (y % 4 == 0)) ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
            }
        }
        ItemRendererOpenGL renderer(display.get());
        ImageSurface surface(renderer, input, destination, StereoContentSideBySideFull);
        auto output = GLTexture::allocate(GL_RGBA8, destination);
        GLFramebuffer framebuffer(output.get());
        QVERIFY(framebuffer.valid());
        RenderTarget target(&framebuffer);
        RenderViewport viewport(RectF(QPointF(), destination), 1, target, QPoint());
        WindowPaintData paint;
        context->pushFramebuffer(&framebuffer);
        for (bool filtered : {false, true}) {
            surface.setStereoContent(filtered ? StereoContentSideBySideFull : StereoContentNone);
            // Keep the same source footprint for the mono baseline.
            surface.setBufferSourceBox(filtered ? RectF(QPointF(), input.size()) : RectF(0, 0, input.width() / 2, input.height()));
            auto draw = [&] {
                for (auto eye : {StereoEye::Left, StereoEye::Right}) {
                    renderer.setStereoEye(eye);
                    renderer.renderItem(target, viewport, &surface, 0, Region::infinite(), paint, {}, {});
                }
                glFinish();
            };
            draw();
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 20; ++i) {
                draw();
            }
            qInfo("%dx %s: %.3f ms/stereo frame, 320x180 per eye, 20 frames, renderer=%s", factor,
                  filtered ? "area" : "bilinear", timer.nsecsElapsed() / 20.0 / 1e6, glGetString(GL_RENDERER));
        }
        context->popFramebuffer();
        QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
    }
};
QTEST_MAIN(StereoDownscaleTest)
#include "test_stereo_downscale.moc"
