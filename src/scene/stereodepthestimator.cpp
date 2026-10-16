/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/stereodepthestimator.h"

#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "utils/common.h"

#include <QMatrix4x4>

namespace KWin
{

StereoDepthEstimator::StereoDepthEstimator() = default;
StereoDepthEstimator::~StereoDepthEstimator() = default;

static QMatrix4x4 projection(const QSize &size)
{
    // a quad of @a size covering the framebuffer; the shaders only read gl_FragCoord
    QMatrix4x4 ret;
    ret.ortho(QRectF(QPointF(), size));
    return ret;
}

bool StereoDepthEstimator::estimate(GLTexture *picture, GLTexture *other, const QVector2D &direction, int range)
{
    if (m_failed || !picture || !other || picture->size() != other->size()) {
        return false;
    }
    if (!m_estimateShader) {
        m_estimateShader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(), QStringLiteral(":/opengl/stereodepthestimate.frag"));
        if (!m_estimateShader) {
            qCWarning(KWIN_CORE) << "Failed to load the stereo depth estimate shader, 3D content shows at its window's level";
            m_failed = true;
            return false;
        }
    }
    const QSize size = picture->size();
    const QSize estimateSize((size.width() + s_scale - 1) / s_scale, (size.height() + s_scale - 1) / s_scale);
    if (!m_estimate || m_estimate->size() != estimateSize) {
        m_estimateFramebuffer.reset();
        m_estimate = GLTexture::allocate(GL_RGBA8, estimateSize);
        if (!m_estimate) {
            return false;
        }
        m_estimate->setFilter(GL_NEAREST);
        m_estimate->setWrapMode(GL_CLAMP_TO_EDGE);
        m_estimateFramebuffer = std::make_unique<GLFramebuffer>(m_estimate.get());
        if (!m_estimateFramebuffer->valid()) {
            m_estimateFramebuffer.reset();
            m_estimate.reset();
            return false;
        }
    }

    GLFramebuffer::pushFramebuffer(m_estimateFramebuffer.get());
    ShaderBinder binder(m_estimateShader.get());
    GLShader *shader = binder.shader();
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection(estimateSize));
    shader->setUniform("picture", 0);
    shader->setUniform("other", 1);
    glUniform2i(shader->uniformLocation("size"), size.width(), size.height());
    shader->setUniform("direction", direction);
    shader->setUniform("range", range);
    shader->setUniform("scale", s_scale);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE1);
    other->bind();
    glActiveTexture(GL_TEXTURE0);
    picture->render(estimateSize);
    glActiveTexture(GL_TEXTURE1);
    other->unbind();
    glActiveTexture(GL_TEXTURE0);
    GLFramebuffer::popFramebuffer();
    return true;
}

bool StereoDepthEstimator::compose(const QSize &size, GLTexture *desktop, bool useEstimate, const StereoDepthCoding &coding)
{
    if (!desktop) {
        return false;
    }
    if (!m_composeShader) {
        m_composeShader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(), QStringLiteral(":/opengl/stereodepthcompose.frag"));
        if (!m_composeShader) {
            qCWarning(KWIN_CORE) << "Failed to load the stereo depth compose shader";
            return false;
        }
    }
    useEstimate = useEstimate && m_estimate;
    ShaderBinder binder(m_composeShader.get());
    GLShader *shader = binder.shader();
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection(size));
    shader->setUniform("desktop", 0);
    shader->setUniform("estimate", 1);
    shader->setUniform("hasEstimate", useEstimate ? 1 : 0);
    shader->setUniform("scale", float(s_scale));
    shader->setUniform("estimateSize", useEstimate ? QVector2D(m_estimate->width(), m_estimate->height()) : QVector2D(1, 1));
    shader->setUniform("zero", float(coding.parallaxZero()));
    shader->setUniform("samplesPerPixel", float(coding.samplesPerPixel()));
    glDisable(GL_BLEND);
    if (useEstimate) {
        glActiveTexture(GL_TEXTURE1);
        m_estimate->bind();
    }
    glActiveTexture(GL_TEXTURE0);
    desktop->render(size);
    if (useEstimate) {
        glActiveTexture(GL_TEXTURE1);
        m_estimate->unbind();
        glActiveTexture(GL_TEXTURE0);
    }
    return true;
}

}
