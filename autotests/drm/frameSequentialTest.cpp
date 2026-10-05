/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "drm_frame_sequential.h"
#include "drm_stereo.h"

#include <QTest>

using namespace KWin;

class FrameSequentialTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void eyeFromSequence()
    {
        // the pinned example from the contract: left first, refresh 100 shows the left eye
        FrameSequentialScheduler leftFirst;
        leftFirst.reset(100);
        QCOMPARE(leftFirst.eyeForSequence(100), StereoEye::Left);
        QCOMPARE(leftFirst.eyeForSequence(101), StereoEye::Right);
        QCOMPARE(leftFirst.eyeForSequence(102), StereoEye::Left);
        QCOMPARE(leftFirst.eyeForSequence(105), StereoEye::Right);
        QCOMPARE(leftFirst.eyeForSequence(106), StereoEye::Left);

        // unsigned wrap: 0xFFFFFFFE is two refreshes before 0
        leftFirst.reset(0xFFFFFFFE);
        QCOMPARE(leftFirst.eyeForSequence(0xFFFFFFFE), StereoEye::Left);
        QCOMPARE(leftFirst.eyeForSequence(0xFFFFFFFF), StereoEye::Right);
        QCOMPARE(leftFirst.eyeForSequence(0), StereoEye::Left);
        QCOMPARE(leftFirst.eyeForSequence(1), StereoEye::Right);
    }

    void timelyAlternation()
    {
        FrameSequentialScheduler scheduler;
        scheduler.reset(101);
        // each submission for the next refresh lands on it: no misses, no wrong slots
        for (uint32_t sequence = 101; sequence <= 106; ++sequence) {
            QCOMPARE(scheduler.nextEye(), scheduler.eyeForSequence(sequence));
            scheduler.noteCompletion(sequence, scheduler.nextEye());
        }
        QCOMPARE(scheduler.missedRefreshes(), 0u);
        QCOMPARE(scheduler.wrongEyeSlots(), 0u);
    }

    void noSubmissionHoldsBuffer()
    {
        FrameSequentialScheduler scheduler;
        scheduler.reset(101);
        scheduler.noteCompletion(101, StereoEye::Left);
        // three refreshes pass with nothing submitted: the left buffer stays on screen,
        // the second of the three is a wrong-eye slot but the eye order is not lost
        QCOMPARE(scheduler.nextEye(), StereoEye::Right);
        scheduler.noteCompletion(104, StereoEye::Right);
        QCOMPARE(scheduler.missedRefreshes(), 2u);
        QCOMPARE(scheduler.wrongEyeSlots(), 1u);
        QCOMPARE(scheduler.nextEye(), scheduler.eyeForSequence(105));
        QCOMPARE(scheduler.nextEye(), StereoEye::Left);
    }

    void lateSubmissionAndPhaseRecovery()
    {
        // the contract example: left at 100; the right submission is one refresh late,
        // so 101 shows the old left (wrong eye) and 102 shows the late right (also wrong);
        // the next submission targets the refresh by its sequence and the order relocks
        FrameSequentialScheduler scheduler;
        scheduler.reset(100);
        scheduler.noteCompletion(100, StereoEye::Left);

        QCOMPARE(scheduler.nextEye(), StereoEye::Right);
        // refresh 101 passes with no submission pending
        scheduler.noteCompletion(102, StereoEye::Right);
        QCOMPARE(scheduler.missedRefreshes(), 1u);
        QCOMPARE(scheduler.wrongEyeSlots(), 2u);

        // relock comes from the sequence, not from what was shown
        QCOMPARE(scheduler.nextEye(), StereoEye::Right);
        scheduler.noteCompletion(103, StereoEye::Right);
        QCOMPARE(scheduler.nextEye(), StereoEye::Left);
        scheduler.noteCompletion(104, StereoEye::Left);
        QCOMPARE(scheduler.missedRefreshes(), 1u);
        QCOMPARE(scheduler.wrongEyeSlots(), 2u);
    }

    void togglingPerFlipSustainsInversion()
    {
        // the failure the contract rules out: choosing the eye opposite to what was last
        // shown instead of the eye due for the refresh leaves the eyes swapped for good
        FrameSequentialScheduler scheduler;
        scheduler.reset(100);
        scheduler.noteCompletion(100, StereoEye::Left);

        // missed refresh 101, then late right at 102 as in the previous test
        scheduler.noteCompletion(102, StereoEye::Right);
        auto shown = StereoEye::Right;
        // toggling from now on sustains the inversion and keeps piling up wrong slots
        for (uint32_t sequence = 103; sequence <= 104; ++sequence) {
            shown = !shown;
            scheduler.noteCompletion(sequence, shown);
        }
        QCOMPARE(scheduler.missedRefreshes(), 1u);
        QCOMPARE(scheduler.wrongEyeSlots(), 4u);
    }

    void unsignedMissedCount()
    {
        // missed refreshes count across a sequence wrap: two refreshes between FFFE and 1
        FrameSequentialScheduler scheduler;
        scheduler.reset(0xFFFFFFFE);
        scheduler.noteCompletion(0xFFFFFFFE, StereoEye::Left);
        scheduler.noteCompletion(0xFFFFFFFF, StereoEye::Right);
        QCOMPARE(scheduler.missedRefreshes(), 0u);
        // the wrap itself: FFFFFFFF -> 1 misses one refresh (0), a wrong-eye slot
        scheduler.noteCompletion(1, StereoEye::Right);
        QCOMPARE(scheduler.missedRefreshes(), 1u);
        QCOMPARE(scheduler.wrongEyeSlots(), 1u);
        QVERIFY(scheduler.lastCompleted().has_value());
        QCOMPARE(*scheduler.lastCompleted(), 1u);
    }

    void resetClearsCounters()
    {
        FrameSequentialScheduler scheduler;
        scheduler.reset(100);
        scheduler.noteCompletion(100, StereoEye::Left);
        scheduler.noteCompletion(103, StereoEye::Right);
        QVERIFY(scheduler.missedRefreshes() > 0);
        scheduler.reset(200);
        QCOMPARE(scheduler.missedRefreshes(), 0u);
        QCOMPARE(scheduler.wrongEyeSlots(), 0u);
        QCOMPARE(scheduler.nextEye(), StereoEye::Left);
        QVERIFY(!scheduler.lastCompleted().has_value());
    }

    void scanoutUsesFullSideBySideFrame()
    {
        drmModeModeInfo mode{};
        mode.hdisplay = 1920;
        mode.vdisplay = 1080;

        QCOMPARE(stereoFrameSize(mode, StereoLayout::SequentialLeftFirst), QSize(3840, 1080));
        QCOMPARE(stereoRightEyeOffset(mode, StereoLayout::SequentialLeftFirst), QPoint(1920, 0));
        QCOMPARE(frameSequentialSourceRect(mode, StereoEye::Left), Rect(QPoint(0, 0), QSize(1920, 1080)));
        QCOMPARE(frameSequentialSourceRect(mode, StereoEye::Right), Rect(QPoint(1920, 0), QSize(1920, 1080)));
    }

    void frameArrivingDuringPairWaitsForLeftRefresh()
    {
        FrameSequentialScheduler scheduler;
        scheduler.reset(100);
        scheduler.noteCompletion(100, StereoEye::Left);
        // A newly drawn frame must not replace the held pair on the right refresh.
        QCOMPARE(scheduler.nextEye(), StereoEye::Right);
        QVERIFY(scheduler.nextEye() != StereoEye::Left);
        scheduler.noteCompletion(101, StereoEye::Right);
        // The next submission is the first eye of the next pair.
        QCOMPARE(scheduler.nextEye(), StereoEye::Left);
        QCOMPARE(scheduler.firstEye(), StereoEye::Left);

    }
};

QTEST_GUILESS_MAIN(FrameSequentialTest)
#include "frameSequentialTest.moc"
