/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_egl_layer_surface.h"

#include "config-kwin.h"

#include "core/colortransformation.h"
#include "core/drm_formats.h"
#include "core/graphicsbufferview.h"
#include "core/iccprofile.h"
#include "core/renderdevice.h"
#include "drm_egl_backend.h"
#include "drm_gpu.h"
#include "drm_logging.h"
#include "multigpuswapchain.h"
#include "opengl/eglnativefence.h"
#include "opengl/eglswapchain.h"
#include "opengl/gllut.h"
#include "opengl/glrendertimequery.h"
#include "opengl/icc_shader.h"
#include "qpainter/qpainterswapchain.h"
#include "utils/envvar.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <gbm.h>
#include <unistd.h>

namespace KWin
{

static const ModifierList linearModifier = {DRM_FORMAT_MOD_LINEAR};
static const ModifierList implicitModifier = {DRM_FORMAT_MOD_INVALID};
static const QList<uint32_t> cpuCopyFormats = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};

static const bool bufferAgeEnabled = environmentVariableBoolValue("KWIN_USE_BUFFER_AGE").value_or(true);
static const bool s_forceMGPUSync = environmentVariableBoolValue("KWIN_DRM_FORCE_GL_FINISH_MGPU_COPY").value_or(false);
static const bool s_forcePresentSync = environmentVariableBoolValue("KWIN_DRM_FORCE_GL_FINISH_PRESENT").value_or(false);

static gbm_format_name_desc formatName(uint32_t format)
{
    gbm_format_name_desc ret;
    gbm_format_get_name(format, &ret);
    return ret;
}

EglGbmLayerSurface::EglGbmLayerSurface(DrmGpu *gpu, EglGbmBackend *eglBackend, BufferTarget target)
    : m_gpu(gpu)
    , m_eglBackend(eglBackend)
    , m_requestedBufferTarget(target)
{
    connect(m_gpu, &DrmGpu::renderDeviceChanged, this, &EglGbmLayerSurface::destroyResources);
}

EglGbmLayerSurface::~EglGbmLayerSurface() = default;

EglGbmLayerSurface::Surface::~Surface()
{
    importSwapchain.reset();
    if (context) {
        (void)context->makeCurrent();
    }
}

void EglGbmLayerSurface::destroyResources()
{
    m_surface = {};
    m_oldSurface = {};
}

std::optional<OutputLayerBeginFrameInfo> EglGbmLayerSurface::startRendering(const QSize &bufferSize, OutputTransform transformation,
                                                                            const FormatModifierMap &formats,
                                                                            const std::shared_ptr<ColorDescription> &blendingColor,
                                                                            const std::shared_ptr<ColorDescription> &layerBlendingColor,
                                                                            const std::shared_ptr<IccProfile> &iccProfile,
                                                                            const Colorimetry &wireColor, const TransferFunction::Type &wireTransfer,
                                                                            double scale, BackendOutput::ColorPowerTradeoff tradeoff,
                                                                            bool useShadowBuffer, uint32_t requiredAlphaBits)
{
    if (!checkSurface(bufferSize, formats, tradeoff, requiredAlphaBits)) {
        return std::nullopt;
    }
    m_oldSurface.reset();

    if (!m_eglBackend->openglContext()->makeCurrent()) {
        return std::nullopt;
    }

    auto slot = m_surface->gbmSwapchain->acquire();
    if (!slot) {
        return std::nullopt;
    }

    if (slot->framebuffer()->colorAttachment()->contentTransform() != transformation) {
        m_surface->damageJournal.clear();
    }
    slot->framebuffer()->colorAttachment()->setContentTransform(transformation);
    m_surface->currentSlot = slot;
    m_surface->scale = scale;

    if (m_surface->blendingColor != blendingColor
        || m_surface->layerBlendingColor != layerBlendingColor
        || m_surface->iccProfile != iccProfile
        || m_surface->wireColor != wireColor
        || m_surface->wireTransfer != wireTransfer) {
        m_surface->damageJournal.clear();
        m_surface->shadowDamageJournal.clear();
        m_surface->needsShadowBuffer = useShadowBuffer;
        m_surface->blendingColor = blendingColor;
        m_surface->layerBlendingColor = layerBlendingColor;
        m_surface->iccProfile = iccProfile;
        m_surface->wireColor = wireColor;
        m_surface->wireTransfer = wireTransfer;
        if (iccProfile) {
            if (!m_surface->iccShader) {
                m_surface->iccShader = IccShader::create();
                if (!m_surface->iccShader) {
                    qCWarning(KWIN_DRM) << "Failed to load the ICC shader.";
                }
            }
        } else {
            m_surface->iccShader.reset();
        }
    }

    m_surface->compositingTimeQuery = GLRenderTimeQuery::begin(m_surface->context);
    if (m_surface->needsShadowBuffer) {
        // frame packing and side by side full: the desktop is rendered once at one eye's size
        const QSize shadowSize = isFullFrameStereo(m_stereoLayout) ? m_eyeSize : m_surface->gbmSwapchain->size();
        if (!m_surface->shadowSwapchain || m_surface->shadowSwapchain->size() != shadowSize) {
            const auto formats = m_eglBackend->eglDisplayObject()->nonExternalOnlySupportedDrmFormats();
            const QList<FormatInfo> sortedFormats = OutputLayer::filterAndSortFormats(formats, requiredAlphaBits, tradeoff);
            for (const auto format : sortedFormats) {
                GraphicsBufferOptions options{
                    .size = shadowSize,
                    .format = format.drmFormat,
                    .modifiers = formats[format.drmFormat],
                    .software = false,
                    .scanout = false,
                };
                m_surface->shadowSwapchain = EglSwapchain::create(m_eglBackend->renderDevice(), options);
                if (m_surface->shadowSwapchain) {
                    break;
                }
            }
        }
        if (!m_surface->shadowSwapchain) {
            qCCritical(KWIN_DRM) << "Failed to create shadow swapchain!";
            return std::nullopt;
        }
        m_surface->currentShadowSlot = m_surface->shadowSwapchain->acquire();
        if (!m_surface->currentShadowSlot) {
            return std::nullopt;
        }
        m_surface->currentShadowSlot->texture()->setContentTransform(m_surface->currentSlot->framebuffer()->colorAttachment()->contentTransform());
        return OutputLayerBeginFrameInfo{
            .renderTarget = RenderTarget(m_surface->currentShadowSlot->framebuffer(), m_surface->blendingColor),
            .repaint = bufferAgeEnabled ? m_surface->shadowDamageJournal.accumulate(m_surface->currentShadowSlot->age(), Region::infinite()) : Region::infinite(),
        };
    } else {
        m_surface->shadowSwapchain.reset();
        m_surface->currentShadowSlot.reset();
        m_surface->rightShadowSwapchain.reset();
        m_surface->currentRightShadowSlot.reset();
        return OutputLayerBeginFrameInfo{
            .renderTarget = RenderTarget(m_surface->currentSlot->framebuffer(), m_surface->blendingColor),
            .repaint = bufferAgeEnabled ? m_surface->damageJournal.accumulate(slot->age(), Region::infinite()) : Region::infinite(),
        };
    }
}

static GLVertexBuffer *uploadGeometry(const Region &devicePixels, const QSize &fboSize)
{
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
    const auto optMap = vbo->map<GLVertex2D>(devicePixels.rects().size() * 6);
    if (!optMap) {
        return nullptr;
    }
    const auto map = *optMap;
    size_t vboIndex = 0;
    for (RectF rect : devicePixels.rects()) {
        const float x0 = rect.left();
        const float y0 = rect.top();
        const float x1 = rect.right();
        const float y1 = rect.bottom();

        const float u0 = x0 / fboSize.width();
        const float v0 = y0 / fboSize.height();
        const float u1 = x1 / fboSize.width();
        const float v1 = y1 / fboSize.height();

        // first triangle
        map[vboIndex++] = GLVertex2D{
            .position = QVector2D(x0, y0),
            .texcoord = QVector2D(u0, v0),
        };
        map[vboIndex++] = GLVertex2D{
            .position = QVector2D(x1, y1),
            .texcoord = QVector2D(u1, v1),
        };
        map[vboIndex++] = GLVertex2D{
            .position = QVector2D(x0, y1),
            .texcoord = QVector2D(u0, v1),
        };

        // second triangle
        map[vboIndex++] = GLVertex2D{
            .position = QVector2D(x0, y0),
            .texcoord = QVector2D(u0, v0),
        };
        map[vboIndex++] = GLVertex2D{
            .position = QVector2D(x1, y0),
            .texcoord = QVector2D(u1, v0),
        };
        map[vboIndex++] = GLVertex2D{
            .position = QVector2D(x1, y1),
            .texcoord = QVector2D(u1, v1),
        };
    }
    vbo->unmap();
    vbo->setVertexCount(vboIndex);
    return vbo;
}

// Each eye's rectangle, in fbo coordinates, gets the whole shadow texture
static GLVertexBuffer *uploadStereoGeometry(std::span<const RectF> eyes)
{
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
    const auto optMap = vbo->map<GLVertex2D>(eyes.size() * 6);
    if (!optMap) {
        return nullptr;
    }
    const auto map = *optMap;
    size_t vboIndex = 0;
    for (const RectF &eye : eyes) {
        const float x0 = eye.left();
        const float y0 = eye.top();
        const float x1 = eye.right();
        const float y1 = eye.bottom();
        map[vboIndex++] = GLVertex2D{.position = QVector2D(x0, y0), .texcoord = QVector2D(0, 0)};
        map[vboIndex++] = GLVertex2D{.position = QVector2D(x1, y1), .texcoord = QVector2D(1, 1)};
        map[vboIndex++] = GLVertex2D{.position = QVector2D(x0, y1), .texcoord = QVector2D(0, 1)};
        map[vboIndex++] = GLVertex2D{.position = QVector2D(x0, y0), .texcoord = QVector2D(0, 0)};
        map[vboIndex++] = GLVertex2D{.position = QVector2D(x1, y0), .texcoord = QVector2D(1, 0)};
        map[vboIndex++] = GLVertex2D{.position = QVector2D(x1, y1), .texcoord = QVector2D(1, 1)};
    }
    vbo->unmap();
    vbo->setVertexCount(vboIndex);
    return vbo;
}

// Dubois least-squares matrices for red/cyan glasses (REEL3D No. 7003): the CRT set as
// given by Sanders and McAllister, the modern-screen set as given by Zhang and McAllister
// (coefficients as collected at http://chrisjones.id.au/Dubois/Dubois.html); rows are the
// output's red, green and blue, columns the eye's.
static QMatrix4x4 duboisMatrix(StereoLayout layout, bool leftEye)
{
    if (layout == StereoLayout::AnaglyphModern) {
        return leftEye ? QMatrix4x4(0.4154, 0.4710, 0.1669, 0, -0.0458, -0.0484, -0.0257, 0, -0.0547, -0.0615, 0.0128, 0, 0, 0, 0, 0)
                       : QMatrix4x4(-0.0109, -0.0364, -0.0060, 0, 0.3756, 0.7333, 0.0111, 0, -0.0651, -0.1287, 1.2971, 0, 0, 0, 0, 0);
    }
    return leftEye ? QMatrix4x4(0.456, 0.500, 0.176, 0, -0.040, -0.038, -0.016, 0, -0.015, -0.021, -0.005, 0, 0, 0, 0, 0)
                   : QMatrix4x4(-0.043, -0.088, -0.002, 0, 0.378, 0.734, -0.018, 0, -0.072, -0.113, 1.226, 0, 0, 0, 0, 0);
}

bool EglGbmLayerSurface::drawAnaglyph(const QSize &fboSize, const Region &repaint)
{
    if (!m_surface->anaglyphShader && !m_surface->anaglyphShaderFailed) {
        m_surface->anaglyphShader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(), QStringLiteral(":/opengl/anaglyph.frag"));
        if (!m_surface->anaglyphShader) {
            m_surface->anaglyphShaderFailed = true;
            qCWarning(KWIN_DRM) << "Failed to load the anaglyph shader, showing the desktop flat";
        }
    }
    if (!m_surface->anaglyphShader) {
        return false;
    }
    ShaderBinder binder(m_surface->anaglyphShader.get());
    GLShader *shader = binder.shader();
    shader->setColorspaceUniforms(m_surface->blendingColor, m_surface->layerBlendingColor, RenderingIntent::AbsoluteColorimetricNoAdaptation);
    // the right eye's own picture when the scene was rendered once per eye, else the one desktop
    const std::shared_ptr<GLTexture> rightEye = m_surface->currentRightShadowSlot ? m_surface->currentRightShadowSlot->texture() : nullptr;
    shader->setUniform("leftEye", 0);
    shader->setUniform("rightEye", rightEye ? 1 : 0);
    shader->setUniform("leftMatrix", duboisMatrix(m_stereoLayout, true));
    shader->setUniform("rightMatrix", duboisMatrix(m_stereoLayout, false));
    QMatrix4x4 mat;
    mat.scale(1, -1);
    mat.ortho(QRectF(QPointF(), fboSize));
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mat);
    if (const auto vbo = uploadGeometry(repaint, m_surface->gbmSwapchain->size())) {
        if (rightEye) {
            glActiveTexture(GL_TEXTURE1);
            rightEye->bind();
            glActiveTexture(GL_TEXTURE0);
        }
        m_surface->currentShadowSlot->texture()->bind();
        vbo->render(GL_TRIANGLES);
        m_surface->currentShadowSlot->texture()->unbind();
        if (rightEye) {
            glActiveTexture(GL_TEXTURE1);
            rightEye->unbind();
            glActiveTexture(GL_TEXTURE0);
        }
    }
    return true;
}

std::optional<OutputLayerBeginFrameInfo> EglGbmLayerSurface::startRightEye()
{
    if (!m_surface || !m_surface->needsShadowBuffer || !m_surface->currentShadowSlot) {
        return std::nullopt;
    }
    const QSize size = m_surface->shadowSwapchain->size();
    const uint32_t format = m_surface->shadowSwapchain->format();
    if (!m_surface->rightShadowSwapchain || m_surface->rightShadowSwapchain->size() != size || m_surface->rightShadowSwapchain->format() != format) {
        m_surface->rightShadowDamageJournal.clear();
        GraphicsBufferOptions options{
            .size = size,
            .format = format,
            .modifiers = {m_surface->shadowSwapchain->modifier()},
            .software = false,
            .scanout = false,
        };
        m_surface->rightShadowSwapchain = EglSwapchain::create(m_eglBackend->renderDevice(), options);
        if (!m_surface->rightShadowSwapchain) {
            qCWarning(KWIN_DRM) << "Failed to create the right eye's shadow swapchain";
            return std::nullopt;
        }
    }
    m_surface->currentRightShadowSlot = m_surface->rightShadowSwapchain->acquire();
    if (!m_surface->currentRightShadowSlot) {
        return std::nullopt;
    }
    m_surface->currentRightShadowSlot->texture()->setContentTransform(m_surface->currentShadowSlot->texture()->contentTransform());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_surface->currentRightShadowSlot->framebuffer(), m_surface->blendingColor),
        .repaint = bufferAgeEnabled ? m_surface->rightShadowDamageJournal.accumulate(m_surface->currentRightShadowSlot->age(), Region::infinite()) : Region::infinite(),
    };
}

void EglGbmLayerSurface::setStereoLayout(StereoLayout layout, const QSize &eyeSize, const QPoint &rightEyeOffset)
{
    if ((layout != m_stereoLayout || eyeSize != m_eyeSize || rightEyeOffset != m_rightEyeOffset) && m_surface) {
        m_surface->damageJournal.clear();
    }
    m_stereoLayout = layout;
    m_eyeSize = eyeSize;
    m_rightEyeOffset = rightEyeOffset;
}

bool EglGbmLayerSurface::endRendering(const Region &damagedDeviceRegion, OutputFrame *frame)
{
    // in 3D both eyes change together, so the whole scanout buffer is new every frame
    const bool anaglyph = isAnaglyph(m_stereoLayout) && m_surface->needsShadowBuffer;
    const bool stereo = m_stereoLayout != StereoLayout::None && m_surface->needsShadowBuffer;
    const Region scanoutDamage = stereo ? Region(Rect(QPoint(), m_surface->gbmSwapchain->size())) : damagedDeviceRegion;
    if (m_surface->needsShadowBuffer) {
        const Region deviceRepaint = scanoutDamage | m_surface->damageJournal.accumulate(m_surface->currentSlot->age(), Region::infinite());
        m_surface->damageJournal.add(scanoutDamage);
        m_surface->shadowDamageJournal.add(damagedDeviceRegion);
        if (m_surface->currentRightShadowSlot) {
            m_surface->rightShadowDamageJournal.add(damagedDeviceRegion);
        } else {
            // not rendered this frame: the next right eye is painted whole
            m_surface->rightShadowDamageJournal.clear();
        }
        const auto mapping = m_surface->currentShadowSlot->framebuffer()->colorAttachment()->contentTransform().combine(OutputTransform::FlipY);
        const QSize rotatedSize = mapping.map(m_surface->gbmSwapchain->size());
        const Region repaint = mapping.map(deviceRepaint & Rect(QPoint(), rotatedSize), rotatedSize);

        GLFramebuffer *fbo = m_surface->currentSlot->framebuffer();
        GLFramebuffer::pushFramebuffer(fbo);
        ShaderBinder binder = m_surface->iccShader ? ShaderBinder(m_surface->iccShader->shader()) : ShaderBinder(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
        // this transform is absolute colorimetric, whitepoint adjustment is done in compositing already
        if (m_surface->iccShader) {
            m_surface->iccShader->setUniforms(m_surface->iccProfile, m_surface->blendingColor,
                                              m_surface->wireColor, m_surface->wireTransfer,
                                              RenderingIntent::AbsoluteColorimetricNoAdaptation);
        } else {
            binder.shader()->setColorspaceUniforms(m_surface->blendingColor, m_surface->layerBlendingColor, RenderingIntent::AbsoluteColorimetricNoAdaptation);
        }
        QMatrix4x4 mat;
        mat.scale(1, -1);
        mat.ortho(QRectF(QPointF(), fbo->size()));
        binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mat);
        glDisable(GL_BLEND);
        if (anaglyph && drawAnaglyph(fbo->size(), repaint)) {
            // both eyes mixed; the repaint is the whole buffer in 3D
        } else if (stereo && !anaglyph) {
            // the desktop into both eyes, left eye first: left half and right half for
            // side by side, top half and bottom half for top and bottom, each eye in full
            // for frame packing (black between them) and side by side full
            const int w = rotatedSize.width();
            const int h = rotatedSize.height();
            std::array<Rect, 2> deviceEyes;
            switch (m_stereoLayout) {
            case StereoLayout::SideBySideHalf:
                deviceEyes = {Rect(0, 0, w / 2, h), Rect(w / 2, 0, w - w / 2, h)};
                break;
            case StereoLayout::FramePacking:
            case StereoLayout::SideBySideFull:
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                deviceEyes = {Rect(QPoint(0, 0), m_eyeSize), Rect(m_rightEyeOffset, m_eyeSize)};
                break;
            default:
                deviceEyes = {Rect(0, 0, w, h / 2), Rect(0, h / 2, w, h - h / 2)};
                break;
            }
            const std::array<RectF, 2> fboEyes{RectF(mapping.map(deviceEyes[0], rotatedSize)), RectF(mapping.map(deviceEyes[1], rotatedSize))};
            // each eye from its own shadow buffer when the scene was rendered once per eye
            const std::array<std::shared_ptr<GLTexture>, 2> eyeTextures{
                m_surface->currentShadowSlot->texture(),
                m_surface->currentRightShadowSlot ? m_surface->currentRightShadowSlot->texture() : m_surface->currentShadowSlot->texture(),
            };
            for (size_t eye = 0; eye < fboEyes.size(); ++eye) {
                if (const auto vbo = uploadStereoGeometry(std::span(&fboEyes[eye], 1))) {
                    eyeTextures[eye]->bind();
                    vbo->render(GL_TRIANGLES);
                    eyeTextures[eye]->unbind();
                }
            }
        } else if (const auto vbo = uploadGeometry(repaint, m_surface->gbmSwapchain->size())) {
            m_surface->currentShadowSlot->texture()->bind();
            vbo->render(GL_TRIANGLES);
            m_surface->currentShadowSlot->texture()->unbind();
        }
        EGLNativeFence fence(m_surface->context->displayObject());
        if (m_surface->currentRightShadowSlot) {
            m_surface->rightShadowSwapchain->release(m_surface->currentRightShadowSlot, fence.fileDescriptor().duplicate());
            m_surface->currentRightShadowSlot.reset();
        }
        m_surface->shadowSwapchain->release(m_surface->currentShadowSlot, fence.takeFileDescriptor());
        GLFramebuffer::popFramebuffer();
    } else {
        m_surface->damageJournal.add(scanoutDamage);
    }
    if (frame && m_surface->compositingTimeQuery) {
        m_surface->compositingTimeQuery->end();
        frame->addRenderTimeQuery(std::move(m_surface->compositingTimeQuery));
    }
    glFlush();
    EGLNativeFence sourceFence(m_eglBackend->eglDisplayObject());
    if (!sourceFence.isValid() || s_forcePresentSync) {
        // llvmpipe doesn't do synchronization properly: https://gitlab.freedesktop.org/mesa/mesa/-/issues/9375
        // and NVidia doesn't support implicit sync
        glFinish();
    }
    m_surface->gbmSwapchain->release(m_surface->currentSlot, sourceFence.fileDescriptor().duplicate());
    const auto buffer = importBuffer(m_surface.get(), m_surface->currentSlot.get(), sourceFence.takeFileDescriptor(), frame, scanoutDamage);
    if (buffer) {
        m_surface->currentFramebuffer = buffer;
        return true;
    } else {
        return false;
    }
}

EglGbmBackend *EglGbmLayerSurface::eglBackend() const
{
    return m_eglBackend;
}

std::shared_ptr<DrmFramebuffer> EglGbmLayerSurface::currentBuffer() const
{
    return m_surface ? m_surface->currentFramebuffer : nullptr;
}

const std::shared_ptr<ColorDescription> &EglGbmLayerSurface::colorDescription() const
{
    if (m_surface) {
        return m_surface->blendingColor;
    } else {
        return ColorDescription::sRGB;
    }
}

std::shared_ptr<DrmFramebuffer> EglGbmLayerSurface::renderTestBuffer(const QSize &bufferSize, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits)
{
    EglContext *context = m_eglBackend->openglContext();
    if (!context->makeCurrent()) {
        qCWarning(KWIN_DRM) << "EglGbmLayerSurface::renderTestBuffer: failed to make opengl context current";
        return nullptr;
    }

    if (checkSurface(bufferSize, formats, tradeoff, requiredAlphaBits)) {
        return m_surface->currentFramebuffer;
    } else {
        return nullptr;
    }
}

void EglGbmLayerSurface::forgetDamage()
{
    if (m_surface) {
        m_surface->damageJournal.clear();
        m_surface->shadowDamageJournal.clear();
        m_surface->rightShadowDamageJournal.clear();
    }
}

bool EglGbmLayerSurface::checkSurface(const QSize &size, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits)
{
    if (doesSurfaceFit(m_surface.get(), size, formats, tradeoff, requiredAlphaBits)) {
        return true;
    }
    if (doesSurfaceFit(m_oldSurface.get(), size, formats, tradeoff, requiredAlphaBits)) {
        m_surface = std::move(m_oldSurface);
        return true;
    }
    if (m_gpu->renderDevice() && m_gpu->renderDevice() != m_eglBackend->renderDevice() && m_gpu->renderDevice()->isInReset()) {
        // avoid creating a suboptimal swapchain until the reset is complete
        return false;
    }
    if (auto newSurface = createSurface(size, formats, tradeoff, requiredAlphaBits)) {
        m_oldSurface = std::move(m_surface);
        if (m_oldSurface) {
            // FIXME: Use absolute frame sequence numbers for indexing the DamageJournal
            m_oldSurface->damageJournal.clear();
            m_oldSurface->shadowDamageJournal.clear();
            m_oldSurface->gbmSwapchain->resetBufferAge();
            if (m_oldSurface->shadowSwapchain) {
                m_oldSurface->shadowSwapchain->resetBufferAge();
            }
            if (m_oldSurface->importSwapchain) {
                m_oldSurface->importSwapchain->resetDamageTracking();
            }
        }
        m_surface = std::move(newSurface);
        return true;
    }
    return false;
}

bool EglGbmLayerSurface::doesSurfaceFit(Surface *surface, const QSize &size, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits) const
{
    if (!surface || surface->needsRecreation || !surface->gbmSwapchain || surface->gbmSwapchain->size() != size) {
        return false;
    }
    if (surface->tradeoff != tradeoff || surface->requiredAlphaBits != requiredAlphaBits) {
        // TODO requiredAlphaBits could be a bit more conservative with reallocations?
        return false;
    }
    if (surface->bufferTarget == BufferTarget::Dumb) {
        return formats.contains(surface->importDumbSwapchain->format());
    }
    switch (surface->importMode) {
    case MultiGpuImportMode::None:
        return formats.containsFormat(surface->gbmSwapchain->format(), surface->gbmSwapchain->modifier());
    case MultiGpuImportMode::DumbBuffer:
        return formats.contains(surface->importDumbSwapchain->format());
    case MultiGpuImportMode::GpuCopy:
        return !surface->importSwapchain->needsRecreation()
            && formats.containsFormat(surface->importSwapchain->format(), surface->importSwapchain->modifier());
    }
    Q_UNREACHABLE();
}

std::unique_ptr<EglGbmLayerSurface::Surface> EglGbmLayerSurface::createSurface(const QSize &size, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits) const
{
    const QList<FormatInfo> sortedFormats = OutputLayer::filterAndSortFormats(formats, requiredAlphaBits, tradeoff);

    // special case: the cursor plane needs linear, but not all GPUs (NVidia) can render to linear
    auto bufferTarget = m_requestedBufferTarget;
    if (m_gpu->renderDevice() == m_eglBackend->renderDevice()) {
        const bool needsLinear = std::ranges::all_of(sortedFormats, [&formats](const FormatInfo &fmt) {
            const auto &mods = formats[fmt.drmFormat];
            return std::ranges::all_of(mods, [](uint64_t mod) {
                return mod == DRM_FORMAT_MOD_LINEAR;
            });
        });
        if (needsLinear) {
            const auto renderFormats = m_eglBackend->eglDisplayObject()->nonExternalOnlySupportedDrmFormats();
            const bool noLinearSupport = std::ranges::none_of(sortedFormats, [&renderFormats](const auto &formatInfo) {
                return renderFormats.containsFormat(formatInfo.drmFormat, DRM_FORMAT_MOD_LINEAR);
            });
            if (noLinearSupport) {
                bufferTarget = BufferTarget::Dumb;
            }
        }
    }

    const auto doTestFormats = [this, &size, &formats, bufferTarget, tradeoff, requiredAlphaBits](const QList<FormatInfo> &gbmFormats, MultiGpuImportMode importMode) -> std::unique_ptr<Surface> {
        for (const auto &format : gbmFormats) {
            auto surface = createSurface(size, format.drmFormat, formats[format.drmFormat], importMode, bufferTarget, tradeoff, requiredAlphaBits);
            if (surface) {
                return surface;
            }
        }
        return nullptr;
    };
    if (m_gpu->renderDevice() == m_eglBackend->renderDevice()) {
        return doTestFormats(sortedFormats, MultiGpuImportMode::None);
    }
    // special case, we're using different display devices but the same render device
    const auto device = m_gpu->renderDevice();
    if (device && !device->eglDisplay()->renderNode().isEmpty() && device->eglDisplay()->renderNode() == m_eglBackend->eglDisplayObject()->renderNode()) {
        if (auto surface = doTestFormats(sortedFormats, MultiGpuImportMode::None)) {
            return surface;
        }
    }
    if (auto surface = doTestFormats(sortedFormats, MultiGpuImportMode::GpuCopy)) {
        // qCDebug(KWIN_DRM) << "chose egl import with format" << formatName(surface->gbmSwapchain->format()).name << "and modifier" << surface->gbmSwapchain->modifier();
        return surface;
    }
    if (auto surface = doTestFormats(sortedFormats, MultiGpuImportMode::DumbBuffer)) {
        qCDebug(KWIN_DRM) << "chose cpu import with format" << formatName(surface->gbmSwapchain->format()).name << "and modifier" << surface->gbmSwapchain->modifier();
        return surface;
    }
    return nullptr;
}

std::unique_ptr<EglGbmLayerSurface::Surface> EglGbmLayerSurface::createSurface(const QSize &size, uint32_t format, const ModifierList &modifiers, MultiGpuImportMode importMode, BufferTarget bufferTarget, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits) const
{
    const bool cpuCopy = importMode == MultiGpuImportMode::DumbBuffer || bufferTarget == BufferTarget::Dumb;
    auto ret = std::make_unique<Surface>();
    ModifierList renderModifiers = m_eglBackend->eglDisplayObject()->nonExternalOnlySupportedDrmFormats()[format];
    if (cpuCopy) {
        if (!cpuCopyFormats.contains(format)) {
            return nullptr;
        }
        ret->importDumbSwapchain = std::make_unique<QPainterSwapchain>(m_gpu->drmDevice()->allocator(), size, format, true);
        if (!ret->importDumbSwapchain) {
            return nullptr;
        }
    } else if (importMode == MultiGpuImportMode::GpuCopy) {
        if (!m_eglBackend->renderDevice()
            || !m_gpu->renderDevice()
            || m_eglBackend->renderDevice()->isSoftwareDevice()
            || m_gpu->renderDevice()->isSoftwareDevice()) {
            return nullptr;
        }
        auto copy = MultiGpuSwapchain::createForScanout(m_eglBackend->renderDevice(), m_gpu->drmDevice(), format, renderModifiers, size, FormatModifierMap{{format, modifiers}});
        if (!copy) {
            return nullptr;
        }
        ret->importSwapchain = std::move(copy->swapchain);
        renderModifiers = copy->importModifiers;
    } else {
        renderModifiers.intersect(modifiers);
    }
    if (renderModifiers.empty()) {
        return nullptr;
    }
    ret->context = m_eglBackend->openglContextRef();
    ret->bufferTarget = bufferTarget;
    ret->importMode = importMode;
    GraphicsBufferOptions options{
        .size = size,
        .format = format,
        .modifiers = renderModifiers,
        .software = false,
        .scanout = importMode == MultiGpuImportMode::None && bufferTarget == BufferTarget::Normal,
    };
    if (importMode == MultiGpuImportMode::None && bufferTarget == BufferTarget::Normal) {
        ret->gbmSwapchain = EglSwapchain::create(m_gpu->drmDevice()->allocator(), m_eglBackend->openglContextRef(), options);
    } else {
        ret->gbmSwapchain = EglSwapchain::create(m_eglBackend->renderDevice(), options);
    }
    ret->tradeoff = tradeoff;
    ret->requiredAlphaBits = requiredAlphaBits;
    if (!ret->gbmSwapchain) {
        return nullptr;
    }
    if (!doRenderTestBuffer(ret.get())) {
        return nullptr;
    }
    return ret;
}

std::shared_ptr<DrmFramebuffer> EglGbmLayerSurface::doRenderTestBuffer(Surface *surface) const
{
    auto slot = surface->gbmSwapchain->acquire();
    if (!slot) {
        return nullptr;
    }
    if (!m_gpu->atomicModeSetting()) {
        EglContext::currentContext()->pushFramebuffer(slot->framebuffer());
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        EglContext::currentContext()->popFramebuffer();
    }
    if (const auto ret = importBuffer(surface, slot.get(), FileDescriptor{}, nullptr, Region::infinite())) {
        // clear the render journal, because this was just a nonsense frame
        if (surface->importSwapchain) {
            surface->importSwapchain->resetDamageTracking();
        }
        surface->currentSlot = slot;
        surface->currentFramebuffer = ret;
        return ret;
    } else {
        return nullptr;
    }
}

std::shared_ptr<DrmFramebuffer> EglGbmLayerSurface::importBuffer(Surface *surface, EglSwapchainSlot *slot, FileDescriptor &&readFence, OutputFrame *frame, const Region &damagedDeviceRegion) const
{
    if (surface->bufferTarget == BufferTarget::Dumb || surface->importMode == MultiGpuImportMode::DumbBuffer) {
        return importWithCpu(surface, slot, frame);
    } else if (surface->importMode == MultiGpuImportMode::GpuCopy) {
        return importWithCopy(surface, slot, std::move(readFence), frame, damagedDeviceRegion);
    } else {
        const auto ret = m_gpu->importBuffer(slot->buffer(), std::move(readFence));
        if (!ret) {
            qCWarning(KWIN_DRM, "Failed to create framebuffer: %s", strerror(errno));
        }
        return ret;
    }
}

std::shared_ptr<DrmFramebuffer> EglGbmLayerSurface::importWithCopy(Surface *surface, EglSwapchainSlot *source, FileDescriptor &&readFence, OutputFrame *frame, const Region &damagedDeviceRegion) const
{
    Q_ASSERT(surface->importSwapchain);

    const auto renderDevice = m_gpu->renderDevice();
    // older versions of the NVidia proprietary driver support neither implicit sync nor EGL_ANDROID_native_fence_sync
    if (!readFence.isValid() || !renderDevice->eglDisplay()->supportsNativeFence() || s_forceMGPUSync) {
        glFinish();
    }

    // The source texture transform looks as follows: output transform + flip-y transform. The
    // flip-y transform is added by the render backend to handle the OpenGL render target origin
    // being in the bottom-left corner.
    //
    // As is, without the flip-y transform, the top-left corner of the graphics buffer will
    // map to the bottom-left corner of the render target and the final image will look upside down.
    //
    // .combine(FlipY) undoes the flip-y transform in "output transform + flip-y" so we only end
    // with the output transform and the bufferDamage has its origin in the top-left corner rather
    // than in the bottom-left corner.
    const OutputTransform mapping = source->texture()->contentTransform().combine(OutputTransform::FlipY);
    const QSize orientedSize = mapping.map(source->texture()->size());
    const Region bufferDamage = mapping.map(damagedDeviceRegion, orientedSize);
    auto imported = surface->importSwapchain->copyRgbBuffer(source->buffer(), bufferDamage, std::move(readFence), frame, source->releasePoint());
    if (!imported) {
        // this is probably caused by a GPU reset, let's not take any chances
        surface->needsRecreation = true;
        return nullptr;
    }
    return m_gpu->importBuffer(imported->buffer, std::move(imported->sync));
}

std::shared_ptr<DrmFramebuffer> EglGbmLayerSurface::importWithCpu(Surface *surface, EglSwapchainSlot *source, OutputFrame *frame) const
{
    std::unique_ptr<CpuRenderTimeQuery> copyTime;
    if (frame) {
        copyTime = std::make_unique<CpuRenderTimeQuery>();
    }
    Q_ASSERT(surface->importDumbSwapchain);
    const auto slot = surface->importDumbSwapchain->acquire();
    if (!slot) {
        qCWarning(KWIN_DRM) << "EglGbmLayerSurface::importWithCpu: failed to get a target dumb buffer";
        return nullptr;
    }
    const auto size = source->buffer()->size();
    const qsizetype srcStride = 4 * size.width();
    EglContext *context = m_eglBackend->openglContext();
    GLFramebuffer::pushFramebuffer(source->framebuffer());
    QImage *const dst = slot->view()->image();
    if (dst->bytesPerLine() == srcStride) {
        context->glReadnPixels(0, 0, dst->width(), dst->height(), GL_BGRA, GL_UNSIGNED_BYTE, dst->sizeInBytes(), dst->bits());
    } else {
        // there's padding, need to copy line by line
        if (surface->cpuCopyCache.size() != dst->size()) {
            surface->cpuCopyCache = QImage(dst->size(), QImage::Format_RGBA8888);
        }
        context->glReadnPixels(0, 0, dst->width(), dst->height(), GL_BGRA, GL_UNSIGNED_BYTE, surface->cpuCopyCache.sizeInBytes(), surface->cpuCopyCache.bits());
        for (int i = 0; i < dst->height(); i++) {
            std::memcpy(dst->scanLine(i), surface->cpuCopyCache.scanLine(i), srcStride);
        }
    }
    GLFramebuffer::popFramebuffer();

    const auto ret = m_gpu->importBuffer(slot->buffer(), FileDescriptor{});
    if (!ret) {
        qCWarning(KWIN_DRM, "Failed to create a framebuffer: %s", strerror(errno));
    }
    surface->importDumbSwapchain->release(slot);
    if (frame) {
        copyTime->end();
        frame->addRenderTimeQuery(std::move(copyTime));
    }
    return ret;
}

}

#include "moc_drm_egl_layer_surface.cpp"
