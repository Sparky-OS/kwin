/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QTest>

#include "core/stereodepthcoding.h"

using namespace KWin;

// The depth coding of picture and depth outputs and its 3D metadata (HDMI 1.4a, Appendix H,
// Table H-4). The formula is our reading of ISO/IEC 23002-3, sections 6.1.2.2 and 6.2.2.2:
// UNVERIFIED against the standard's text.
class StereoDepthCodingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void metadata()
    {
        // a 46 inch 16:9 television, 1018 x 573 mm, at its automatic viewing distance
        const StereoDepthCoding coding(QSize(1920, 1080), QSize(1018, 573), 0);
        QCOMPARE(coding.wref(), 102);
        QCOMPARE(coding.dref(), 172); // three picture heights
        // 64 pixels of 1920 on 102 cm are 3.4 cm, in 127 samples either way
        QCOMPARE(coding.parallaxScale(), 37);
        QCOMPARE(coding.parallaxZero(), 128);
        const std::array<uint8_t, 8> expected{0x00, 0x80, 0x00, 37, 0x00, 172, 0x00, 102};
        QCOMPARE(coding.metadata(), expected);
    }

    void unknownSize()
    {
        // no size in the EDID (projectors): the CSS reference pixel, 96 per inch
        const StereoDepthCoding coding(QSize(1920, 1080), QSize(), 0);
        QCOMPARE(coding.wref(), 51); // 50.8 cm
        QCOMPARE(coding.dref(), 86); // 3 x 28.575 cm
        QCOMPARE(coding.parallaxScale(), 74);
    }

    void viewingDistance()
    {
        const StereoDepthCoding coding(QSize(1280, 720), QSize(1018, 573), 250);
        QCOMPARE(coding.dref(), 250);
        QCOMPARE(coding.metadata()[4], uint8_t(0));
        QCOMPARE(coding.metadata()[5], uint8_t(250));
    }

    void samples_data()
    {
        QTest::addColumn<QSize>("eye");
        QTest::addColumn<QSize>("physical");
        QTest::newRow("1080p 46 inch") << QSize(1920, 1080) << QSize(1018, 573);
        QTest::newRow("720p projector") << QSize(1280, 720) << QSize();
        QTest::newRow("2160p 65 inch") << QSize(3840, 2160) << QSize(1439, 809);
    }

    void samples()
    {
        QFETCH(QSize, eye);
        QFETCH(QSize, physical);
        const StereoDepthCoding coding(eye, physical, 0);
        // no parallax is the zero sample; behind the screen (the right eye's copy to the right) is above it
        QCOMPARE(coding.sample(0), uint8_t(128));
        QVERIFY(coding.sample(4) > 128);
        QVERIFY(coding.sample(-4) < 128);
        // the range either way fits, and samples beyond it are clamped
        const double range = StereoDepthCoding::s_range1920 * eye.width() / 1920;
        QVERIFY(coding.sample(range) <= 255 && coding.sample(range) > 250);
        QVERIFY(coding.sample(-range) >= 1 && coding.sample(-range) < 6);
        QCOMPARE(coding.sample(10 * range), uint8_t(255));
        QCOMPARE(coding.sample(-10 * range), uint8_t(0));
        // a sample stands for its parallax to half a sample
        for (double parallax = -range; parallax <= range; parallax += 0.25) {
            QVERIFY(std::abs(coding.parallax(coding.sample(parallax)) - parallax) <= 0.5 / coding.samplesPerPixel() + 1e-9);
        }
        // and what the metadata says decodes to the same: p = (m - parallax_zero) / parallax_scale cm on wref cm
        const auto bytes = coding.metadata();
        const double zero = bytes[0] << 8 | bytes[1];
        const double scale = bytes[2] << 8 | bytes[3];
        const double wref = bytes[6] << 8 | bytes[7];
        for (int sample : {0, 37, 128, 200, 255}) {
            const double centimetres = (sample - zero) / scale;
            QCOMPARE(centimetres * eye.width() / wref, coding.parallax(sample));
        }
    }
};

QTEST_GUILESS_MAIN(StereoDepthCodingTest)
#include "test_stereo_depth_coding.moc"
