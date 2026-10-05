/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QHash>
#include <QMap>
#include <QPointer>
#include <chrono>
#include <optional>

#include "core/outputlayer.h"
#include "drm_plane.h"
#include "drm_stereo.h"
#include "opengl/gltexture.h"
#include "utils/damagejournal.h"
#include "utils/filedescriptor.h"

namespace KWin
{

class DrmFramebuffer;
class EglSwapchain;
class EglSwapchainSlot;
class QPainterSwapchain;
class EglContext;
class EglGbmBackend;
class GraphicsBuffer;
class SurfaceItem;
class GLTexture;
class GLShader;
class GLRenderTimeQuery;
class ColorTransformation;
class GlLookUpTable;
class IccProfile;
class IccShader;
class MultiGpuSwapchain;

class EglGbmLayerSurface : public QObject
{
    Q_OBJECT

public:
    enum class BufferTarget {
        Normal,
        Dumb,
    };
    explicit EglGbmLayerSurface(DrmGpu *gpu, EglGbmBackend *eglBackend, BufferTarget target = BufferTarget::Normal);
    ~EglGbmLayerSurface();

    std::optional<OutputLayerBeginFrameInfo> startRendering(const QSize &bufferSize, OutputTransform transformation, const FormatModifierMap &formats,
                                                            const std::shared_ptr<ColorDescription> &blendingColor,
                                                            const std::shared_ptr<ColorDescription> &layerBlendingColor,
                                                            const std::shared_ptr<IccProfile> &iccProfile,
                                                            const Colorimetry &wireColor, const TransferFunction::Type &wireTransfer,
                                                            double scale, BackendOutput::ColorPowerTradeoff tradeoff,
                                                            bool useShadowBuffer, uint32_t requiredAlphaBits);
    bool endRendering(const Region &damagedDeviceRegion, OutputFrame *frame);
    /**
     * In 3D the shadow buffer holds the desktop once, and endRendering() draws it into
     * both eyes of the scanout buffer; needs the shadow buffer (DrmOutput::needsShadowBuffer).
     */
    void setStereoPair(StereoPairMode mode, StereoPairRole role);
    void setStereoLayout(StereoLayout layout, const QSize &eyeSize = QSize(), const QPoint &rightEyeOffset = QPoint(), bool eyeSwap = false);
    /**
     * With stereo content the scene is rendered once per eye: the shadow buffer from
     * startRendering() holds the left eye, this one the right eye. Call it between
     * startRendering() and endRendering(); without it the left eye is shown in both.
     */
    std::optional<OutputLayerBeginFrameInfo> startRightEye();

    void destroyResources();
    EglGbmBackend *eglBackend() const;
    std::shared_ptr<DrmFramebuffer> renderTestBuffer(const QSize &bufferSize, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits);
    void forgetDamage();

    std::shared_ptr<DrmFramebuffer> currentBuffer() const;
    const std::shared_ptr<ColorDescription> &colorDescription() const;

private:
    enum class MultiGpuImportMode {
        None,
        GpuCopy,
        DumbBuffer,
    };
    struct Surface
    {
        ~Surface();

        bool needsRecreation = false;

        std::shared_ptr<EglContext> context;
        std::shared_ptr<EglSwapchain> gbmSwapchain;
        std::shared_ptr<EglSwapchainSlot> currentSlot;
        DamageJournal damageJournal;
        std::unique_ptr<QPainterSwapchain> importDumbSwapchain;
        std::unique_ptr<MultiGpuSwapchain> importSwapchain;
        QImage cpuCopyCache;
        MultiGpuImportMode importMode;
        std::shared_ptr<DrmFramebuffer> currentFramebuffer;
        BufferTarget bufferTarget;
        double scale = 1;
        uint32_t requiredAlphaBits = 0;

        // for color management
        bool needsShadowBuffer = false;
        std::shared_ptr<EglSwapchain> shadowSwapchain;
        std::shared_ptr<EglSwapchainSlot> currentShadowSlot;
        std::shared_ptr<ColorDescription> layerBlendingColor = ColorDescription::sRGB;
        std::shared_ptr<ColorDescription> blendingColor = ColorDescription::sRGB;
        double brightness = 1.0;
        std::unique_ptr<IccShader> iccShader;
        std::shared_ptr<IccProfile> iccProfile;
        Colorimetry wireColor = Colorimetry::BT709;
        TransferFunction::Type wireTransfer = TransferFunction::Type::gamma22;
        DamageJournal shadowDamageJournal;
        // the right eye's shadow buffer, when the scene is rendered once per eye
        std::shared_ptr<EglSwapchain> rightShadowSwapchain;
        std::shared_ptr<EglSwapchainSlot> currentRightShadowSlot;
        DamageJournal rightShadowDamageJournal;
        BackendOutput::ColorPowerTradeoff tradeoff = BackendOutput::ColorPowerTradeoff::PreferEfficiency;

        std::unique_ptr<GLRenderTimeQuery> compositingTimeQuery;

        // the two eyes mixed as red/cyan (anaglyph layouts)
        std::unique_ptr<GLShader> stereoPatternShader;
        bool stereoPatternShaderFailed = false;
        std::unique_ptr<GLShader> anaglyphShader;
        bool anaglyphShaderFailed = false;
    };
    bool drawStereoPattern(const QSize &fboSize, const Region &repaint);
    bool drawAnaglyph(const QSize &fboSize, const Region &repaint);
    bool drawIzed3d(const QSize &fboSize, const Region &repaint);
    bool checkSurface(const QSize &size, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits);
    bool doesSurfaceFit(Surface *surface, const QSize &size, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits) const;
    std::unique_ptr<Surface> createSurface(const QSize &size, const FormatModifierMap &formats, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits) const;
    std::unique_ptr<Surface> createSurface(const QSize &size, uint32_t format, const ModifierList &modifiers, MultiGpuImportMode importMode, BufferTarget bufferTarget, BackendOutput::ColorPowerTradeoff tradeoff, uint32_t requiredAlphaBits) const;
    std::shared_ptr<EglSwapchain> createGbmSwapchain(DrmGpu *gpu, EglContext *context, const QSize &size, uint32_t format, const ModifierList &modifiers, MultiGpuImportMode importMode, BufferTarget bufferTarget) const;

    std::shared_ptr<DrmFramebuffer> doRenderTestBuffer(Surface *surface) const;
    std::shared_ptr<DrmFramebuffer> importBuffer(Surface *surface, EglSwapchainSlot *source, FileDescriptor &&readFence, OutputFrame *frame, const Region &damagedDeviceRegion) const;
    std::shared_ptr<DrmFramebuffer> importWithCopy(Surface *surface, EglSwapchainSlot *source, FileDescriptor &&readFence, OutputFrame *frame, const Region &damagedDeviceRegion) const;
    std::shared_ptr<DrmFramebuffer> importWithCpu(Surface *surface, EglSwapchainSlot *source, OutputFrame *frame) const;

    std::unique_ptr<Surface> m_surface;
    std::unique_ptr<Surface> m_oldSurface;

    DrmGpu *const m_gpu;
    EglGbmBackend *const m_eglBackend;
    StereoLayout m_stereoLayout = StereoLayout::None;
    // frame packing and side by side full: one eye's size, and where the right eye starts
    QSize m_eyeSize;
    QPoint m_rightEyeOffset;
    StereoPairMode m_stereoPairMode = StereoPairMode::None;
    StereoPairRole m_stereoPairRole = StereoPairRole::Left;
    bool m_eyeSwap = false;
    const BufferTarget m_requestedBufferTarget;
};

}
