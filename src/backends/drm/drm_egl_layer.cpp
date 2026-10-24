/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_egl_layer.h"
#include "drm_stereo.h"
#include "core/colorconversion.h"
#include "core/colorpipeline.h"
#include "core/iccprofile.h"
#include "core/renderdevice.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_crtc.h"
#include "drm_egl_backend.h"
#include "drm_gpu.h"
#include "drm_logging.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "scene/surfaceitem_wayland.h"
#include "utils/envvar.h"
#include "wayland/surface.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <gbm.h>
#include <unistd.h>

namespace KWin
{

static EglGbmLayerSurface::BufferTarget targetFor(DrmGpu *gpu, DrmPlane::TypeIndex planeType)
{
    if ((!gpu->atomicModeSetting() || gpu->drmDevice()->isVirtualMachine()) && planeType == DrmPlane::TypeIndex::Cursor) {
        return EglGbmLayerSurface::BufferTarget::Dumb;
    } else {
        return EglGbmLayerSurface::BufferTarget::Normal;
    }
}

EglGbmLayer::EglGbmLayer(EglGbmBackend *eglBackend, DrmPlane *plane)
    : DrmPipelineLayer(plane)
    , m_surface(plane->gpu(), eglBackend, targetFor(plane->gpu(), plane->type.enumValue()))
{
}

EglGbmLayer::EglGbmLayer(EglGbmBackend *eglBackend, DrmGpu *gpu, DrmPlane::TypeIndex type)
    : DrmPipelineLayer(type)
    , m_surface(gpu, eglBackend, targetFor(gpu, type))
{
}

// the scanout buffer: for frame packing, side by side full and frame sequential the whole frame holding both
// eyes, otherwise the layer's size
QSize EglGbmLayer::scanoutSize() const
{
    const StereoLayout layout = drmOutput()->activeStereoLayout();
    if (m_type == OutputLayerType::Primary && drmOutput()->isStereoPair() && drmOutput()->stereoPairMode() == StereoPairMode::Ized3d) {
        return pipeline()->mode()->size();
    }
    if (m_type == OutputLayerType::Primary && isFullFrameStereo(layout)) {
        return stereoFrameSize(*pipeline()->mode()->nativeMode(), layout, pipeline()->mode()->flags());
    }
    return targetRect().size();
}

std::optional<OutputLayerBeginFrameInfo> EglGbmLayer::doBeginFrame()
{
    m_scanoutBuffer.reset();
    const StereoLayout stereoLayout = drmOutput()->activeStereoLayout();
    if (m_type == OutputLayerType::Primary && isFullFrameStereo(stereoLayout)) {
        const drmModeModeInfo *mode = pipeline()->mode()->nativeMode();
        const OutputModeline::Flags flags = pipeline()->mode()->flags();
        m_surface.setStereoLayout(stereoLayout, stereoEyeSize(*mode, flags), stereoRightEyeOffset(*mode, stereoLayout, flags));
    } else {
        m_surface.setStereoLayout(stereoLayout);
    }
    m_surface.setStereoPair(drmOutput()->isStereoPair() ? drmOutput()->stereoPairMode() : StereoPairMode::None,
                            drmOutput()->stereoPairRole());
    // Ordinary pairs share one packed buffer; each plane selects its own eye.
    m_surface.setStereoSwapEyes(drmOutput()->stereoSwapEyes()
                               && (!drmOutput()->isStereoPair() || drmOutput()->stereoPairMode() == StereoPairMode::Ized3d));
    m_surface.setIzed3dOptions(drmOutput()->ized3dMode(), drmOutput()->ized3dTransfer());
    return m_surface.startRendering(scanoutSize(),
                                    drmOutput()->transform().combine(OutputTransform::FlipY),
                                    supportedDrmFormats(),
                                    drmOutput()->blendingColor(),
                                    drmOutput()->layerBlendingColor(),
                                    drmOutput()->needsShadowBuffer() ? pipeline()->iccProfile() : nullptr,
                                    drmOutput()->wireColor(drmOutput()->nextState()),
                                    drmOutput()->wireTransfer(drmOutput()->nextState()),
                                    drmOutput()->scale(),
                                    drmOutput()->colorPowerTradeoff(),
                                    drmOutput()->needsShadowBuffer(),
                                    m_requiredAlphaBits);
}

bool EglGbmLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    return m_surface.endRendering(damagedDeviceRegion, frame);
}

bool EglGbmLayer::hasStereoEyes() const
{
    return m_type == OutputLayerType::Primary && drmOutput()->activeStereoLayout() != StereoLayout::None;
}

void EglGbmLayer::setEyesWanted(bool wanted)
{
    DrmPipelineLayer::setEyesWanted(wanted);
    m_surface.setShareEyes(wanted && m_type == OutputLayerType::Primary);
}

std::shared_ptr<StereoEyePair> EglGbmLayer::composedEyes() const
{
    return m_surface.composedEyes();
}

// The copy lands in the shadow buffers this layer would have painted the scene into, and
// everything after them (the layout, the eye swap, the colour wire) stays this layer's own, so
// the eyes only have to be in the orientation and colour space of this layer's own scene.
bool EglGbmLayer::canCopyEyes(const StereoEyePair &eyes) const
{
    const auto no = [this](const char *why) {
        qCDebug(KWIN_DRM, "stereo-replica: %s cannot copy the eyes: %s", qPrintable(drmOutput()->name()), why);
        return false;
    };
    if (m_type != OutputLayerType::Primary) {
        return no("not the primary layer");
    }
    if (!m_surface.isSameDevice(eyes.owner)) {
        return no("another device");
    }
    if (drmOutput()->isStereoPair() || !stereoPlanes().isEmpty()) {
        return no("stereo pair or picture and depth layout");
    }
    if (isFrameSequential(drmOutput()->activeStereoLayout())) {
        return no("frame sequential layout");
    }
    if (drmOutput()->transform() != OutputTransform::Kind::Normal || eyes.transform != OutputTransform(OutputTransform::Kind::Normal).combine(OutputTransform::FlipY)) {
        return no("transformed output");
    }
    if (pipeline()->iccProfile()) {
        return no("ICC profile");
    }
    // the eyes are in the colour space the composing layer blends in; this layer's wire stage
    // reads its own shadow buffers as blendingColor. Another colour space (brightness, dimming,
    // HDR, a different colour resolution) is converted once on the copy, with the table of the
    // pair, when that gives what painting the scene again would give.
    if (!eyes.blendingColor || !drmOutput()->blendingColor()) {
        return no("no colour space");
    }
    if (!(*eyes.blendingColor == *drmOutput()->blendingColor())) {
        QString reason;
        if (!ColorConversionTable::forCopy(eyes.blendingColor, drmOutput()->blendingColor(), eyesHdrContent(), &reason)) {
            return no(qPrintable(QStringLiteral("another colour space: ") + reason));
        }
    }
    return true;
}

std::shared_ptr<const ColorConversionTable> EglGbmLayer::eyesConversion(const StereoEyePair &eyes) const
{
    if (!eyes.blendingColor || !drmOutput()->blendingColor() || *eyes.blendingColor == *drmOutput()->blendingColor()) {
        return nullptr;
    }
    return ColorConversionTable::forCopy(eyes.blendingColor, drmOutput()->blendingColor(), eyesHdrContent(), nullptr);
}

std::optional<OutputLayerBeginFrameInfo> EglGbmLayer::beginRightEyeFrame()
{
    return m_surface.startRightEye();
}

QList<StereoPlane> EglGbmLayer::stereoPlanes() const
{
    if (m_type != OutputLayerType::Primary) {
        return {};
    }
    switch (drmOutput()->activeStereoLayout()) {
    case StereoLayout::LDepth:
        return {StereoPlane::Depth};
    case StereoLayout::LDepthGraphics:
        return {StereoPlane::Depth, StereoPlane::Graphics, StereoPlane::GraphicsDepth};
    default:
        return {};
    }
}

std::optional<OutputLayerBeginFrameInfo> EglGbmLayer::beginStereoPlaneFrame(StereoPlane plane)
{
    return m_surface.startStereoPlane(plane);
}

bool EglGbmLayer::preparePresentationTest()
{
    if (m_type != OutputLayerType::Primary && drmOutput()->shouldDisableNonPrimaryPlanes()) {
        return false;
    }
    m_scanoutBuffer.reset();
    return m_surface.renderTestBuffer(scanoutSize(), supportedDrmFormats(), drmOutput()->nextState().colorPowerTradeoff, m_requiredAlphaBits) != nullptr;
}

static const auto s_allowHardwareRotation = environmentVariableBoolValue("KWIN_ENABLE_HW_ROTATION");
static const bool s_directScanoutDisabled = environmentVariableBoolValue("KWIN_DRM_NO_DIRECT_SCANOUT").value_or(false);

bool EglGbmLayer::earlyScanoutChecks()
{
    if (s_directScanoutDisabled) {
        return false;
    }
    if (m_type != OutputLayerType::Primary && drmOutput()->shouldDisableNonPrimaryPlanes()) {
        return false;
    }
    if (isFrameSequential(drmOutput()->activeStereoLayout())) {
        // Direct scanout cannot hold a stable pair while the primary plane
        // alternates its source rectangle.
        return false;
    }
    if (gpu()->needsModeset()) {
        // don't do direct scanout with modeset, it might lead to locking
        // the hardware to some buffer format we can't switch away from
        return false;
    }
    if (drmOutput()->replicatesEyes()) {
        // the scene comes from another output's eyes, not from a client's buffer
        return false;
    }
    if (drmOutput()->needsShadowBuffer()) {
        // while there are cases where this could still work (if the client prepares the buffer to match the output exactly)
        // it's likely not worth making this code more complicated to handle those edge cases
        return false;
    }
    if (!m_colorPipeline.isIdentity()) {
        if (!m_plane || drmOutput()->colorPowerTradeoff() == BackendOutput::ColorPowerTradeoff::PreferAccuracy) {
            return false;
        }
        const auto pipelines = m_plane->colorPipelines();
        const bool match = std::ranges::any_of(pipelines, [this](DrmColorOp *colorop) {
            return colorop->colorOp()->matchPipeline(gpu(), m_colorPipeline);
        });
        if (!match) {
            return false;
        }
    }
    // kernel documentation says that
    // "Devices that don’t support subpixel plane coordinates can ignore the fractional part."
    // so we need to make sure that doesn't cause a difference vs the composited result
    if (sourceRect() != sourceRect().toRect()) {
        return false;
    }
    if (offloadTransform() != OutputTransform::Kind::Normal) {
        // hardware rotation is broken on AMD GPUs that don't support modifiers,
        // see https://gitlab.freedesktop.org/drm/amd/-/work_items/5403
        if (!s_allowHardwareRotation.value_or(!gpu()->drmDevice()->isAmdgpu() || gpu()->addFB2ModifiersSupported())) {
            return false;
        }
        if (!m_plane || !m_plane->supportsTransformation(offloadTransform())) {
            return false;
        }
    }
    return true;
}

bool EglGbmLayer::importScanoutBuffer(GraphicsBuffer *buffer, const std::shared_ptr<OutputFrame> &frame)
{
    if (buffer->dmabufAttributes()->device != gpu()->drmDevice()->deviceId()
        && (!gpu()->renderDevice() || buffer->dmabufAttributes()->device != gpu()->renderDevice()->drmDevice()->deviceId())) {
        // Disallow direct scanout between GPUs, as
        // - there are some significant driver bugs with direct scanout from other GPUs,
        //   like https://gitlab.freedesktop.org/drm/amd/-/issues/2075
        // - with implicit modifiers, direct scanout on secondary GPUs
        //   is also very unlikely to yield the correct results.
        return false;
    }
    m_scanoutBuffer = gpu()->importBuffer(buffer, FileDescriptor{});
    if (m_scanoutBuffer) {
        m_surface.forgetDamage(); // TODO: Use absolute frame sequence numbers for indexing the DamageJournal. It's more flexible and less error-prone
    }
    return m_scanoutBuffer != nullptr;
}

std::shared_ptr<DrmFramebuffer> EglGbmLayer::currentBuffer() const
{
    return m_sharedBuffer ? m_sharedBuffer : (m_scanoutBuffer ? m_scanoutBuffer : m_surface.currentBuffer());
}

void EglGbmLayer::releaseBuffers()
{
    m_scanoutBuffer.reset();
    m_surface.destroyResources();
}

}
