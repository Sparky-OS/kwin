/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/colorpipeline.h"
#include "core/colorspace.h"
#include "kwin_export.h"

#include <QMatrix4x4>
#include <QVector2D>

#include <QString>

#include <memory>
#include <optional>

namespace KWin
{

/**
 * The conversion from one colour description to another, built once from KWin's colour pipeline
 * (ColorPipeline::create: transfer function, matrix, tone mapping, clamp, encoding) and reused for
 * every frame, both eyes and every output that needs it. get() returns the table of a pair of
 * descriptions; it is built the first time the pair is asked for and rebuilt only when either
 * description changes (a description that compares equal is the same pair).
 *
 * Users:
 * - an output that replicates one with stereo eyes copies the composed eyes and converts them
 *   once into its own blending colour space, instead of painting the scene again
 *   (STEREO3D.md, "An output that replicates another one copies, it does not paint again");
 * - HDR content on an SDR output with deep colour: the same table, with the source an HDR
 *   description (PQ, HLG, BT.2020), tone-mapped into the output's range and kept as a float value
 *   until it is encoded once at the output's depth (10, 12 or 16 bits per channel), so the
 *   gradients keep the source's steps.
 *
 * The values on the GPU are the ones GLShader::setColorspaceUniforms() sets for the pair, taken
 * from the table, so the picture is converted by the same shader code the scene pass uses.
 */
class KWIN_EXPORT ColorConversionTable
{
public:
    /** What the shader gets (GLShader::setColorspaceUniforms), computed once per pair. */
    struct ShaderParameters
    {
        QMatrix4x4 colorimetry;
        int sourceTransferFunction = 0;
        QVector2D sourceTransferFunctionParameters;
        float sourceReferenceLuminance = 0;
        int destinationTransferFunction = 0;
        QVector2D destinationTransferFunctionParameters;
        float destinationReferenceLuminance = 0;
        float maxDestinationLuminance = 0;
        float maxTonemappingLuminance = 0;
        QMatrix4x4 destinationToLMS;
        QMatrix4x4 lmsToDestination;
    };

    struct Statistics
    {
        quint64 builds = 0; // tables built
        quint64 reuses = 0; // get() calls that found the table
    };

    /** The table of a pair; the same object for as long as neither description changes. */
    static std::shared_ptr<const ColorConversionTable> get(const std::shared_ptr<ColorDescription> &from, const std::shared_ptr<ColorDescription> &to, RenderingIntent intent);
    static Statistics statistics();
    /** Forgets every table (a test starts from none). */
    static void clear();

    const std::shared_ptr<ColorDescription> &from() const;
    const std::shared_ptr<ColorDescription> &to() const;
    RenderingIntent intent() const;
    const ColorPipeline &pipeline() const;
    const ShaderParameters &shaderParameters() const;
    /** Nothing to do: the picture already is in the destination description. */
    bool isIdentity() const;

    /** One value of the source's encoding, converted to the destination's encoding, in floating point. */
    QVector3D convert(const QVector3D &encoded) const;

    /**
     * How far the picture of a stereo output's composed eyes, converted with this table, is from
     * the picture painted again for the destination, in the destination's encoded values: for
     * pictures of the given content (colours of a client, an SDR one or an HDR one) the scene pass
     * converts the content to the source's description (and the eyes keep it between 0 and 1) and
     * this table converts that to the destination's; painting again converts the content to the
     * destination's directly. Measured on a grid of grey steps, the primaries and mixed colours.
     */
    double deviationFromPaintingAgain(const std::shared_ptr<ColorDescription> &content) const;

    /**
     * The table to copy composed eyes with, if copying the eyes gives the picture painting the
     * scene again would give, to within one level of 10 bits per channel (so also one level of 8):
     * for SDR content always, and for HDR content when hdrContent says some is on screen (the
     * eyes carry one tone mapping, painting again does it once for the destination, which can
     * differ in the highlights). Otherwise nullptr, and the reason. The rendering intent that fits
     * is chosen from the ones the scene pass and the wire stage use; the verdict of a pair is
     * decided once.
     */
    static std::shared_ptr<const ColorConversionTable> forCopy(const std::shared_ptr<ColorDescription> &from, const std::shared_ptr<ColorDescription> &to, bool hdrContent, QString *reason);

    /** One level at 10 bits per channel, in encoded values. */
    static constexpr double s_copyTolerance = 1.0 / 1023.0;

private:
    ColorConversionTable(const std::shared_ptr<ColorDescription> &from, const std::shared_ptr<ColorDescription> &to, RenderingIntent intent);

    std::shared_ptr<ColorDescription> m_from;
    std::shared_ptr<ColorDescription> m_to;
    RenderingIntent m_intent;
    ColorPipeline m_pipeline;
    ShaderParameters m_shader;
    bool m_identity;
};

}
