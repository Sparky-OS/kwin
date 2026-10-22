/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QElapsedTimer>
#include <QTest>
#include <algorithm>
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
        QTest::addColumn<QSizeF>("factor");
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("address");
        for (double factor : {1.0, 1.5, 2.0, 3.5, 4.0, 8.0}) {
            QTest::addRow("%.1fx", factor) << QSizeF(factor, factor) << int(StereoContentSideBySideFull) << false;
        }
        // every sample carries its eye, column and row, wrapping at 256 like a ramp that jumps
        QTest::addRow("address 1.5x") << QSizeF(1.5, 1.5) << int(StereoContentSideBySideFull) << true;
        // a shrink in one axis is integrated while the other one is magnified
        QTest::addRow("address 1.5x by 0.75x") << QSizeF(1.5, 0.75) << int(StereoContentSideBySideFull) << true;
        // a magnified eye takes no sample of the other eye at its edge
        QTest::addRow("address 0.5x") << QSizeF(0.5, 0.5) << int(StereoContentSideBySideFull) << true;
    }

    void downscale()
    {
        QFETCH(QSizeF, factor);
        QFETCH(int, layout);
        QFETCH(bool, address);
        const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY), nullptr);
        QVERIFY(display);
        const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, nullptr);
        QVERIFY(context);
        QVERIFY(context->makeCurrent());
        const QSize destination = address ? QSize(256, 192) : QSize(32, 24);
        const QSize eyeSize(std::round(destination.width() * factor.width()), std::round(destination.height() * factor.height()));
        const auto content = StereoContent(layout);
        const bool sbs = isSideBySideStereoContent(content);
        QImage input(sbs ? QSize(eyeSize.width() * 2, eyeSize.height()) : QSize(eyeSize.width(), eyeSize.height() * 2), QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < input.height(); ++y) {
            for (int x = 0; x < input.width(); ++x) {
                const bool second = sbs ? x >= eyeSize.width() : y >= eyeSize.height();
                const int ex = x % eyeSize.width();
                const int ey = y % eyeSize.height();
                if (address) {
                    input.setPixel(x, y, qRgb(ex & 255, ey & 255, (second << 7) | ((ex >> 8) << 3) | (ey >> 8)));
                } else {
                    const int value = ((ex % 4 == 0) != (ey % 4 == 0)) ? 255 : 0;
                    input.setPixel(x, y, second ? qRgb(0, value, 0) : qRgb(value, 0, 0));
                }
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
            // the area each pixel covers in the eye, at least one sample wide (a magnified axis is
            // then bilinear), with the eye's own edge samples repeated beyond it
            const QSizeF window(std::max(factor.width(), 1.0), std::max(factor.height(), 1.0));
            for (int y = 0; y < destination.height(); ++y) {
                for (int x = 0; x < destination.width(); ++x) {
                    double sumR = 0, sumG = 0, sumB = 0;
                    const double left = (x + 0.5) * factor.width() - window.width() / 2;
                    const double top = (y + 0.5) * factor.height() - window.height() / 2;
                    for (int sy = std::floor(top); sy < std::ceil(top + window.height()); ++sy) {
                        for (int sx = std::floor(left); sx < std::ceil(left + window.width()); ++sx) {
                            const double wx = std::min(double(sx + 1), left + window.width()) - std::max(double(sx), left);
                            const double wy = std::min(double(sy + 1), top + window.height()) - std::max(double(sy), top);
                            const QRgb pixel = input.pixel(offset.x() + std::clamp(sx, 0, eyeSize.width() - 1),
                                                           offset.y() + std::clamp(sy, 0, eyeSize.height() - 1));
                            sumR += qRed(pixel) * wx * wy;
                            sumG += qGreen(pixel) * wx * wy;
                            sumB += qBlue(pixel) * wx * wy;
                        }
                    }
                    const double area = window.width() * window.height();
                    reference.setPixel(x, y, qRgb(qRound(sumR / area), qRound(sumG / area), qRound(sumB / area)));
                    const QRgb pixel = actual.pixel(x, y);
                    const QRgb expected = reference.pixel(x, y);
                    if (std::abs(qRed(pixel) - qRed(expected)) > 1 || std::abs(qGreen(pixel) - qGreen(expected)) > 1 || std::abs(qBlue(pixel) - qBlue(expected)) > 1) {
                        QFAIL(qPrintable(QStringLiteral("pixel %1,%2 is %3, the area average is %4").arg(x).arg(y).arg(pixel & 0xffffff, 6, 16, QLatin1Char('0')).arg(expected & 0xffffff, 6, 16, QLatin1Char('0'))));
                    }
                }
            }
            const QString artifacts = qEnvironmentVariable("STEREO_TEST_ARTIFACT_DIR");
            if (!artifacts.isEmpty() && layout == 3 && (address || factor.width() == 4 || factor.width() == 8)) {
                const QString stem = artifacts + QStringLiteral("/%1-eye-%2").arg(QLatin1StringView(QTest::currentDataTag())).arg(int(eye));
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
