/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QTest>
#include "core/colorspace.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshadermanager.h"
#include "opengl/glshader.h"
#include "opengl/gltexture.h"

using namespace KWin;

class StereoPatternTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void pixels_data()
    {
        QTest::addColumn<int>("pattern");
        QTest::addColumn<int>("rightFirst");
        QTest::addColumn<int>("position");
        for (int pattern = 0; pattern < 3; ++pattern) {
            for (int rightFirst = 0; rightFirst < 2; ++rightFirst) {
                for (int position : {2, 3}) {
                    QTest::addRow("pattern-%d-order-%d-position-%d", pattern, rightFirst, position) << pattern << rightFirst << position;
                }
            }
        }
    }

    void pixels()
    {
        QFETCH(int, pattern);
        QFETCH(int, rightFirst);
        QFETCH(int, position);
        const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY), nullptr);
        QVERIFY(display);
        const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, nullptr);
        QVERIFY(context);
        QVERIFY(context->makeCurrent());
        const QSize size(20, 18);
        const QRect window(position, position, 12, 10);
        QImage left(size, QImage::Format_RGBA8888_Premultiplied);
        QImage right(size, QImage::Format_RGBA8888_Premultiplied);
        left.fill(Qt::black);
        right.fill(Qt::black);
        for (int y = window.top(); y <= window.bottom(); ++y) {
            for (int x = window.left(); x <= window.right(); ++x) {
                left.setPixel(x, y, qRgb(255, 0, 0));
                right.setPixel(x, y, qRgb(0, 255, 0));
            }
        }
        auto shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(), QStringLiteral(":/opengl/stereopattern.frag"));
        QVERIFY(shader);
        ShaderBinder binder(shader.get());
        QMatrix4x4 projection;
        projection.ortho(QRectF(QPointF(), size));
        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
        shader->setColorspaceUniforms(ColorDescription::sRGB, ColorDescription::sRGB, RenderingIntent::AbsoluteColorimetricNoAdaptation);
        shader->setUniform("leftEye", 0);
        shader->setUniform("rightEye", 1);
        shader->setUniform("pattern", pattern);
        shader->setUniform("rightFirst", rightFirst);
        shader->setUniform("outputHeight", size.height());
        const auto target = GLTexture::allocate(GL_RGBA8, size);
        QVERIFY(target);
        GLFramebuffer framebuffer(target.get());
        QVERIFY(framebuffer.valid());
        const auto leftTexture = GLTexture::upload(left);
        const auto rightTexture = GLTexture::upload(right);
        QVERIFY(leftTexture);
        QVERIFY(rightTexture);
        context->pushFramebuffer(&framebuffer);
        glActiveTexture(GL_TEXTURE1);
        rightTexture->bind();
        glActiveTexture(GL_TEXTURE0);
        leftTexture->render(size);
        context->popFramebuffer();
        const QImage actual = target->toImage().flipped(Qt::Vertical);
        QCOMPARE(actual.size(), size);
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const bool selectRight = ((pattern == 0 ? y : pattern == 1 ? x : x + y) % 2) != rightFirst;
                const QRgb expected = !window.contains(x, y) ? qRgb(0, 0, 0) : selectRight ? qRgb(0, 255, 0) : qRgb(255, 0, 0);
                QCOMPARE(actual.pixel(x, y), expected);
            }
        }
    }
};
QTEST_MAIN(StereoPatternTest)
#include "test_stereo_pattern.moc"
