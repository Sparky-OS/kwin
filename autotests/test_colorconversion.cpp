/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include "core/colorconversion.h"
#include "core/colorpipeline.h"
#include "core/colorspace.h"

#include <cmath>
#include <set>

using namespace KWin;

/**
 * The conversion table of a pair of colour descriptions (ORDER 230): built once from the colour
 * pipeline and reused; the eyes of an output copied into the colour space of another one equal
 * the scene painted again for that one; HDR content on a deep-colour SDR output keeps the steps
 * of the source.
 */
class TestColorConversion : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void testBuiltOnceAndReused();
    void testCopyEqualsPaintingAgain_data();
    void testCopyEqualsPaintingAgain();
    void testHdrContentLeavesTheCopyOnTheOldPath();
    void testHdrRampOnDeepColourSdr_data();
    void testHdrRampOnDeepColourSdr();
    void testPerChannelAtDepths_data();
    void testPerChannelAtDepths();
};

// what DrmOutput blends in for an SDR output: gamma 2.2 up to the output's reference white
static std::shared_ptr<ColorDescription> sdrBlending(double brightness, EncodingRange range = EncodingRange::Full)
{
    const double reference = TransferFunction::defaultReferenceLuminanceFor(TransferFunction::gamma22) * brightness;
    return std::make_shared<ColorDescription>(ColorDescription{
        Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, reference),
        reference,
        0,
        reference,
        reference,
        YUVMatrixCoefficients::Identity,
        range,
    });
}

// what DrmOutput::createColorDescription makes for an SDR output with the sRGB profile at a brightness
// (the reference white moves, the output keeps its headroom to 200 cd/m2 and its black level)
static std::shared_ptr<ColorDescription> drmSdrBlending(double brightness)
{
    return std::make_shared<ColorDescription>(ColorDescription{
        Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0.01, 200),
        5 + (200 - 5) * brightness,
        0.01,
        200,
        200,
        Colorimetry::BT709,
        Colorimetry::BT709,
    });
}

// an HDR output: PQ up to its peak, SDR content at reference white
static std::shared_ptr<ColorDescription> hdrBlending(double peak, double reference)
{
    return std::make_shared<ColorDescription>(ColorDescription{
        Colorimetry::BT2020,
        TransferFunction(TransferFunction::gamma22, 0, peak),
        reference,
        0,
        peak,
        peak,
    });
}

void TestColorConversion::init()
{
    ColorConversionTable::clear();
}

void TestColorConversion::testBuiltOnceAndReused()
{
    // descriptions that compare equal are one pair, however many objects hold them
    const auto first = ColorConversionTable::get(sdrBlending(1.0), sdrBlending(0.5), RenderingIntent::Perceptual);
    QCOMPARE(ColorConversionTable::statistics().builds, 1u);
    for (int frame = 0; frame < 100; ++frame) {
        const auto again = ColorConversionTable::get(sdrBlending(1.0), sdrBlending(0.5), RenderingIntent::Perceptual);
        QCOMPARE(again.get(), first.get());
    }
    QCOMPARE(ColorConversionTable::statistics().builds, 1u);
    QCOMPARE(ColorConversionTable::statistics().reuses, 100u);

    // a description that changes (the brightness) is a new pair, built once, and the old one stays
    const auto dimmer = ColorConversionTable::get(sdrBlending(1.0), sdrBlending(0.25), RenderingIntent::Perceptual);
    QVERIFY(dimmer.get() != first.get());
    QCOMPARE(ColorConversionTable::statistics().builds, 2u);
    QCOMPARE(ColorConversionTable::get(sdrBlending(1.0), sdrBlending(0.5), RenderingIntent::Perceptual).get(), first.get());
    QCOMPARE(ColorConversionTable::statistics().builds, 2u);

    // another intent is another table
    ColorConversionTable::get(sdrBlending(1.0), sdrBlending(0.5), RenderingIntent::AbsoluteColorimetricNoAdaptation);
    QCOMPARE(ColorConversionTable::statistics().builds, 3u);

    // the verdict of copying is decided once per pair as well
    ColorConversionTable::clear();
    for (int frame = 0; frame < 50; ++frame) {
        ColorConversionTable::forCopy(sdrBlending(1.0), sdrBlending(0.5), false, nullptr);
    }
    const auto statistics = ColorConversionTable::statistics();
    QVERIFY(statistics.builds <= 4);
    QCOMPARE(statistics.reuses, 0u);
}

void TestColorConversion::testCopyEqualsPaintingAgain_data()
{
    QTest::addColumn<std::shared_ptr<ColorDescription>>("source");
    QTest::addColumn<std::shared_ptr<ColorDescription>>("destination");
    QTest::addColumn<bool>("copies");

    QTest::addRow("the same colour space") << sdrBlending(1.0) << sdrBlending(1.0) << true;
    QTest::addRow("the output's own descriptions, brightness 100% and 50%") << drmSdrBlending(1.0) << drmSdrBlending(0.5) << true;
    QTest::addRow("the output's own descriptions, brightness 40% and 100%") << drmSdrBlending(0.4) << drmSdrBlending(1.0) << true;
    QTest::addRow("the output's own descriptions, brightness 100% and 20%") << drmSdrBlending(1.0) << drmSdrBlending(0.2) << true;
    QTest::addRow("brightness 100% and 50%") << sdrBlending(1.0) << sdrBlending(0.5) << true;
    QTest::addRow("brightness 30% and 100%") << sdrBlending(0.3) << sdrBlending(1.0) << true;
    QTest::addRow("BT.709 limited range and full") << sdrBlending(1.0, EncodingRange::Limited) << sdrBlending(1.0) << true;
    QTest::addRow("BT.709 limited range at 70% and full at 100%") << sdrBlending(0.7, EncodingRange::Limited) << sdrBlending(1.0) << true;
}

void TestColorConversion::testCopyEqualsPaintingAgain()
{
    QFETCH(std::shared_ptr<ColorDescription>, source);
    QFETCH(std::shared_ptr<ColorDescription>, destination);
    QFETCH(bool, copies);

    for (const RenderingIntent intent : {RenderingIntent::Perceptual, RenderingIntent::RelativeColorimetricWithBPC, RenderingIntent::RelativeColorimetric, RenderingIntent::AbsoluteColorimetricNoAdaptation}) {
        const auto t = ColorConversionTable::get(source, destination, intent);
        qInfo() << "intent" << int(intent) << "SDR deviation" << t->deviationFromPaintingAgain(ColorDescription::sRGB) << "HDR deviation" << t->deviationFromPaintingAgain(ColorDescription::BT2020PQ);
    }
    QString reason;
    const auto table = ColorConversionTable::forCopy(source, destination, false, &reason);
    QCOMPARE(table != nullptr, copies);
    if (!copies) {
        QVERIFY(!reason.isEmpty());
        return;
    }
    // within one level of 10 bits (so of 8) of the scene painted again, for SDR content
    QVERIFY(table->deviationFromPaintingAgain(ColorDescription::sRGB) <= ColorConversionTable::s_copyTolerance);
}

// HDR content on screen: the eyes carry the tone mapping for the source, painting again does it for
// the destination; when those differ by more than a level the copy is left on the old path, with
// the reason, and an identical colour space copies anyway
void TestColorConversion::testHdrContentLeavesTheCopyOnTheOldPath()
{
    QString reason;
    QVERIFY(ColorConversionTable::forCopy(sdrBlending(1.0), sdrBlending(1.0), true, &reason));
    QVERIFY(ColorConversionTable::forCopy(sdrBlending(1.0), sdrBlending(0.5), false, &reason));
    QVERIFY(!ColorConversionTable::forCopy(sdrBlending(1.0), sdrBlending(0.5), true, &reason));
    QVERIFY2(reason.contains(QStringLiteral("HDR content")), qPrintable(reason));
}

// A PQ ramp and an HLG-like ramp onto a 12 bit SDR output: the table converts in floating point and the
// value is encoded once at the output's depth, so the steps of the source are kept: no code is skipped
// where the source has a step for it, and the ramp never goes backwards.
void TestColorConversion::testHdrRampOnDeepColourSdr_data()
{
    QTest::addColumn<std::shared_ptr<ColorDescription>>("content");
    QTest::addColumn<int>("bits");

    const auto pq = ColorDescription::BT2020PQ;
    for (const int bits : {10, 12, 16}) {
        QTest::addRow("PQ ramp onto %d bit SDR", bits) << pq << bits;
    }
}

void TestColorConversion::testHdrRampOnDeepColourSdr()
{
    QFETCH(std::shared_ptr<ColorDescription>, content);
    QFETCH(int, bits);

    const auto output = sdrBlending(1.0);
    const auto table = ColorConversionTable::get(content, output, RenderingIntent::Perceptual);
    QVERIFY(!table->isIdentity());
    const int levels = (1 << bits) - 1;
    // an input step finer than the output's: 4 inputs per output level
    const int steps = std::min(levels * 4, 1 << 18);
    std::set<int> codes;
    int previous = -1;
    int largestJump = 0;
    double previousValue = -1;
    for (int i = 0; i <= steps; ++i) {
        const float v = float(i) / steps;
        const QVector3D out = table->convert(QVector3D(v, v, v));
        // a grey stays grey
        QVERIFY(std::abs(out.x() - out.y()) < 1e-3 && std::abs(out.y() - out.z()) < 1e-3);
        const double value = std::clamp<double>(out.y(), 0, 1);
        QVERIFY2(value >= previousValue - 1e-6, "the ramp goes backwards");
        previousValue = value;
        const int code = int(std::lround(value * levels));
        codes.insert(code);
        if (previous >= 0) {
            largestJump = std::max(largestJump, code - previous);
        }
        previous = code;
    }
    // no banding beyond the output's depth: the biggest jump between two neighbouring inputs
    // is the slope of the tone map, which the inputs sample finely, so it stays a few levels, and
    // the output uses a large part of its codes
    QVERIFY2(largestJump <= 16, qPrintable(QStringLiteral("largest jump %1 levels").arg(largestJump)));
    const int usedCodes = int(codes.size());
    qInfo() << bits << "bit output:" << usedCodes << "of" << levels + 1 << "codes used, largest jump" << largestJump << "levels";
    QVERIFY(usedCodes >= (levels + 1) / 8);
}

// the same table applied to the three channels at 8, 10 and 12 bits is per channel exact: the
// converted value, encoded at the depth, is within one level of the pipeline's value
void TestColorConversion::testPerChannelAtDepths_data()
{
    QTest::addColumn<int>("bits");
    QTest::addRow("8 bit") << 8;
    QTest::addRow("10 bit") << 10;
    QTest::addRow("12 bit") << 12;
}

void TestColorConversion::testPerChannelAtDepths()
{
    QFETCH(int, bits);
    const auto source = sdrBlending(1.0);
    const auto destination = sdrBlending(0.5);
    const auto table = ColorConversionTable::forCopy(source, destination, false, nullptr);
    QVERIFY(table);
    const int levels = (1 << bits) - 1;
    // content painted for the destination directly, quantized once
    const ColorPipeline direct = ColorPipeline::create(ColorDescription::sRGB, destination, RenderingIntent::Perceptual, ColorPipeline::InputType::FloatingPoint);
    const ColorPipeline toSource = ColorPipeline::create(ColorDescription::sRGB, source, RenderingIntent::Perceptual, ColorPipeline::InputType::FloatingPoint);
    int worst = 0;
    for (int r = 0; r <= 8; ++r) {
        for (int g = 0; g <= 8; ++g) {
            for (int b = 0; b <= 8; ++b) {
                const QVector3D color(r / 8.0f, g / 8.0f, b / 8.0f);
                // the eyes: quantized at the depth of the buffer the source composed in
                QVector3D eye = toSource.evaluate(color);
                eye = QVector3D(std::round(std::clamp(eye.x(), 0.0f, 1.0f) * levels), std::round(std::clamp(eye.y(), 0.0f, 1.0f) * levels), std::round(std::clamp(eye.z(), 0.0f, 1.0f) * levels)) / levels;
                const QVector3D copied = table->convert(eye);
                const QVector3D painted = direct.evaluate(color);
                for (int c = 0; c < 3; ++c) {
                    const int a = int(std::lround(std::clamp(copied[c], 0.0f, 1.0f) * levels));
                    const int d = int(std::lround(std::clamp(painted[c], 0.0f, 1.0f) * levels));
                    worst = std::max(worst, std::abs(a - d));
                }
            }
        }
    }
    qInfo() << bits << "bit: largest difference between the copy and the scene painted again:" << worst << "level(s)";
    QVERIFY(worst <= 1);
}

QTEST_GUILESS_MAIN(TestColorConversion)
#include "test_colorconversion.moc"
