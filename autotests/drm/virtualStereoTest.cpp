/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "drm_connector.h"
#include "drm_stereo.h"

#include <QTest>
#include <QFile>
#include <cstring>

using namespace KWin;

class ModeWithTestBlob : public DrmConnectorMode
{
public:
    explicit ModeWithTestBlob(const drmModeModeInfo &timing)
        : DrmConnectorMode(nullptr, timing, {})
        , testBlob(std::make_shared<DrmBlob>(nullptr, 0))
    {
    }
    std::shared_ptr<DrmBlob> blob() override { return testBlob; }
    const std::shared_ptr<DrmBlob> testBlob;
};

class VirtualStereoTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void modes_data()
    {
        QTest::addColumn<bool>("anaglyph");
        QTest::addColumn<bool>("other");
        QTest::addColumn<int>("count");
        QTest::newRow("off") << false << false << 4;
        QTest::newRow("anaglyph") << true << false << 10;
        QTest::newRow("other") << false << true << 16;
        QTest::newRow("both") << true << true << 22;
    }

    void modes()
    {
        QFETCH(bool, anaglyph);
        QFETCH(bool, other);
        QFETCH(int, count);
        drmModeModeInfo timing{};
        timing.clock = 148500;
        timing.hdisplay = 1920;
        timing.htotal = 2200;
        timing.vdisplay = 1080;
        timing.vtotal = 1125;
        timing.type = DRM_MODE_TYPE_PREFERRED;
        auto native = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        timing.type = 0;
        timing.clock = 185625;
        auto native75 = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        timing.hdisplay = 1280;
        timing.vdisplay = 720;
        auto smaller = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        timing.flags = DRM_MODE_FLAG_3D_TOP_AND_BOTTOM;
        auto hdmi = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        const QList<std::shared_ptr<OutputMode>> bases{native, native75, smaller, hdmi};
        const auto modes = DrmConnector::withVirtualStereoModes(bases, {}, anaglyph, other);
        QCOMPARE(modes.size(), count);
        for (int i = 0; i < bases.size(); ++i) {
            QCOMPARE(modes[i], bases[i]);
        }
        for (int i = bases.size(); i < modes.size(); ++i) {
            const auto mode = std::static_pointer_cast<DrmConnectorMode>(modes[i]);
            QVERIFY(mode->flags() & OutputModeline::Flag::VirtualStereo);
            QVERIFY(!(mode->flags() & OutputModeline::Flag::Preferred));
            const auto base = mode->virtualBase();
            QVERIFY(base);
            QVERIFY(base != hdmi);
            QCOMPARE(std::memcmp(base->nativeMode(), mode->nativeMode(), sizeof(drmModeModeInfo)), 0);
            QCOMPARE(*base, *mode);
            const auto layout = stereoLayoutForMode(mode->flags());
            if (isSpatialStereo(layout)) {
                QCOMPARE(mode->size(), QSize(1920, 1080));
            }
            QVERIFY(isAnaglyph(layout) ? anaglyph : other);
        }
        // Rebuilding an unchanged list must preserve mode objects and their protocol lifetime.
        QCOMPARE(DrmConnector::withVirtualStereoModes(bases, modes, anaglyph, other), modes);
        QCOMPARE(DrmConnector::withVirtualStereoModes(bases, modes, false, false), bases);
        for (const auto &mode : modes) {
            QCOMPARE(DrmConnectorMode::resolveVirtualMode(mode, modes), mode);
            if (mode->flags() & OutputModeline::Flag::VirtualStereo) {
                QCOMPARE(DrmConnectorMode::resolveVirtualMode(mode, bases), std::static_pointer_cast<DrmConnectorMode>(mode)->virtualBase());
            }
        }
    }

    void ycbcr420Only()
    {
        drmModeModeInfo timing{};
        timing.clock = 594000;
        timing.hdisplay = 3840;
        timing.htotal = 4400;
        timing.vdisplay = 2160;
        timing.vtotal = 2250;
        timing.type = DRM_MODE_TYPE_PREFERRED;
        auto base = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{}, true);
        const auto modes = DrmConnector::withVirtualStereoModes({base}, {}, true, true);
        QCOMPARE(modes.size(), 3);
        QCOMPARE(stereoLayoutForMode(modes[1]->flags()), StereoLayout::SideBySideHalf);
        QCOMPARE(stereoLayoutForMode(modes[2]->flags()), StereoLayout::TopAndBottom);
    }

    void edid420()
    {
        QFile file(QFINDTESTDATA("../integration/data/Odyssey G5.bin"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray bytes = file.read(128);
        QCOMPARE(bytes.size(), 128);
        bytes[126] = 1;
        bytes.append(QByteArray(128, '\0'));
        bytes[128] = 2; // CTA extension
        bytes[129] = 3;
        bytes[130] = 7;
        bytes[132] = char(0xe2); // Extended block, two bytes
        bytes[133] = 0x0e; // YCbCr 4:2:0-only video data block
        bytes[134] = 97; // 3840x2160p60
        const auto checksum = [&bytes](int offset) {
            unsigned sum = 0;
            for (int i = offset; i < offset + 127; ++i) {
                sum += uint8_t(bytes[i]);
            }
            bytes[offset + 127] = char(-sum);
        };
        checksum(0);
        checksum(128);
        const Edid edid(bytes);
        QVERIFY(edid.isValid());
        QVERIFY(edid.requiresYcbcr420(QSize(3840, 2160), 60000));
        QVERIFY(edid.requiresYcbcr420(QSize(3840, 2160), 59940));
        QVERIFY(!edid.requiresYcbcr420(QSize(3840, 2160), 30000));
        QVERIFY(!edid.requiresYcbcr420(QSize(1920, 1080), 60000));
        // The ordinary video block explicitly permits a full-chroma timing.
        bytes[130] = 9;
        bytes[135] = 0x41;
        bytes[136] = 97;
        checksum(128);
        const Edid fullChroma(bytes);
        QVERIFY(fullChroma.isValid());
        QVERIFY(!fullChroma.requiresYcbcr420(QSize(3840, 2160), 60000));
    }

    void sameModeBlob()
    {
        drmModeModeInfo timing{};
        timing.clock = 148500;
        timing.hdisplay = 1920;
        timing.htotal = 2200;
        timing.vdisplay = 1080;
        timing.vtotal = 1125;
        const auto base = std::make_shared<ModeWithTestBlob>(timing);
        DrmConnectorMode twin(base, OutputModeline::Flag::Stereo3DAnaglyphModern);
        QCOMPARE(twin.blob(), base->testBlob);
        QCOMPARE(twin.blob(), twin.blob());
        // This is the legacy backend's timing comparison before it calls a modeset.
        QVERIFY(twin == *base->nativeMode());
    }

    void sequential()
    {
        // frame sequential twins: every 2D mode of 100 Hz or more, any size, chroma included
        drmModeModeInfo timing{};
        timing.hdisplay = 1920;
        timing.htotal = 2200;
        timing.vdisplay = 1080;
        timing.vtotal = 1125;
        timing.type = DRM_MODE_TYPE_PREFERRED;
        timing.clock = 297000;
        auto base120 = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        QCOMPARE(base120->refreshRate(), 120000u);
        timing.type = 0;
        timing.clock = 247500;
        auto base100 = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        QCOMPARE(base100->refreshRate(), 100000u);
        timing.hdisplay = 1280;
        timing.vdisplay = 720;
        timing.clock = 247252;
        auto base999 = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        QCOMPARE(base999->refreshRate(), 99900u);
        const QList<std::shared_ptr<OutputMode>> bases{base120, base100, base999};

        const auto modes = DrmConnector::withVirtualStereoModes(bases, {}, false, true);
        QList<std::shared_ptr<DrmConnectorMode>> sequential;
        for (const auto &mode : modes) {
            if (isFrameSequential(stereoLayoutForMode(mode->flags()))) {
                sequential.append(std::static_pointer_cast<DrmConnectorMode>(mode));
            }
        }
        QCOMPARE(sequential.size(), 2);
        QCOMPARE(sequential[0]->flags() & OutputModeline::Flag::Stereo3DSequentialLeftFirst,
                 OutputModeline::Flags(OutputModeline::Flag::Stereo3DSequentialLeftFirst));
        QCOMPARE(sequential[0]->virtualBase(), base120);
        QCOMPARE(sequential[1]->virtualBase(), base100);
        // sequential twins share the base mode blob: the timing is sent unchanged
        QCOMPARE(sequential[0]->nativeMode()->flags, base120->nativeMode()->flags);
        QCOMPARE(sequential[0]->nativeMode()->clock, base120->nativeMode()->clock);
        // no twins below 100 Hz, even when it is the preferred size
        for (const auto &mode : modes) {
            QCOMPARE(DrmConnectorMode::resolveVirtualMode(mode, modes), mode);
        }
        QCOMPARE(DrmConnector::withVirtualStereoModes(bases, modes, false, true), modes);
        QCOMPARE(DrmConnector::withVirtualStereoModes(bases, modes, false, false), bases);

        // "other stereo formats" off: anaglyph twins only, nothing sequential
        const auto anaglyphOnly = DrmConnector::withVirtualStereoModes(bases, {}, true, false);
        for (const auto &mode : anaglyphOnly) {
            QVERIFY(!isFrameSequential(stereoLayoutForMode(mode->flags())));
        }
        QCOMPARE(DrmConnector::withVirtualStereoModes({base100}, {}, false, false).size(), 1);
    }

    void sequentialChroma()
    {
        // a 4:2:0-only mode still gets frame sequential twins: the timing is unchanged
        drmModeModeInfo timing{};
        timing.clock = 1188000;
        timing.hdisplay = 3840;
        timing.htotal = 4400;
        timing.vdisplay = 2160;
        timing.vtotal = 2250;
        timing.type = DRM_MODE_TYPE_PREFERRED;
        // 2160p120 with the 4:2:0 pixel rate
        auto base = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{}, true);
        QCOMPARE(base->refreshRate(), 120000u);
        const auto modes = DrmConnector::withVirtualStereoModes({base}, {}, false, true);
        QCOMPARE(modes.size(), 4);
        QCOMPARE(stereoLayoutForMode(modes[1]->flags()), StereoLayout::SideBySideHalf);
        QCOMPARE(stereoLayoutForMode(modes[2]->flags()), StereoLayout::TopAndBottom);
        QCOMPARE(stereoLayoutForMode(modes[3]->flags()), StereoLayout::SequentialLeftFirst);
        for (int i = 0; i < modes.size(); ++i) {
            const auto mode = std::static_pointer_cast<DrmConnectorMode>(modes[i]);
            QCOMPARE(mode->nativeMode()->clock, timing.clock);
        }
    }

    void noPreferredSize()
    {
        drmModeModeInfo timing{};
        // 99.9 Hz, under the frame-sequential threshold
        timing.clock = 999;
        timing.hdisplay = timing.htotal = 100;
        timing.vdisplay = timing.vtotal = 100;
        auto base = std::make_shared<DrmConnectorMode>(nullptr, timing, OutputModeline::Flags{});
        const auto modes = DrmConnector::withVirtualStereoModes({base}, {}, false, true);
        QCOMPARE(modes.size(), 3);
        QCOMPARE(stereoLayoutForMode(modes[1]->flags()), StereoLayout::SideBySideHalf);
        QCOMPARE(stereoLayoutForMode(modes[2]->flags()), StereoLayout::TopAndBottom);
    }
};

QTEST_GUILESS_MAIN(VirtualStereoTest)
#include "virtualStereoTest.moc"
