/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "backends/drm/drm_stereo_pair.h"

#include <QTest>

using namespace KWin;

class StereoPairTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sourceRectSelectsEachEye();
    void rolesMustBeComplementary();
    void mirrorReflectionIsAnOutputTransform();
    void ized3dUsesEncodedAverageAndRatio();
    void ized3dBlackPixelsUseTheDefinedFallback();
};

static drmModeModeInfo mode()
{
    drmModeModeInfo ret{};
    ret.hdisplay = 1920;
    ret.vdisplay = 1080;
    return ret;
}

void StereoPairTest::sourceRectSelectsEachEye()
{
    const auto native = mode();
    QCOMPARE(stereoPairSourceRect(native, StereoPairRole::Left), Rect(QPoint(0, 0), QSize(1920, 1080)));
    QCOMPARE(stereoPairSourceRect(native, StereoPairRole::Right), Rect(QPoint(1920, 0), QSize(1920, 1080)));
    QCOMPARE(stereoPairSourceRect(native, StereoPairRole::Back), stereoPairSourceRect(native, StereoPairRole::Left));
    QCOMPARE(stereoPairSourceRect(native, StereoPairRole::Front), stereoPairSourceRect(native, StereoPairRole::Right));
}

void StereoPairTest::rolesMustBeComplementary()
{
    QVERIFY(stereoPairRolesMatch(StereoPairMode::DualProjection, StereoPairRole::Left, StereoPairRole::Right));
    QVERIFY(stereoPairRolesMatch(StereoPairMode::MirrorRig, StereoPairRole::Right, StereoPairRole::Left));
    QVERIFY(stereoPairRolesMatch(StereoPairMode::Ized3d, StereoPairRole::Back, StereoPairRole::Front));
    QVERIFY(!stereoPairRolesMatch(StereoPairMode::Ized3d, StereoPairRole::Left, StereoPairRole::Right));
    QVERIFY(!stereoPairRolesMatch(StereoPairMode::DualProjection, StereoPairRole::Left, StereoPairRole::Left));
}

void StereoPairTest::mirrorReflectionIsAnOutputTransform()
{
    QCOMPARE(stereoPairOutputTransform(StereoPairReflection::None), OutputTransform::Normal);
    QCOMPARE(stereoPairOutputTransform(StereoPairReflection::Horizontal), OutputTransform::FlipX);
    QCOMPARE(stereoPairOutputTransform(StereoPairReflection::Vertical), OutputTransform::FlipY);
}

void StereoPairTest::ized3dUsesEncodedAverageAndRatio()
{
    const auto result = ized3dPixel(QVector3D(0.25f, 0.5f, 0.75f), QVector3D(0.75f, 0.5f, 0.25f));
    QCOMPARE(result.back, QVector3D(0.5f, 0.5f, 0.5f));
    QCOMPARE(result.front, QVector3D(0.75f, 0.5f, 0.25f));
}

void StereoPairTest::ized3dBlackPixelsUseTheDefinedFallback()
{
    const auto result = ized3dPixel(QVector3D(), QVector3D());
    QCOMPARE(result.back, QVector3D());
    QCOMPARE(result.front, QVector3D(0.5f, 0.5f, 0.5f));
}

QTEST_GUILESS_MAIN(StereoPairTest)
#include "stereoPairTest.moc"
