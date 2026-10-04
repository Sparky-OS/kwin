/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "drm_stereo.h"
#include "effect/globals.h"

#include <cstdint>
#include <optional>

namespace KWin
{

inline StereoEye operator!(StereoEye eye)
{
    return eye == StereoEye::Left ? StereoEye::Right : StereoEye::Left;
}

/**
 * Which eye a frame-sequential output shows on each refresh. The eye is tied to the
 * vblank sequence from the page-flip events, so a missed refresh can't swap the eyes for
 * good: when the sequence jumps, the order relocks on the next refresh. A missed refresh
 * itself can carry a wrong eye (a transient slot): it is counted from the flip sequence
 * numbers and reported in the debug output. A strict per-refresh guarantee needs
 * scanout-side alternation in the kernel or hardware; HDMI frame packing, where the
 * display alternates, stays the robust path.
 *
 * The pipeline submits both eyes once per pair of refreshes; when nothing has changed,
 * the previous pair is alternated again, so the glasses never lose sync.
 */
class FrameSequentialScheduler
{
public:
    explicit FrameSequentialScheduler(StereoLayout layout = StereoLayout::SequentialLeftFirst)
        : m_leftFirst(layout != StereoLayout::SequentialRightFirst)
    {
    }

    // the eye the refresh with this vblank sequence must show
    StereoEye eyeForSequence(uint32_t sequence) const
    {
        const uint32_t offset = sequence - m_phase;
        return (offset & 1) == (m_leftFirst ? 0u : 1u) ? StereoEye::Left : StereoEye::Right;
    }

    // the eye the pipeline must submit now, for the refresh after the last completed one
    StereoEye nextEye() const
    {
        return eyeForSequence(m_lastCompleted.value_or(m_phase - 1) + 1);
    }

    /**
     * relock the eye order: refresh @p sequence is the next one and shows the first eye.
     * Called on mode entry, modesets, and after dpms.
     */
    void reset(uint32_t nextSequence)
    {
        m_phase = nextSequence;
        m_lastCompleted.reset();
        m_lastShownEye.reset();
        m_missedRefreshes = 0;
        m_wrongEyeSlots = 0;
    }

    /**
     * a page-flip event: @p sequence is the vblank sequence the submitted frame took.
     * Counts the refreshes between this and the last completion (missed: the previous
     * buffer stayed on screen, showing the wrong eye on every second one of them) and
     * whether the submitted frame was the wrong eye for the refresh it landed on.
     */
    void noteCompletion(uint32_t sequence, StereoEye submittedEye)
    {
        if (m_lastCompleted) {
            const uint32_t gap = sequence - *m_lastCompleted - 1;
            m_missedRefreshes += gap;
            if (gap > 0) {
                const bool firstMissedDue = eyeForSequence(*m_lastCompleted + 1) != *m_lastShownEye;
                // the due eye alternates through the gap, the shown one does not
                m_wrongEyeSlots += firstMissedDue ? (gap + 1) / 2 : gap / 2;
            }
            if (submittedEye != eyeForSequence(sequence)) {
                m_wrongEyeSlots++;
            }
        }
        m_lastCompleted = sequence;
        m_lastShownEye = submittedEye;
    }

    uint64_t missedRefreshes() const
    {
        return m_missedRefreshes;
    }

    uint64_t wrongEyeSlots() const
    {
        return m_wrongEyeSlots;
    }

    std::optional<uint32_t> lastCompleted() const
    {
        return m_lastCompleted;
    }

private:
    bool m_leftFirst;
    uint32_t m_phase = 0;
    std::optional<uint32_t> m_lastCompleted;
    std::optional<StereoEye> m_lastShownEye;
    uint64_t m_missedRefreshes = 0;
    uint64_t m_wrongEyeSlots = 0;
};

}
