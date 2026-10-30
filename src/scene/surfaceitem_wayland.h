/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "scene/surfaceitem.h"

#include <QTimer>
#include <unordered_map>

namespace KWin
{

class GraphicsBuffer;
class SubSurfaceInterface;
class SurfaceInterface;
class X11Window;

/**
 * The SurfaceItemWayland class represents a Wayland surface in the scene.
 */
class KWIN_EXPORT SurfaceItemWayland : public SurfaceItem
{
    Q_OBJECT

public:
    explicit SurfaceItemWayland(SurfaceInterface *surface, Item *parent = nullptr);

    RegionF shape() const override;
    RegionF opaque() const override;
    ContentType contentType() const override;
    void setScanoutHint(DrmDevice *device, const FormatModifierMap &drmFormats) override;
    void freeze() override;

    SurfaceInterface *surface() const;

private Q_SLOTS:
    void handleSurfaceCommitted();
    void handleSurfaceSizeChanged();
    void handleBufferChanged();
    void handleBufferSourceBoxChanged();
    void handleBufferTransformChanged();

    void handleChildSubSurfaceRemoved(SubSurfaceInterface *child);
    void handleChildSubSurfacesChanged();
    void handleSubSurfacePositionChanged();
    void handleSurfaceMappedChanged();
    void handleColorDescriptionChanged();
    void handlePresentationModeHintChanged();
    void handleReleasePointChanged();
    void handleAlphaMultiplierChanged();

private:
    SurfaceItemWayland *getOrCreateSubSurfaceItem(SubSurfaceInterface *s);
    void handleFramePainted(LogicalOutput *output, OutputFrame *frame, std::chrono::milliseconds timestamp) override;

    QPointer<SurfaceInterface> m_surface;
    struct ScanoutFeedback
    {
        DrmDevice *device = nullptr;
        FormatModifierMap formats;
    };
    std::optional<ScanoutFeedback> m_scanoutFeedback;
    std::unordered_map<SubSurfaceInterface *, std::unique_ptr<SurfaceItemWayland>> m_subsurfaces;
};

#if KWIN_BUILD_X11
/**
 * The SurfaceItemXwaylandChild class represents the surface of a child window that holds
 * stereo content, shown at the child's place in its X11 window.
 */
class KWIN_EXPORT SurfaceItemXwaylandChild : public SurfaceItemWayland
{
    Q_OBJECT

public:
    explicit SurfaceItemXwaylandChild(SurfaceInterface *surface, Item *parent = nullptr);

    void setChildGeometry(const RectF &geometry);
    void setChildShape(const RegionF &shape, const QSize &nativeSize);
    RegionF shape() const override;
    RegionF opaque() const override;
    void setMapped(bool mapped);

private:
    void updateDestinationSize();
    void updateVisibility();

    RegionF shapeForSourceBox(const RectF &bufferSourceBox) const override;

    RegionF m_nativeShape;
    QSize m_nativeSize;
    QSizeF m_childSize;
    bool m_mapped = false;
};

/**
 * The SurfaceItemXwayland class represents an Xwayland surface in the scene.
 */
class KWIN_EXPORT SurfaceItemXwayland : public SurfaceItemWayland
{
    Q_OBJECT

public:
    explicit SurfaceItemXwayland(X11Window *window, Item *parent = nullptr);

    RegionF opaque() const override;
    RegionF shape() const override;
    void freeze() override;

private:
    void handleShapeChange();
    void updateDestinationSize();
    void updateStereoChildren();

    X11Window *m_window;
    RegionF m_previousBufferShape;
    std::vector<std::unique_ptr<SurfaceItemXwaylandChild>> m_stereoChildren;
};
#endif

} // namespace KWin
