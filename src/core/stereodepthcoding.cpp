/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/stereodepthcoding.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace KWin
{

static uint16_t clampToUint16(double value)
{
    return uint16_t(std::clamp(std::round(value), 1.0, double(std::numeric_limits<uint16_t>::max())));
}

StereoDepthCoding::StereoDepthCoding(const QSize &eyeSize, const QSize &physicalSize, uint32_t viewingDistance)
    : m_eyeWidth(std::max(eyeSize.width(), 1))
{
    // without a size in the EDID, the CSS reference pixel: 96 per inch
    const double width = physicalSize.width() > 0 ? physicalSize.width() / 10.0 : m_eyeWidth * 2.54 / 96;
    const double height = physicalSize.height() > 0 ? physicalSize.height() / 10.0 : std::max(eyeSize.height(), 1) * 2.54 / 96;
    m_wref = clampToUint16(width);
    m_dref = viewingDistance ? clampToUint16(viewingDistance) : clampToUint16(3 * height);
    // the range either way in centimetres on this screen, in the 127 samples above and below zero
    const double range = s_range1920 * m_wref / 1920.0;
    m_parallaxScale = clampToUint16(std::floor(127 / range));
}

uint16_t StereoDepthCoding::parallaxZero() const
{
    return s_parallaxZero;
}

uint16_t StereoDepthCoding::parallaxScale() const
{
    return m_parallaxScale;
}

uint16_t StereoDepthCoding::dref() const
{
    return m_dref;
}

uint16_t StereoDepthCoding::wref() const
{
    return m_wref;
}

double StereoDepthCoding::samplesPerPixel() const
{
    return m_parallaxScale * double(m_wref) / m_eyeWidth;
}

uint8_t StereoDepthCoding::sample(double parallax) const
{
    return uint8_t(std::clamp(std::round(s_parallaxZero + parallax * samplesPerPixel()), 0.0, 255.0));
}

double StereoDepthCoding::parallax(int sample) const
{
    return (sample - s_parallaxZero) / samplesPerPixel();
}

std::array<uint8_t, 8> StereoDepthCoding::metadata() const
{
    return {
        uint8_t(s_parallaxZero >> 8),
        uint8_t(s_parallaxZero & 0xff),
        uint8_t(m_parallaxScale >> 8),
        uint8_t(m_parallaxScale & 0xff),
        uint8_t(m_dref >> 8),
        uint8_t(m_dref & 0xff),
        uint8_t(m_wref >> 8),
        uint8_t(m_wref & 0xff),
    };
}

}
