/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/surfaceitem_wayland.h"
#include "core/backendoutput.h"
#include "core/drmdevice.h"
#include "core/renderbackend.h"
#include "texture.h"
#include "wayland/linuxdmabufv1clientbuffer.h"
#include "wayland/subcompositor.h"
#include "wayland/surface.h"
#include "window.h"

#include <algorithm>

#if KWIN_BUILD_X11
#include "x11stereochildren.h"
#include "x11window.h"
#endif

namespace KWin
{

SurfaceItemWayland::SurfaceItemWayland(SurfaceInterface *surface, Item *parent)
    : SurfaceItem(parent)
    , m_surface(surface)
{
    connect(surface, &SurfaceInterface::sizeChanged,
            this, &SurfaceItemWayland::handleSurfaceSizeChanged);
    connect(surface, &SurfaceInterface::bufferChanged,
            this, &SurfaceItemWayland::handleBufferChanged);
    connect(surface, &SurfaceInterface::bufferSourceBoxChanged,
            this, &SurfaceItemWayland::handleBufferSourceBoxChanged);
    connect(surface, &SurfaceInterface::bufferTransformChanged,
            this, &SurfaceItemWayland::handleBufferTransformChanged);

    connect(surface, &SurfaceInterface::childSubSurfacesChanged,
            this, &SurfaceItemWayland::handleChildSubSurfacesChanged);
    connect(surface, &SurfaceInterface::committed,
            this, &SurfaceItemWayland::handleSurfaceCommitted);
    connect(surface, &SurfaceInterface::damaged,
            this, &SurfaceItemWayland::addDamage);
    connect(surface, &SurfaceInterface::childSubSurfaceRemoved,
            this, &SurfaceItemWayland::handleChildSubSurfaceRemoved);
    connect(surface, &SurfaceInterface::colorDescriptionChanged,
            this, &SurfaceItemWayland::handleColorDescriptionChanged);
    connect(surface, &SurfaceInterface::presentationModeHintChanged,
            this, &SurfaceItemWayland::handlePresentationModeHintChanged);
    connect(surface, &SurfaceInterface::bufferReleasePointChanged, this, &SurfaceItemWayland::handleReleasePointChanged);
    connect(surface, &SurfaceInterface::alphaMultiplierChanged, this, &SurfaceItemWayland::handleAlphaMultiplierChanged);

    connect(surface, &SurfaceInterface::mapped,
            this, &SurfaceItemWayland::handleSurfaceMappedChanged);
    connect(surface, &SurfaceInterface::unmapped,
            this, &SurfaceItemWayland::handleSurfaceMappedChanged);
    setVisible(surface->isMapped());

    SubSurfaceInterface *subsurface = surface->subSurface();
    if (subsurface) {
        setStereoContent(surface->stereoContent());
        connect(surface, &SurfaceInterface::stereoContentChanged, this, [this, surface] {
            setStereoContent(surface->stereoContent());
        });
        connect(subsurface, &SubSurfaceInterface::positionChanged,
                this, &SurfaceItemWayland::handleSubSurfacePositionChanged);
        setPosition(subsurface->position());
    }

    handleChildSubSurfacesChanged();
    setDestinationSize(surface->size());
    setBufferTransform(surface->bufferTransform());
    setBufferSourceBox(surface->bufferSourceBox());
    setBuffer(surface->buffer());
    m_bufferReleasePoint = m_surface->bufferReleasePoint();
    setColorDescription(surface->colorDescription());
    setRenderingIntent(surface->renderingIntent());
    setPresentationHint(surface->presentationModeHint());
    setOpacity(surface->alphaMultiplier());
}

RegionF SurfaceItemWayland::shape() const
{
    return RegionF{rect()};
}

RegionF SurfaceItemWayland::opaque() const
{
    if (m_surface) {
        return m_surface->opaque();
    }
    return RegionF();
}

SurfaceInterface *SurfaceItemWayland::surface() const
{
    return m_surface;
}

void SurfaceItemWayland::handleSurfaceSizeChanged()
{
    setDestinationSize(m_surface->size());
}

void SurfaceItemWayland::handleBufferChanged()
{
    setBuffer(m_surface->buffer());
}

void SurfaceItemWayland::handleBufferSourceBoxChanged()
{
    setBufferSourceBox(m_surface->bufferSourceBox());
}

void SurfaceItemWayland::handleBufferTransformChanged()
{
    setBufferTransform(m_surface->bufferTransform());
}

void SurfaceItemWayland::handleSurfaceCommitted()
{
    if (m_surface->hasFrameCallbacks() || m_surface->hasFifoBarrier() || m_surface->hasPresentationFeedback()) {
        scheduleFrame();
    }
}

SurfaceItemWayland *SurfaceItemWayland::getOrCreateSubSurfaceItem(SubSurfaceInterface *child)
{
    auto &item = m_subsurfaces[child];
    if (!item) {
        item = std::make_unique<SurfaceItemWayland>(child->surface(), this);
    }
    return item.get();
}

void SurfaceItemWayland::handleChildSubSurfaceRemoved(SubSurfaceInterface *child)
{
    m_subsurfaces.erase(child);
}

void SurfaceItemWayland::handleChildSubSurfacesChanged()
{
    const QList<SubSurfaceInterface *> below = m_surface->below();
    const QList<SubSurfaceInterface *> above = m_surface->above();

    for (int i = 0; i < below.count(); ++i) {
        SurfaceItemWayland *subsurfaceItem = getOrCreateSubSurfaceItem(below[i]);
        subsurfaceItem->setZ(i - below.count());
    }

    for (int i = 0; i < above.count(); ++i) {
        SurfaceItemWayland *subsurfaceItem = getOrCreateSubSurfaceItem(above[i]);
        subsurfaceItem->setZ(i);
    }
}

void SurfaceItemWayland::handleSubSurfacePositionChanged()
{
    setPosition(m_surface->subSurface()->position());
}

void SurfaceItemWayland::handleSurfaceMappedChanged()
{
    setVisible(m_surface->isMapped());
}

ContentType SurfaceItemWayland::contentType() const
{
    return m_surface ? m_surface->contentType() : ContentType::None;
}

void SurfaceItemWayland::setScanoutHint(DrmDevice *device, const FormatModifierMap &drmFormats)
{
    if (!m_surface || !m_surface->dmabufFeedbackV1()) {
        return;
    }
    if (!device && m_scanoutFeedback.has_value()) {
        m_surface->dmabufFeedbackV1()->setScanoutTranches({});
        m_scanoutFeedback.reset();
        return;
    }
    if (!m_scanoutFeedback || m_scanoutFeedback->device != device || m_scanoutFeedback->formats != drmFormats) {
        m_scanoutFeedback = ScanoutFeedback{
            .device = device,
            .formats = drmFormats,
        };
        m_surface->dmabufFeedbackV1()->setScanoutTranches(device, drmFormats);
    }
}

void SurfaceItemWayland::freeze()
{
    if (!m_surface) {
        return;
    }

    m_surface->disconnect(this);
    if (auto subsurface = m_surface->subSurface()) {
        subsurface->disconnect(this);
    }

    for (auto &[subsurface, subsurfaceItem] : m_subsurfaces) {
        subsurfaceItem->freeze();
    }

    m_surface = nullptr;
}

void SurfaceItemWayland::handleColorDescriptionChanged()
{
    auto description = m_surface->colorDescription();
    if (m_surface->colorDescriptionType() == ColorDescriptionType::Windows) {
        // TODO also react to config changes after the image description is set?
        const auto group = kwinApp()->config()->group("Windows_HDR");
        const double reference = group.readEntry("Reference", 203.0);
        description = description->withReference(reference);
        // the max average is intentionally ignored, since the calibration app doesn't set it (anymore)
        description = description->withHdrMetadata(reference, group.readEntry("MaxLuminance", 1'000));
    }
    setColorDescription(description);
    setRenderingIntent(m_surface->renderingIntent());
}

void SurfaceItemWayland::handlePresentationModeHintChanged()
{
    setPresentationHint(m_surface->presentationModeHint());
}

void SurfaceItemWayland::handleReleasePointChanged()
{
    m_bufferReleasePoint = m_surface->bufferReleasePoint();
    if (m_texture) {
        m_texture->setReleasePoint(m_bufferReleasePoint);
    }
}

void SurfaceItemWayland::handleAlphaMultiplierChanged()
{
    setOpacity(m_surface->alphaMultiplier());
}

void SurfaceItemWayland::handleFramePainted(LogicalOutput *output, OutputFrame *frame, std::chrono::milliseconds timestamp)
{
    if (!m_surface) {
        return;
    }
    m_surface->frameRendered(timestamp.count());
    if (frame) {
        // FIXME make frame always valid
        if (auto feedback = m_surface->presentationFeedback(output)) {
            frame->addFeedback(std::move(feedback));
        }
    }
    // TODO only call this once per refresh cycle
    m_surface->clearFifoBarrier(output ? std::optional(std::chrono::nanoseconds(1'000'000'000'000) / output->refreshRate()) : std::nullopt);
}

#if KWIN_BUILD_X11
SurfaceItemXwaylandChild::SurfaceItemXwaylandChild(SurfaceInterface *surface, Item *parent)
    : SurfaceItemWayland(surface, parent)
{
    // after SurfaceItemWayland's own handlers: the child's place decides the size and visibility
    connect(surface, &SurfaceInterface::sizeChanged, this, &SurfaceItemXwaylandChild::updateDestinationSize);
    connect(surface, &SurfaceInterface::mapped, this, &SurfaceItemXwaylandChild::updateVisibility);
    connect(surface, &SurfaceInterface::unmapped, this, &SurfaceItemXwaylandChild::updateVisibility);
    updateVisibility();
}

// The child window's place in its X11 window. Its buffer can be larger than the child, as it is
// twice the child's width when it holds both eyes at full size; it is drawn at the child's size.
void SurfaceItemXwaylandChild::setChildGeometry(const RectF &geometry)
{
    setPosition(geometry.topLeft());
    m_childSize = geometry.size();
    updateDestinationSize();
}

void SurfaceItemXwaylandChild::setChildShape(const RegionF &shape, const QSize &nativeSize)
{
    if (m_nativeShape == shape && m_nativeSize == nativeSize) {
        return;
    }
    m_nativeShape = shape;
    m_nativeSize = nativeSize;
    discardQuads();
    // Both the uncovered ordinary UI and the new shape need repainting.
    scheduleRepaint(rect());
}

RegionF SurfaceItemXwaylandChild::shapeForSourceBox(const RectF &sourceBox) const
{
    if (sourceBox.isEmpty() || m_nativeSize.isEmpty()) {
        return {};
    }
    RegionF nativeShape = m_nativeShape;
    // The GLX version-4 route keeps a one-eye X window and a twice-wide backing
    // pixmap. Its X shape applies to both views. A packed-raster child instead
    // has native coordinates spanning both eyes, including independent holes.
    if (stereoContent() != StereoContentNone
        && bufferSize() == QSize(2 * m_nativeSize.width(), m_nativeSize.height())) {
        nativeShape |= m_nativeShape.translated(m_nativeSize.width(), 0);
    }
    return nativeShape.intersected(sourceBox)
        .translated(-sourceBox.topLeft())
        .scaled(destinationSize().width() / sourceBox.width(), destinationSize().height() / sourceBox.height());
}

RegionF SurfaceItemXwaylandChild::shape() const
{
    if (stereoContent() == StereoContentNone) {
        return shapeForSourceBox(bufferSourceBox());
    }
    RectF left = bufferSourceBox();
    left.setWidth(left.width() / 2);
    const RectF right = left.translated(left.width(), 0);
    return shapeForSourceBox(left) | shapeForSourceBox(right);
}

RegionF SurfaceItemXwaylandChild::opaque() const
{
    // Coverage may differ between eyes. Do not occlude the ordinary owner
    // behind a hole using the other eye's opaque region.
    return {};
}

void SurfaceItemXwaylandChild::setMapped(bool mapped)
{
    m_mapped = mapped;
    updateVisibility();
}

void SurfaceItemXwaylandChild::updateDestinationSize()
{
    if (!m_childSize.isEmpty()) {
        setDestinationSize(m_childSize);
    }
}

void SurfaceItemXwaylandChild::updateVisibility()
{
    setVisible(m_mapped && surface() && surface()->isMapped());
}

SurfaceItemXwayland::SurfaceItemXwayland(X11Window *window, Item *parent)
    : SurfaceItemWayland(window->surface(), parent)
    , m_window(window)
{
    connect(window, &X11Window::shapeChanged, this, &SurfaceItemXwayland::handleShapeChange);
    connect(window->surface(), &SurfaceInterface::sizeChanged, this, &SurfaceItemXwayland::updateDestinationSize);
    connect(window, &X11Window::bufferGeometryChanged, this, &SurfaceItemXwayland::updateDestinationSize);
    connect(window, &X11Window::stereoContentChanged, this, &SurfaceItemXwayland::updateDestinationSize);
    connect(window, &X11Window::stereoChildrenChanged, this, &SurfaceItemXwayland::updateStereoChildren);
    updateDestinationSize();
    updateStereoChildren();
}

// Declared children keep their native buffers and independent place relative to the parent,
// one view for each eye; the rest of the window is the same in both eyes.
void SurfaceItemXwayland::updateStereoChildren()
{
    std::vector<std::unique_ptr<SurfaceItemXwaylandChild>> items;
    if (const X11StereoChildren *children = m_window->stereoChildren()) {
        for (const auto &child : children->children()) {
            if (!child->surface) {
                continue;
            }
            std::unique_ptr<SurfaceItemXwaylandChild> item;
            auto it = std::ranges::find_if(m_stereoChildren, [&child](const std::unique_ptr<SurfaceItemXwaylandChild> &item) {
                return item && item->surface() == child->surface;
            });
            if (it != m_stereoChildren.end()) {
                item = std::move(*it);
            } else {
                item = std::make_unique<SurfaceItemXwaylandChild>(child->surface, this);
            }
            item->setChildGeometry(child->place());
            item->setStereoContent(child->content);
            item->setChildShape(child->shape, child->nativeSize);
            item->setMapped(child->mapped);
            item->setZ(child->belowParent ? int(items.size()) - int(children->children().size()) : int(items.size()) + 1);
            items.push_back(std::move(item));
        }
    }
    m_stereoChildren = std::move(items);
}

void SurfaceItemXwayland::freeze()
{
    disconnect(m_window, &X11Window::stereoChildrenChanged, this, &SurfaceItemXwayland::updateStereoChildren);
    for (const auto &item : m_stereoChildren) {
        item->freeze();
    }
    SurfaceItemWayland::freeze();
}

// A full-resolution stereo window's X11 window is larger than its place on screen: its buffer
// is drawn at the window's size, as Xwayland does for an emulated resolution.
void SurfaceItemXwayland::updateDestinationSize()
{
    if (m_window->stereoClientScale() != QSizeF(1, 1)) {
        setDestinationSize(m_window->bufferGeometry().size());
    } else if (surface()) {
        setDestinationSize(surface()->size());
    }
}

void SurfaceItemXwayland::handleShapeChange()
{
    const auto newShape = m_window->shapeRegion();
    scheduleRepaint(newShape.xored(m_previousBufferShape));
    m_previousBufferShape = newShape;
    discardQuads();
}

RegionF SurfaceItemXwayland::shape() const
{
    return m_window->shapeRegion() & rect();
}

RegionF SurfaceItemXwayland::opaque() const
{
    if (!m_window->hasAlpha()) {
        return shape();
    } else {
        return m_window->opaqueRegion() & shape();
    }
    return RegionF();
}
#endif

} // namespace KWin

#include "moc_surfaceitem_wayland.cpp"
