/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "core/colorconversion.h"

#include "utils/common.h"

#include <QString>

#include <algorithm>
#include <mutex>
#include <vector>

namespace KWin
{

static const bool s_disableTonemapping = qEnvironmentVariableIntValue("KWIN_DISABLE_TONEMAPPING") == 1;

namespace
{
struct Cache
{
    std::mutex mutex;
    std::vector<std::shared_ptr<const ColorConversionTable>> tables; // most recently used first
    ColorConversionTable::Statistics statistics;
};

Cache &cache()
{
    static Cache s_cache;
    return s_cache;
}

// outputs change their descriptions rarely and a desktop has a few: a handful of tables is enough,
// the least recently used one goes when the cache is full
constexpr size_t s_maxTables = 32;

bool sameDescription(const std::shared_ptr<ColorDescription> &one, const std::shared_ptr<ColorDescription> &two)
{
    return one == two || (one && two && *one == *two);
}
}

ColorConversionTable::ColorConversionTable(const std::shared_ptr<ColorDescription> &from, const std::shared_ptr<ColorDescription> &to, RenderingIntent intent)
    : m_from(from)
    , m_to(to)
    , m_intent(intent)
    , m_pipeline(ColorPipeline::create(from, to, intent, ColorPipeline::InputType::FloatingPoint))
    , m_identity(from == to || *from == *to)
{
    const auto &src = *from;
    const auto &dst = *to;
    m_shader.colorimetry = src.toOther(dst, intent);
    m_shader.sourceTransferFunction = src.transferFunction().type;
    if (src.transferFunction().type == TransferFunction::BT1886) {
        m_shader.sourceTransferFunctionParameters = QVector2D(src.transferFunction().bt1886B(), src.transferFunction().bt1886A());
    } else {
        m_shader.sourceTransferFunctionParameters = QVector2D(src.transferFunction().minLuminance, src.transferFunction().maxLuminance - src.transferFunction().minLuminance);
    }
    m_shader.sourceReferenceLuminance = src.referenceLuminance();
    m_shader.destinationTransferFunction = dst.transferFunction().type;
    if (dst.transferFunction().type == TransferFunction::BT1886) {
        m_shader.destinationTransferFunctionParameters = QVector2D(dst.transferFunction().bt1886B(), dst.transferFunction().bt1886A());
    } else {
        m_shader.destinationTransferFunctionParameters = QVector2D(dst.transferFunction().minLuminance, dst.transferFunction().maxLuminance - dst.transferFunction().minLuminance);
    }
    m_shader.destinationReferenceLuminance = dst.referenceLuminance();
    m_shader.maxDestinationLuminance = dst.maxHdrLuminance().value_or(10'000);
    if (!s_disableTonemapping && intent == RenderingIntent::Perceptual) {
        m_shader.maxTonemappingLuminance = src.maxHdrLuminance().value_or(src.referenceLuminance()) * dst.referenceLuminance() / src.referenceLuminance();
    } else {
        m_shader.maxTonemappingLuminance = dst.maxHdrLuminance().value_or(10'000);
    }
    m_shader.destinationToLMS = dst.containerColorimetry().toLMS();
    m_shader.lmsToDestination = dst.containerColorimetry().fromLMS();
}

std::shared_ptr<const ColorConversionTable> ColorConversionTable::get(const std::shared_ptr<ColorDescription> &from, const std::shared_ptr<ColorDescription> &to, RenderingIntent intent)
{
    Cache &c = cache();
    std::lock_guard lock(c.mutex);
    for (auto it = c.tables.begin(); it != c.tables.end(); ++it) {
        const auto &table = **it;
        if (table.m_intent == intent && sameDescription(table.m_from, from) && sameDescription(table.m_to, to)) {
            c.statistics.reuses++;
            if (it != c.tables.begin()) {
                std::rotate(c.tables.begin(), it, it + 1);
            }
            return c.tables.front();
        }
    }
    c.statistics.builds++;
    std::shared_ptr<const ColorConversionTable> table(new ColorConversionTable(from, to, intent));
    qCDebug(KWIN_CORE, "color-conversion: built table %llu (transfer functions %d -> %d, intent %d, reference %g -> %g cd/m2)", static_cast<unsigned long long>(c.statistics.builds), int(from->transferFunction().type), int(to->transferFunction().type), int(intent), from->referenceLuminance(), to->referenceLuminance());
    if (c.tables.size() >= s_maxTables) {
        c.tables.pop_back();
    }
    c.tables.insert(c.tables.begin(), table);
    return table;
}

ColorConversionTable::Statistics ColorConversionTable::statistics()
{
    Cache &c = cache();
    std::lock_guard lock(c.mutex);
    return c.statistics;
}

const std::shared_ptr<ColorDescription> &ColorConversionTable::from() const
{
    return m_from;
}

const std::shared_ptr<ColorDescription> &ColorConversionTable::to() const
{
    return m_to;
}

RenderingIntent ColorConversionTable::intent() const
{
    return m_intent;
}

const ColorPipeline &ColorConversionTable::pipeline() const
{
    return m_pipeline;
}

const ColorConversionTable::ShaderParameters &ColorConversionTable::shaderParameters() const
{
    return m_shader;
}

bool ColorConversionTable::isIdentity() const
{
    return m_identity;
}

QVector3D ColorConversionTable::convert(const QVector3D &encoded) const
{
    return m_pipeline.evaluate(encoded);
}

// The colours the comparison is made on: 33 greys, the primaries and secondaries at three levels
// and a cube of mixed colours.
static std::vector<QVector3D> probeColors()
{
    std::vector<QVector3D> colors;
    for (int i = 0; i <= 32; ++i) {
        const float v = i / 32.0f;
        colors.emplace_back(v, v, v);
    }
    for (const float level : {0.25f, 0.5f, 1.0f}) {
        for (int mask = 1; mask < 8; ++mask) {
            colors.emplace_back((mask & 1) ? level : 0.0f, (mask & 2) ? level : 0.0f, (mask & 4) ? level : 0.0f);
        }
    }
    for (int r = 0; r < 5; ++r) {
        for (int g = 0; g < 5; ++g) {
            for (int b = 0; b < 5; ++b) {
                colors.emplace_back(r / 4.0f, g / 4.0f, b / 4.0f);
            }
        }
    }
    return colors;
}

double ColorConversionTable::deviationFromPaintingAgain(const std::shared_ptr<ColorDescription> &content) const
{
    // the scene pass converts with the perceptual intent, as every client surface does
    const ColorPipeline toSource = ColorPipeline::create(content, m_from, RenderingIntent::Perceptual, ColorPipeline::InputType::FloatingPoint);
    const ColorPipeline toDestination = ColorPipeline::create(content, m_to, RenderingIntent::Perceptual, ColorPipeline::InputType::FloatingPoint);
    double worst = 0;
    for (const QVector3D &color : probeColors()) {
        QVector3D eye = toSource.evaluate(color);
        // the eyes are stored in a buffer: unsigned normalized, between 0 and 1
        eye = QVector3D(std::clamp(eye.x(), 0.0f, 1.0f), std::clamp(eye.y(), 0.0f, 1.0f), std::clamp(eye.z(), 0.0f, 1.0f));
        QVector3D viaEyes = m_identity ? eye : m_pipeline.evaluate(eye);
        viaEyes = QVector3D(std::clamp(viaEyes.x(), 0.0f, 1.0f), std::clamp(viaEyes.y(), 0.0f, 1.0f), std::clamp(viaEyes.z(), 0.0f, 1.0f));
        const QVector3D direct = toDestination.evaluate(color);
        const QVector3D directClamped(std::clamp(direct.x(), 0.0f, 1.0f), std::clamp(direct.y(), 0.0f, 1.0f), std::clamp(direct.z(), 0.0f, 1.0f));
        worst = std::max({worst, double(std::abs(viaEyes.x() - directClamped.x())), double(std::abs(viaEyes.y() - directClamped.y())), double(std::abs(viaEyes.z() - directClamped.z()))});
    }
    return worst;
}

namespace
{
// what forCopy decided for a pair: asked every frame, decided once
struct CopyVerdict
{
    std::shared_ptr<ColorDescription> from;
    std::shared_ptr<ColorDescription> to;
    std::shared_ptr<const ColorConversionTable> table; // right for SDR content, null when it is not
    bool hdrExact = false; // right for HDR content too
    QString reason; // why not, for SDR content or for HDR content
};
std::mutex s_verdictMutex;
std::vector<CopyVerdict> s_verdicts;
}

void ColorConversionTable::clear()
{
    {
        Cache &c = cache();
        std::lock_guard lock(c.mutex);
        c.tables.clear();
        c.statistics = {};
    }
    std::lock_guard lock(s_verdictMutex);
    s_verdicts.clear();
}

std::shared_ptr<const ColorConversionTable> ColorConversionTable::forCopy(const std::shared_ptr<ColorDescription> &from, const std::shared_ptr<ColorDescription> &to, bool hdrContent, QString *reason)
{
    const CopyVerdict *found = nullptr;
    CopyVerdict decided;
    {
        std::lock_guard lock(s_verdictMutex);
        for (const CopyVerdict &verdict : s_verdicts) {
            if (sameDescription(verdict.from, from) && sameDescription(verdict.to, to)) {
                decided = verdict;
                found = &decided;
                break;
            }
        }
    }
    if (!found) {
        // SDR content (sRGB) and HDR content (BT.2020 PQ), each with the intents the scene pass
        // and the wire stage use; the first intent that fits both is the table, else the one that
        // fits SDR content with the least deviation for HDR
        decided = CopyVerdict{from, to, nullptr, false, QString()};
        double bestSdr = 1;
        double bestHdr = 1;
        for (const RenderingIntent intent : {RenderingIntent::Perceptual, RenderingIntent::RelativeColorimetricWithBPC, RenderingIntent::AbsoluteColorimetricNoAdaptation, RenderingIntent::RelativeColorimetric}) {
            auto table = get(from, to, intent);
            const double sdr = table->deviationFromPaintingAgain(ColorDescription::sRGB);
            const double hdr = table->deviationFromPaintingAgain(ColorDescription::BT2020PQ);
            bestSdr = std::min(bestSdr, sdr);
            if (sdr <= s_copyTolerance && (!decided.table || (hdr <= s_copyTolerance && !decided.hdrExact))) {
                decided.table = table;
                decided.hdrExact = hdr <= s_copyTolerance;
            }
            if (sdr <= s_copyTolerance) {
                bestHdr = std::min(bestHdr, hdr);
            }
            if (decided.hdrExact) {
                break;
            }
        }
        if (!decided.table) {
            decided.reason = QStringLiteral("the conversion of the eyes differs from painting the scene again by %1 of the range").arg(bestSdr);
        } else if (!decided.hdrExact) {
            decided.reason = QStringLiteral("HDR content: the tone mapping of the converted eyes differs from painting the scene again by %1 of the range").arg(bestHdr);
        }
        if (decided.table && decided.hdrExact) {
            qCDebug(KWIN_CORE, "color-conversion: eyes can be copied between these colour spaces, intent %d", int(decided.table->intent()));
        } else {
            qCDebug(KWIN_CORE, "color-conversion: eyes copied between these colour spaces only for SDR content: %s", qPrintable(decided.reason));
        }
        std::lock_guard lock(s_verdictMutex);
        if (s_verdicts.size() >= s_maxTables) {
            s_verdicts.erase(s_verdicts.begin());
        }
        s_verdicts.push_back(decided);
    }
    if (reason) {
        *reason = decided.reason;
    }
    if (hdrContent && !decided.hdrExact) {
        return nullptr;
    }
    return decided.table;
}

}
