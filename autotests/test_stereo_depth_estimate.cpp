/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QRandomGenerator>
#include <QTest>

#include "core/stereodepthcoding.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "scene/stereodepthestimator.h"

using namespace KWin;

// The depth of 3D content on a picture and depth output, estimated from its two views: a textured
// picture whose other view is moved by a known parallax gives back that parallax in the depth
// picture's samples, the desktop's own samples are kept where it knows them, and a plain area,
// whose views match at every parallax, keeps the sample the desktop gives it (its window's level).
class StereoDepthEstimateTest : public QObject
{
    Q_OBJECT

    static constexpr QSize s_size{640, 96};

    static QImage noise()
    {
        QImage image(s_size, QImage::Format_RGBA8888_Premultiplied);
        QRandomGenerator random(151);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const int grey = random.bounded(256);
                image.setPixel(x, y, qRgb(grey, grey, grey));
            }
        }
        return image;
    }

    // the same picture with each point @a parallax pixels to the right
    static QImage moved(const QImage &image, int parallax)
    {
        QImage ret(image.size(), image.format());
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                ret.setPixel(x, y, image.pixel(std::clamp(x - parallax, 0, image.width() - 1), y));
            }
        }
        return ret;
    }

    struct Result
    {
        QImage depth;
        StereoDepthCoding coding;
    };

    // a plain grey picture with the noise inside @a patch
    static QImage patched(const QRect &patch)
    {
        const QImage source = noise();
        QImage image(s_size, QImage::Format_RGBA8888_Premultiplied);
        image.fill(qRgb(90, 90, 90));
        for (int y = patch.top(); y <= patch.bottom(); ++y) {
            for (int x = patch.left(); x <= patch.right(); ++x) {
                image.setPixel(x, y, source.pixel(x, y));
            }
        }
        return image;
    }

    // The desktop's picture: alpha one with @a knownSample inside @a known, elsewhere alpha zero (the
    // estimate asked for) with @a levelSample, the sample where the views don't tell. Written byte by
    // byte, since a colour with alpha zero is no premultiplied colour.
    std::optional<Result> run(const QImage &picture, const QImage &other, bool estimate, const QRect &known = QRect(), int knownSample = 0, int levelSample = 128)
    {
        const StereoDepthCoding coding(s_size, QSize(), 0);
        const auto pictureTexture = GLTexture::upload(picture);
        const auto otherTexture = GLTexture::upload(other);
        QImage desktop(s_size, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < s_size.height(); ++y) {
            uchar *line = desktop.scanLine(y);
            for (int x = 0; x < s_size.width(); ++x) {
                const bool isKnown = known.contains(x, y);
                const uchar sample = isKnown ? knownSample : levelSample;
                line[4 * x] = line[4 * x + 1] = line[4 * x + 2] = sample;
                line[4 * x + 3] = isKnown ? 255 : 0;
            }
        }
        const auto desktopTexture = GLTexture::upload(desktop);
        const auto target = GLTexture::allocate(GL_RGBA8, s_size);
        if (!pictureTexture || !otherTexture || !desktopTexture || !target) {
            return std::nullopt;
        }
        GLFramebuffer framebuffer(target.get());
        if (!framebuffer.valid()) {
            return std::nullopt;
        }
        StereoDepthEstimator estimator;
        const bool estimated = estimate && estimator.estimate(pictureTexture.get(), otherTexture.get(), QVector2D(1, 0), 16);
        if (estimate && !estimated) {
            return std::nullopt;
        }
        GLFramebuffer::pushFramebuffer(&framebuffer);
        const bool composed = estimator.compose(s_size, desktopTexture.get(), estimated, coding);
        GLFramebuffer::popFramebuffer();
        if (!composed) {
            return std::nullopt;
        }
        return Result{target->toImage(), coding};
    }

    // the parallax the depth samples stand for, over the inside of the picture
    static std::pair<double, double> measured(const Result &result, const QRect &area)
    {
        double low = 1e9;
        double high = -1e9;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const double parallax = result.coding.parallax(qRed(result.depth.pixel(x, y)));
                low = std::min(low, parallax);
                high = std::max(high, parallax);
            }
        }
        return {low, high};
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY), nullptr);
        QVERIFY(m_display);
        m_context = EglContext::create(m_display.get(), EGL_NO_CONFIG_KHR, nullptr);
        QVERIFY(m_context);
        QVERIFY(m_context->makeCurrent());
    }

    void parallax_data()
    {
        QTest::addColumn<int>("parallax");
        for (int parallax : {8, -8, 13, -3, 0}) {
            QTest::addRow("%d px", parallax) << parallax;
        }
    }

    void parallax()
    {
        QFETCH(int, parallax);
        const QImage picture = noise();
        const auto result = run(picture, moved(picture, parallax), true);
        QVERIFY(result);
        // inside the picture, away from its edges where the other view has nothing to match
        const QRect inside(32, 16, s_size.width() - 64, s_size.height() - 32);
        const double tolerance = 1 / result->coding.samplesPerPixel();
        const auto [low, high] = measured(*result, inside);
        QVERIFY2(low >= parallax - 0.5 - tolerance && high <= parallax + 0.5 + tolerance,
                 qPrintable(QStringLiteral("estimated %1 to %2 px for %3 px").arg(low).arg(high).arg(parallax)));
    }

    void swappedEyesFlipTheSign()
    {
        // the control: the views the other way round give the opposite parallax, so a test that
        // took the views in the wrong order would fail
        const QImage picture = noise();
        const QImage other = moved(picture, 8);
        const auto result = run(other, picture, true);
        QVERIFY(result);
        const QRect inside(32, 16, s_size.width() - 64, s_size.height() - 32);
        const auto [low, high] = measured(*result, inside);
        QVERIFY2(high <= -7, qPrintable(QStringLiteral("estimated %1 to %2 px for -8 px").arg(low).arg(high)));
    }

    void desktopSamplesAreKept()
    {
        const QImage picture = noise();
        const QRect known(100, 20, 60, 40);
        const auto result = run(picture, moved(picture, 8), true, known, 200);
        QVERIFY(result);
        for (int y = known.top(); y <= known.bottom(); ++y) {
            for (int x = known.left(); x <= known.right(); ++x) {
                QCOMPARE(qRed(result->depth.pixel(x, y)), 200);
            }
        }
        // and the estimate around it, to a sample
        QVERIFY(std::abs(qRed(result->depth.pixel(50, 48)) - int(result->coding.sample(8))) <= 1);
    }

    void withoutEstimateTheLevel()
    {
        // the control of the estimate: without it, 3D content shows at its window's level
        const QImage picture = noise();
        const auto result = run(picture, moved(picture, 8), false, QRect(), 0, 150);
        QVERIFY(result);
        const QImage &depth = result->depth;
        for (int y = 0; y < depth.height(); ++y) {
            for (int x = 0; x < depth.width(); ++x) {
                QCOMPARE(qRed(depth.pixel(x, y)), 150);
            }
        }
    }

    void plainAreaKeepsItsLevel()
    {
        // A window at a level of 4 pixels whose 3D content is plain but for a textured patch 8
        // pixels further: the patch gets its own parallax from the views, and the plain area, which
        // matches at every parallax, its window's level, not the screen.
        const QRect patch(200, 24, 240, 48);
        const QImage picture = patched(patch);
        const StereoDepthCoding coding(s_size, QSize(), 0);
        const int level = coding.sample(4);
        QVERIFY(level != 128);
        const auto result = run(picture, moved(picture, 12), true, QRect(), 0, level);
        QVERIFY(result);
        const double tolerance = 1 / result->coding.samplesPerPixel();
        const auto [low, high] = measured(*result, patch.adjusted(24, 12, -24, -12));
        QVERIFY2(low >= 12 - 0.5 - tolerance && high <= 12 + 0.5 + tolerance,
                 qPrintable(QStringLiteral("estimated %1 to %2 px in the patch for 12 px").arg(low).arg(high)));
        for (const QRect &plain : {QRect(16, 16, 120, 64), QRect(520, 16, 100, 64)}) {
            for (int y = plain.top(); y <= plain.bottom(); ++y) {
                for (int x = plain.left(); x <= plain.right(); ++x) {
                    QCOMPARE(qRed(result->depth.pixel(x, y)), level);
                }
            }
        }
    }

private:
    std::unique_ptr<EglDisplay> m_display;
    std::shared_ptr<EglContext> m_context;
};

QTEST_GUILESS_MAIN(StereoDepthEstimateTest)
#include "test_stereo_depth_estimate.moc"
