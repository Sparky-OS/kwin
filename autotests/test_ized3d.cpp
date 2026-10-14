// SPDX-License-Identifier: GPL-2.0-or-later
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>
#include <vector>

using namespace KWin;

class Ized3dTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QFile file(QFINDTESTDATA("data/ized3d-formulas.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        m_data = QJsonDocument::fromJson(file.readAll(), &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(m_data["cases"].toArray().size(), 48);
    }

    void pixels_data()
    {
        QTest::addColumn<QJsonObject>("testCase");
        for (const auto &entry : m_data["cases"].toArray()) {
            const auto testCase = entry.toObject();
            QTest::newRow(qPrintable(testCase["name"].toString())) << testCase;
        }
    }

    void pixels()
    {
        QFETCH(QJsonObject, testCase);
        const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY), nullptr);
        QVERIFY(display);
        const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, nullptr);
        QVERIFY(context);
        QVERIFY(context->makeCurrent());
        const QSize size(m_data["width"].toInt(), m_data["height"].toInt());
        auto input = [&](const char *name) {
            QImage image(size, QImage::Format_RGBA8888_Premultiplied);
            const auto pixels = m_data[name].toArray();
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    const auto rgb = pixels[y * size.width() + x].toArray();
                    image.setPixel(x, y, qRgb(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt()));
                }
            }
            return GLTexture::upload(image);
        };
        const auto left = input("left");
        const auto right = input("right");
        QVERIFY(left);
        QVERIFY(right);
        auto shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(), QStringLiteral(":/opengl/ized3d.frag"));
        QVERIFY(shader);
        ShaderBinder binder(shader.get());
        QMatrix4x4 projection;
        projection.scale(1, -1);
        projection.ortho(QRectF(QPointF(), size));
        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
        shader->setUniform("leftEye", testCase["swap"].toBool() ? 1 : 0);
        shader->setUniform("rightEye", testCase["swap"].toBool() ? 0 : 1);
        shader->setUniform("panel", testCase["panel"].toInt());
        shader->setUniform("formula", testCase["formula"].toInt());
        shader->setUniform("transfer", testCase["transfer"].toInt());
        const auto target = GLTexture::allocate(GL_RGBA32F, size);
        QVERIFY(target);
        GLFramebuffer framebuffer(target.get());
        QVERIFY(framebuffer.valid());
        context->pushFramebuffer(&framebuffer);
        glActiveTexture(GL_TEXTURE1);
        right->bind();
        glActiveTexture(GL_TEXTURE0);
        left->render(size);
        std::vector<float> actual(size.width() * size.height() * 4);
        glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, actual.data());
        QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
        context->popFramebuffer();
        const auto expected = testCase["expected"].toArray();
        QCOMPARE(expected.size(), size.width() * size.height());
        for (qsizetype pixel = 0; pixel < expected.size(); ++pixel) {
            const auto rgb = expected[pixel].toArray();
            for (int channel = 0; channel < 3; ++channel) {
                const double value = actual[pixel * 4 + channel];
                const double prediction = rgb[channel].toDouble();
                QVERIFY2(std::abs(value - prediction) < 0.000002,
                         qPrintable(QStringLiteral("pixel %1 channel %2: got %3, expected %4")
                                        .arg(pixel).arg(channel).arg(value, 0, 'g', 10).arg(prediction, 0, 'g', 10)));
            }
            QCOMPARE(actual[pixel * 4 + 3], 1.0f);
        }
    }

private:
    QJsonObject m_data;
};

QTEST_MAIN(Ized3dTest)
#include "test_ized3d.moc"
