/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "kwin_export.h"

#include <QSize>

#include <array>
#include <cstdint>

namespace KWin
{

/**
 * How the depth picture of a picture and depth 3D mode (HDMI 1.4a, Appendix H, L + depth and
 * L + depth + graphics + graphics-depth) codes depth, and the 3D metadata that tells the
 * display how: the parallax information of ISO/IEC 23002-3, sections 6.1.2.2 and 6.2.2.2,
 * which HDMI sends as 3D_Metadata_type 0 (Table H-4): parallax_zero, parallax_scale, dref
 * and wref, 16 bits each, most significant byte first.
 *
 * dref and wref are the reference viewer's distance and the reference screen's width in
 * centimetres (as TSDuck reads them, AuxiliaryVideoStreamDescriptor). Here they are this
 * screen's: its width from the EDID, or at 96 pixels per inch when it has none (the CSS
 * reference pixel, CSS Values and Units Level 4, 6.2), and the viewing distance the user set,
 * or three times the picture's height.
 *
 * Each 8-bit sample m of the depth picture is a parallax on that screen,
 *     p = (m - parallax_zero) / parallax_scale centimetres,
 * positive behind the screen (the right eye's copy to the right of the left eye's), with
 * parallax_zero 128 and parallax_scale chosen so that 64 pixels of a view 1920 wide either
 * way fit in the samples. The formula is our reading of sections 6.1.2.2 and 6.2.2.2 of
 * ISO/IEC 23002-3, whose text is not freely available: UNVERIFIED against the standard.
 */
class KWIN_EXPORT StereoDepthCoding
{
public:
    /**
     * @a eyeSize is one eye's size in pixels, @a physicalSize the screen's in millimetres
     * (empty when unknown), @a viewingDistance the viewer's distance in centimetres, 0 to
     * derive it.
     */
    StereoDepthCoding(const QSize &eyeSize, const QSize &physicalSize, uint32_t viewingDistance);

    static constexpr uint16_t s_parallaxZero = 128;
    // the parallax the samples carry either way, in pixels of a view 1920 wide
    static constexpr double s_range1920 = 64;

    uint16_t parallaxZero() const;
    uint16_t parallaxScale() const;
    uint16_t dref() const;
    uint16_t wref() const;

    /**
     * Samples per pixel of parallax on this screen.
     */
    double samplesPerPixel() const;

    /**
     * The sample for a parallax of @a parallax pixels of one eye, the right eye's position
     * minus the left eye's, clamped to the samples.
     */
    uint8_t sample(double parallax) const;

    /**
     * The parallax in pixels of one eye that @a sample stands for.
     */
    double parallax(int sample) const;

    /**
     * 3D_Metadata_1 to 3D_Metadata_8 (HDMI 1.4a, Appendix H).
     */
    std::array<uint8_t, 8> metadata() const;

    bool operator==(const StereoDepthCoding &other) const = default;

private:
    int m_eyeWidth;
    uint16_t m_parallaxScale;
    uint16_t m_dref;
    uint16_t m_wref;
};

}
