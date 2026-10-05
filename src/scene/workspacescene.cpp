/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
 Design:

 When compositing is turned on, XComposite extension is used to redirect
 drawing of windows to pixmaps and XDamage extension is used to get informed
 about damage (changes) to window contents. This code is mostly in composite.cpp .

 Compositor::performCompositing() starts one painting pass. Painting is done
 by painting the screen, which in turn paints every window. Painting can be affected
 using effects, which are chained. E.g. painting a screen means that actually
 paintScreen() of the first effect is called, which possibly does modifications
 and calls next effect's paintScreen() and so on, until Scene::finalPaintScreen()
 is called.

 There are 3 phases of every paint (not necessarily done together):
 The pre-paint phase, the paint phase and the post-paint phase.

 The pre-paint phase is used to find out about how the painting will be actually
 done (i.e. what the effects will do). For example when only a part of the screen
 needs to be updated and no effect will do any transformation it is possible to use
 an optimized paint function. How the painting will be done is controlled
 by the mask argument, see PAINT_WINDOW_* and PAINT_SCREEN_* flags in scene.h .
 For example an effect that decides to paint a normal windows as translucent
 will need to modify the mask in its prePaintWindow() to include
 the PAINT_WINDOW_TRANSLUCENT flag. The paintWindow() function will then get
 the mask with this flag turned on and will also paint using transparency.

 The paint pass does the actual painting, based on the information collected
 using the pre-paint pass. After running through the effects' paintScreen()
 either paintGenericScreen() or optimized paintSimpleScreen() are called.
 Those call paintWindow() on windows (not necessarily all), possibly using
 clipping to optimize performance and calling paintWindow() first with only
 PAINT_WINDOW_OPAQUE to paint the opaque parts and then later
 with PAINT_WINDOW_TRANSLUCENT to paint the transparent parts. Function
 paintWindow() again goes through effects' paintWindow() until
 finalPaintWindow() is called, which calls the window's performPaint() to
 do the actual painting.

 The post-paint can be used for cleanups and is also used for scheduling
 repaints during the next painting pass for animations. Effects wanting to
 repaint certain parts can manually damage them during post-paint and repaint
 of these parts will be done during the next paint pass.

*/

#include "scene/workspacescene.h"
#include "compositor.h"
#include "core/backendoutput.h"
#include "core/graphicsbufferview.h"
#include "core/output.h"
#include "core/pixelgrid.h"
#include "core/renderbackend.h"
#include "core/renderloop.h"
#include "core/renderviewport.h"
#include "cursoritem.h"
#include "effect/effecthandler.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "options.h"
#include "scene/backgroundeffectitem.h"
#include "scene/decorationitem.h"
#include "scene/dndiconitem.h"
#include "scene/itemrenderer.h"
#include "scene/rootitem.h"
#include "scene/shadowitem.h"
#include "scene/stereodepth.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "utils/envvar.h"
#include "wayland/seat.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <QtMath>

#include <algorithm>

namespace KWin
{

//****************************************
// Scene
//****************************************

WorkspaceScene::WorkspaceScene()
    : m_containerItem(std::make_unique<RootItem>(this))
    , m_overlayItem(std::make_unique<RootItem>(this))
    , m_cursorItem(std::make_unique<CursorItem>(m_overlayItem.get()))
{
    setGeometry(workspace()->geometry());
    connect(workspace(), &Workspace::geometryChanged, this, [this]() {
        setGeometry(workspace()->geometry());
    });

    // the levels of the stereo depth follow what is active, stacked and shown
    connect(options, &Options::stereoDepthChanged, this, &WorkspaceScene::repaintStereoDepth);
    connect(workspace(), &Workspace::windowActivated, this, &WorkspaceScene::repaintStereoDepth);
    connect(workspace(), &Workspace::stackingOrderChanged, this, &WorkspaceScene::repaintStereoDepth);
    connect(workspace(), &Workspace::windowRemoved, this, &WorkspaceScene::repaintStereoDepth);
    connect(workspace(), &Workspace::currentDesktopChanged, this, &WorkspaceScene::repaintStereoDepth);
    connect(workspace(), &Workspace::windowAdded, this, [this](Window *window) {
        connect(window, &Window::minimizedChanged, this, &WorkspaceScene::repaintStereoDepth);
        connect(window, &Window::transientChanged, this, &WorkspaceScene::repaintStereoDepth);
        connect(window, &Window::fullScreenChanged, this, &WorkspaceScene::repaintStereoDepth);
    });

    connect(waylandServer()->seat(), &SeatInterface::dragStarted, this, &WorkspaceScene::createDndIconItem);
    connect(waylandServer()->seat(), &SeatInterface::dragEnded, this, &WorkspaceScene::destroyDndIconItem);

    // make sure it's over the dnd icon
    m_cursorItem->setZ(1);
    connect(Cursors::self(), &Cursors::hiddenChanged, this, &WorkspaceScene::updateCursor);
    connect(Cursors::self(), &Cursors::positionChanged, this, &WorkspaceScene::updateCursor);
    updateCursor();
}

WorkspaceScene::~WorkspaceScene()
{
}

void WorkspaceScene::attachRenderer(std::unique_ptr<ItemRenderer> &&renderer)
{
    m_renderer = std::move(renderer);
    if (m_renderer) {
        m_renderer->setLayerDebugging(m_layerDebugging);
    }
}

void WorkspaceScene::detachRenderer()
{
    releaseResources(m_containerItem.get());
    releaseResources(m_overlayItem.get());
    releaseResources(m_cursorItem.get());

    m_renderer.reset();
}

void WorkspaceScene::createDndIconItem()
{
    DragAndDropIcon *dragIcon = waylandServer()->seat()->dragIcon();
    if (!dragIcon) {
        return;
    }
    m_dndIcon = std::make_unique<DragAndDropIconItem>(dragIcon, m_overlayItem.get());

    auto updatePosition = [this]() {
        const auto position = waylandServer()->seat()->dragPosition();
        m_dndIcon->setPosition(position);
        m_dndIcon->setOutput(workspace()->outputAt(position));
    };

    updatePosition();
    connect(waylandServer()->seat(), &SeatInterface::dragMoved, m_dndIcon.get(), updatePosition);
}

void WorkspaceScene::destroyDndIconItem()
{
    m_dndIcon.reset();
}

void WorkspaceScene::updateCursor()
{
    if (Cursors::self()->isCursorHidden()) {
        m_cursorItem->setVisible(false);
    } else {
        m_cursorItem->setVisible(true);
        m_cursorItem->setPosition(Cursors::self()->currentCursor()->pos());
    }
}

Item *WorkspaceScene::containerItem() const
{
    return m_containerItem.get();
}

Item *WorkspaceScene::overlayItem() const
{
    return m_overlayItem.get();
}

Item *WorkspaceScene::cursorItem() const
{
    return m_cursorItem.get();
}

struct ClipCorner
{
    RectF box;
    BorderRadius radius;
};

static std::optional<ClipCorner> calculateClipCorner(Item *item, const std::optional<ClipCorner> &parentClip)
{
    if (!item->borderRadius().isNull()) {
        return ClipCorner{
            .box = item->rect(),
            .radius = item->borderRadius(),
        };
    } else if (parentClip.has_value()) {
        return ClipCorner{
            .box = item->transform().inverted().mapRect(parentClip->box.translated(-item->position())),
            .radius = parentClip->radius,
        };
    } else {
        return std::nullopt;
    }
}

static void maybePushCorners(Item *item, QStack<ClipCorner> &corners)
{
    std::optional<ClipCorner> top;
    if (!corners.isEmpty()) {
        top = corners.top();
    }
    if (const auto clip = calculateClipCorner(item, top)) {
        corners.push(*clip);
    }
}

static bool checkForBlackBackground(Item *background)
{
    SurfaceItem *surface = qobject_cast<SurfaceItem *>(background);
    if (!surface) {
        return false;
    }
    if (!surface->buffer()
        || (!surface->buffer()->singlePixelAttributes() && !surface->buffer()->shmAttributes())
        || surface->buffer()->size() != QSize(1, 1)) {
        return false;
    }
    const GraphicsBufferView view(surface->buffer());
    if (!view.image()) {
        return false;
    }
    const QRgb rgb = view.image()->pixel(0, 0);
    const QVector3D encoded(qRed(rgb) / 255.0, qGreen(rgb) / 255.0, qBlue(rgb) / 255.0);
    const QVector3D nits = surface->colorDescription()->mapTo(encoded, ColorDescription(Colorimetry::BT709, TransferFunction(TransferFunction::linear), 100, 0, std::nullopt, std::nullopt), surface->renderingIntent());
    // below 0.1 nits, it shouldn't be noticeable that we replace it with black
    return nits.lengthSquared() <= (0.1 * 0.1);
}

static Rect mapToDevice(SceneView *view, Item *item, const RectF &itemLocal)
{
    const RectF localLogical = item->mapToView(itemLocal, view).translated(-view->viewport().topLeft());
    return localLogical.scaled(view->scale()).rounded();
}

static Region mapToDevice(SceneView *view, Item *item, const RegionF &itemLocal)
{
    Region ret;
    for (const RectF &local : itemLocal.rects()) {
        ret |= mapToDevice(view, item, local);
    }
    return ret;
}

static bool isCandidate(SurfaceItem *item, const Rect &deviceRect, bool isOpaque, SceneView *view)
{
    if (!item || !item->buffer() || !item->buffer()->dmabufAttributes()) {
        return false;
    }
    // TODO make the compositor handle item opacity as well
    if (item->opacity() < 1.0) {
        return false;
    }
    const bool updatesQuickly = item->frameTimeEstimation().transform([](const auto t) {
        return t < std::chrono::nanoseconds(1'000'000'000) / 20;
    }).value_or(false);
    if (updatesQuickly) {
        return true;
    }
    // fullscreen with low enough bandwidth requirements are always good to scanout
    const auto info = FormatInfo::get(item->buffer()->dmabufAttributes()->format);
    if (!info) {
        return false;
    }
    return isOpaque && info->bitsPerPixel <= 32 && deviceRect.contains(view->deviceRect());
}

/**
 * This returns
 * - true if the search is complete and the visible parts of
          the scene are fully represented in underlays and overlays,
 * - std::nullopt if it can still be continued,
 * - false if the search failed and should be stopped
 */
static std::optional<bool> findOverlayCandidates(SceneView *view, Item *item, ssize_t maxTotalCount,
                                                 Region &occupied, Region &opaque, Region &effected,
                                                 QList<Item *> &overlays, QList<Item *> &underlays,
                                                 QStack<ClipCorner> &corners, bool &needsCompositedScene)
{
    if (opaque.contains(view->deviceRect())) {
        return true;
    }
    if (!item || !item->isVisible() || item->opacity() == 0.0 || item->boundingRect().isEmpty()
        || !view->viewport().intersects(item->mapToView(item->boundingRect(), view))) {
        return std::nullopt;
    }
    if (item->hasEffects()) {
        // can't put this item, any children on items below this one
        // on an overlay/underlay, as we don't know what the effect does
        effected += mapToDevice(view, item, item->boundingRect());
        needsCompositedScene = true;
        return std::nullopt;
    }
    maybePushCorners(item, corners);
    auto cleanupCorners = qScopeGuard([&corners]() {
        if (!corners.isEmpty()) {
            corners.pop();
        }
    });

    const QList<Item *> children = item->sortedChildItems();
    auto it = children.rbegin();
    for (; it != children.rend(); it++) {
        Item *const child = *it;
        if (child->z() < 0) {
            break;
        }
        if (auto ret = findOverlayCandidates(view, child, maxTotalCount, occupied, opaque, effected, overlays, underlays, corners, needsCompositedScene)) {
            return ret;
        }
    }

    const Rect deviceRect = mapToDevice(view, item, item->rect()) & view->deviceRect();
    // if the item is empty or completely covered, ignore it
    if (!deviceRect.isEmpty() && !opaque.contains(deviceRect)) {
        // for the Item to be possibly relevant for over- or underlays, it needs to
        // - be a SurfaceItem (for now at least)
        // - use dmabufs
        // - regularly get updates
        // - not be covered by any effects
        // - not have any surface-wide opacity (for now)
        const Region deviceOpaque = mapToDevice(view, item, item->opaque());
        SurfaceItem *surfaceItem = dynamic_cast<SurfaceItem *>(item);
        const bool isOpaque = deviceOpaque.contains(deviceRect);
        if (!effected.intersects(deviceRect) && isCandidate(surfaceItem, deviceRect, isOpaque, view)) {
            if (occupied.intersects(deviceRect) || (!corners.isEmpty() && corners.top().radius.clips(item->rect(), corners.top().box))) {
                if (!isOpaque) {
                    // only fully opaque items can be used as underlays
                    return false;
                }
                underlays.push_back(surfaceItem);
            } else {
                overlays.push_back(surfaceItem);
            }
            // if the scene will need to be composited, we need a layer to put it on,
            // so the max total number of layers is effectively reduced by one
            const ssize_t effectiveMax = needsCompositedScene ? maxTotalCount - 1 : maxTotalCount;
            if (overlays.size() + underlays.size() > effectiveMax) {
                // If we have to repaint the primary plane anyways, it's not going to provide an efficiency
                // or latency improvement to put some but not all quickly updating surfaces on overlays,
                // at least not with the current way we use them.
                return false;
            }
        } else {
            occupied += deviceRect;
            if (!needsCompositedScene
                && deviceOpaque.contains(view->deviceRect())
                && checkForBlackBackground(item)) {
                opaque = deviceOpaque;
                // completely black background -> this item can be skipped,
                // and we don't need to consider any items below it either
                return true;
            }
            needsCompositedScene = true;
        }
        opaque += deviceOpaque;
        if (opaque.contains(view->deviceRect())) {
            return true;
        }
    }

    for (; it != children.rend(); it++) {
        Item *const child = *it;
        if (auto ret = findOverlayCandidates(view, child, maxTotalCount, occupied, opaque, effected, overlays, underlays, corners, needsCompositedScene)) {
            return ret;
        }
    }
    return std::nullopt;
}

static const bool s_forceSoftwareCursor = environmentVariableBoolValue("KWIN_FORCE_SW_CURSOR").value_or(false);

QList<Item *> WorkspaceScene::layerCandidates(ssize_t maxTotalCount) const
{
    // This algorithm searches for candidates to put on their own layers.
    // These are the cases we care about:
    // - compositing. There are either no quickly updating items, or none
    //   are suitable for direct scanout.
    //   This case will return [containerItem]
    // - fullscreen direct scanout. One or more items cover the entire screen,
    //   and the scene itself does not have to be rendered at all.
    //   This case will return for example [overlay, overlay, overlay]
    // - overlay direct scanout. One or more items update quickly, and can be
    //   put above the scene.
    //   This case will return for example [containerItem, overlay, overlay]
    // - underlay direct scanout. One or more items update quickly and require
    //   compositing above them (because of a composited item on top, or rounded
    //   corners)
    //   This case will return for example [underlay, underlay, containerItem]
    // - mixed underlay and overlay direct scanout.
    //   This case will return for example [underlay, containerItem, overlay]
    //
    // One notable special case is that the cursor is (currently) always treated
    // as the highest priority item for an overlay, since its responsiveness
    // is especially noticeable to users.

    if (effects->blocksDirectScanout()) {
        return {containerItem()};
    }

    const auto fallback = [this, maxTotalCount]() {
        QList<Item *> ret;
        if (maxTotalCount > 1
            && !s_forceSoftwareCursor
            && cursorItem()->isVisible()
            && painted_delegate->viewport().intersects(cursorItem()->mapToView(cursorItem()->boundingRect(), painted_delegate))) {
            ret.push_back(cursorItem());
        }
        ret.push_back(containerItem());
        return ret;
    };
    Region occupied;
    Region opaque;
    Region effected;
    QList<Item *> overlays;
    QList<Item *> underlays;
    QStack<ClipCorner> cornerStack;
    bool needsCompositedScene = false;
    std::optional<bool> result;

    const auto overlayItems = m_overlayItem->sortedChildItems();
    for (Item *item : overlayItems | std::views::reverse) {
        if (!item->isVisible() || !painted_delegate->viewport().intersects(item->mapToView(item->boundingRect(), painted_delegate))) {
            continue;
        }
        if (item == cursorItem()) {
            if (s_forceSoftwareCursor) {
                needsCompositedScene = true;
                occupied |= mapToDevice(painted_delegate, item, item->boundingRect()) & painted_delegate->deviceRect();
                continue;
            }
            // for the time being, prioritize the cursor above all else,
            // even while it's not moving
            overlays.push_back(item);
            // if the scene will need to be composited, we need a layer to put it on,
            // so the max total number of layers is effectively reduced by one
            const ssize_t effectiveMax = needsCompositedScene ? maxTotalCount - 1 : maxTotalCount;
            if (underlays.size() + overlays.size() > effectiveMax) {
                return fallback();
            }
            continue;
        }
        result = findOverlayCandidates(painted_delegate, item, maxTotalCount, occupied, opaque, effected, overlays, underlays, cornerStack, needsCompositedScene);
        if (result.has_value()) {
            if (*result) {
                break;
            } else {
                return fallback();
            }
        }
    }

    if (!result) {
        result = findOverlayCandidates(painted_delegate, m_containerItem.get(), maxTotalCount, occupied, opaque, effected, overlays, underlays, cornerStack, needsCompositedScene);
        if (result.has_value() && !*result) {
            return fallback();
        }
    }

    // NOTE the return list needs to be sorted top to bottom
    QList<Item *> ret;
    ret.reserve(underlays.size() + 1 + overlays.size());
    std::ranges::copy(overlays, std::back_inserter(ret));
    if (needsCompositedScene || (underlays.empty() && overlays.empty())) {
        ret.push_back(containerItem());
    }
    std::ranges::copy(underlays, std::back_inserter(ret));
    return ret;
}

static double getDesiredHdrHeadroom(Item *item)
{
    if (!item->isVisible()) {
        return 1;
    }
    double ret = 1;
    const auto children = item->childItems();
    for (const auto &child : children) {
        ret = std::max(ret, getDesiredHdrHeadroom(child));
    }
    const auto &color = item->colorDescription();
    if (color->maxHdrLuminance() && *color->maxHdrLuminance() > color->referenceLuminance()) {
        return std::max(ret, *color->maxHdrLuminance() / color->referenceLuminance());
    } else {
        return ret;
    }
}

double WorkspaceScene::desiredHdrHeadroom() const
{
    double maxHeadroom = 1;
    for (const auto &item : stacking_order) {
        if (!item->window()->frameGeometry().intersects(painted_delegate->viewport())) {
            continue;
        }
        maxHeadroom = std::max(maxHeadroom, getDesiredHdrHeadroom(item));
    }
    return maxHeadroom;
}

void WorkspaceScene::frame(SceneView *delegate, OutputFrame *frame)
{
    LogicalOutput *logicalOutput = delegate->logicalOutput();
    const auto frameTime = std::chrono::duration_cast<std::chrono::milliseconds>(logicalOutput->backendOutput()->renderLoop()->lastPresentationTimestamp());
    m_containerItem->framePainted(delegate, logicalOutput, frame, frameTime);
    if (m_overlayItem) {
        m_overlayItem->framePainted(delegate, logicalOutput, frame, frameTime);
    }
}

void WorkspaceScene::prePaint(SceneView *delegate, OutputFrame *frame)
{
    painted_delegate = delegate;
    painted_screen = painted_delegate->logicalOutput();

    createStackingOrder();

    // preparation step
    effects->startPaint();

    ScreenPrePaintData prePaintData;
    prePaintData.mask = 0;
    prePaintData.screen = painted_screen;
    prePaintData.view = delegate;
    prePaintData.frame = frame;

    effects->makeOpenGLContextCurrent();
    Q_EMIT preFrameRender();

    effects->prePaintScreen(prePaintData);
    m_paintContext.deviceDamage = painted_delegate->mapToDeviceCoordinatesAligned(prePaintData.paint) & painted_delegate->deviceRect();
    m_paintContext.mask = prePaintData.mask;
    m_paintContext.phase2Data.clear();

    if (m_paintContext.mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) {
        preparePaintGenericScreen();
    } else {
        preparePaintSimpleScreen();
    }
}

static void resetRepaintsHelper(Item *item, SceneView *delegate)
{
    if (delegate->shouldRenderItem(item)) {
        item->resetRepaints(delegate);
    }

    const auto childItems = item->childItems();
    for (Item *childItem : childItems) {
        resetRepaintsHelper(childItem, delegate);
    }
}

static void accumulateRepaints(Item *item, SceneView *view, Region *windowRepaints, Region *accumulatedRepaints, Region *forceTranslucent)
{
    const auto childItems = item->sortedChildItems();
    auto childIt = childItems.begin();
    for (; childIt != childItems.end() && (*childIt)->z() < 0; childIt++) {
        accumulateRepaints(*childIt, view, windowRepaints, accumulatedRepaints, forceTranslucent);
    }
    if (auto background = qobject_cast<BackgroundEffectItem *>(item)) {
        const Rect viewRect = view->mapToDeviceCoordinates(item->mapToView(item->rect(), view)).rounded();
        if (accumulatedRepaints->intersects(viewRect)) {
            *windowRepaints |= viewRect;
            *accumulatedRepaints |= viewRect;
            if (uint32_t pixels = background->pixelsToExpandRepaintsBelowOpaqueRegions()) {
                *forceTranslucent |= accumulatedRepaints->grownBy(QMargins(pixels, pixels, pixels, pixels)) & viewRect;
            }
        }
    } else if (view->shouldRenderItem(item)) {
        const Region repaints = item->takeDeviceRepaints(view);
        *windowRepaints |= repaints;
        *accumulatedRepaints |= repaints;
    }

    for (; childIt != childItems.end(); childIt++) {
        accumulateRepaints(*childIt, view, windowRepaints, accumulatedRepaints, forceTranslucent);
    }
}

void WorkspaceScene::preparePaintGenericScreen()
{
    for (WindowItem *windowItem : std::as_const(stacking_order)) {
        resetRepaintsHelper(windowItem, painted_delegate);

        WindowPrePaintData data;
        data.mask = m_paintContext.mask;

        effects->prePaintWindow(painted_delegate, windowItem->effectWindow(), data);
        m_paintContext.phase2Data.append(Phase2Data{
            .item = windowItem,
            .deviceRegion = Region::infinite(),
            .deviceOpaque = Region{},
            .mask = data.mask,
        });
    }
}

static void addOpaqueRegionRecursive(SceneView *view, Item *item, const std::optional<ClipCorner> &parentCorner, Region &ret)
{
    const std::optional<ClipCorner> corner = calculateClipCorner(item, parentCorner);
    RegionF opaque = item->opaque();
    if (corner.has_value()) {
        opaque = corner->radius.clip(item->opaque(), corner->box);
    }
    const Rect deviceRect = snapToPixelGrid(view->mapToDeviceCoordinates(item->mapToView(item->rect(), view)));
    for (const RectF &rect : opaque.rects()) {
        ret |= snapToPixelGrid(view->mapToDeviceCoordinates(item->mapToView(rect, view))) & deviceRect;
    }
    const auto children = item->childItems();
    for (Item *child : children) {
        addOpaqueRegionRecursive(view, child, corner, ret);
    }
}

void WorkspaceScene::preparePaintSimpleScreen()
{
    for (WindowItem *windowItem : std::as_const(stacking_order)) {
        Window *window = windowItem->window();
        WindowPrePaintData data;
        data.mask = m_paintContext.mask;

        effects->prePaintWindow(painted_delegate, windowItem->effectWindow(), data);

        if (stereoDepthPainted(window)) {
            // each eye shows the window at its own place, and its shadow at the places of what it falls on
            data.mask |= PAINT_WINDOW_TRANSFORMED;
        }

        Region opaque;
        if (window->opacity() == 1.0 && !(data.mask & PAINT_WINDOW_TRANSLUCENT)) {
            addOpaqueRegionRecursive(painted_delegate, windowItem, std::nullopt, opaque);
        }
        m_paintContext.phase2Data.append(Phase2Data{
            .item = windowItem,
            .deviceRegion = Region{},
            .deviceOpaque = std::move(opaque),
            .mask = data.mask,
        });
    }
}

Region WorkspaceScene::collectDamage()
{
    if (m_paintContext.mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) {
        resetRepaintsHelper(m_overlayItem.get(), painted_delegate);
        m_paintContext.deviceDamage = painted_delegate->deviceRect();
        return m_paintContext.deviceDamage;
    } else {
        // collect all damage, from bottom to top
        Region accumulatedRepaints;
        Region forceTranslucent;
        for (auto &data : m_paintContext.phase2Data) {
            data.deviceOpaque -= forceTranslucent;
            accumulateRepaints(data.item, painted_delegate, &data.deviceRegion, &accumulatedRepaints, &forceTranslucent);
        }
        accumulateRepaints(m_overlayItem.get(), painted_delegate, &m_paintContext.deviceDamage, &accumulatedRepaints, &forceTranslucent);

        // Perform an occlusion cull pass, to remove surface damage occluded by opaque windows.
        Region opaque;
        addOpaqueRegionRecursive(painted_delegate, m_overlayItem.get(), std::nullopt, opaque);
        for (auto &paintData : m_paintContext.phase2Data | std::views::reverse) {
            m_paintContext.deviceDamage |= paintData.deviceRegion - opaque;

            // TODO make occlusion culling per item, rather than per window
            const bool canCover = painted_delegate->shouldRenderItem(paintData.item->surfaceItem())
                || painted_delegate->shouldRenderHole(paintData.item->surfaceItem());
            if (!(paintData.mask & (PAINT_WINDOW_TRANSLUCENT | PAINT_WINDOW_TRANSFORMED)) && canCover) {
                opaque += paintData.deviceOpaque;
            }
        }

        if (painted_screen && painted_screen->hasStereoEyes() && StereoDepth::isEnabled()) {
            // what is damaged is drawn at its level in each eye, a little to either side
            const int shift = StereoDepth::maxEyeShift(painted_delegate->deviceRect().width());
            m_paintContext.deviceDamage = m_paintContext.deviceDamage.grownBy(QMargins(shift, 0, shift, 0));
        }
        return m_paintContext.deviceDamage & painted_delegate->deviceRect();
    }
}

void WorkspaceScene::postPaint()
{
    effects->postPaintScreen();

    painted_delegate = nullptr;
    painted_screen = nullptr;
    clearStackingOrder();
}

void WorkspaceScene::paint(const RenderTarget &renderTarget, const QPoint &deviceOffset, const Region &deviceRegion)
{
    RenderViewport viewport(painted_delegate->viewport(), painted_delegate->scale(), renderTarget, deviceOffset);

    m_renderer->beginFrame(renderTarget, viewport);

    effects->paintScreen(renderTarget, viewport, m_paintContext.mask, deviceRegion, painted_screen);

    Q_EMIT frameRendered();
    m_renderer->endFrame();
}

// the function that'll be eventually called by paintScreen() above
void WorkspaceScene::finalPaintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    if (mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) {
        paintGenericScreen(renderTarget, viewport, mask, screen);
    } else {
        paintSimpleScreen(renderTarget, viewport, mask, deviceRegion);
    }
}

// The generic painting code that can handle even transformations.
// It simply paints bottom-to-top.
void WorkspaceScene::paintGenericScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int, LogicalOutput *screen)
{
    m_renderer->renderBackground(renderTarget, viewport, Region::infinite());

    for (const Phase2Data &paintData : std::as_const(m_paintContext.phase2Data)) {
        paintWindow(renderTarget, viewport, paintData.item, paintData.mask, paintData.deviceRegion);
    }

    const Rect bounds = viewport.mapToDeviceCoordinates(m_overlayItem->mapToScene(m_overlayItem->boundingRect())).toRect();
    m_renderer->renderItem(renderTarget, viewport, m_overlayItem.get(), PAINT_SCREEN_TRANSFORMED, bounds, WindowPaintData{}, [this](Item *item) {
        return !painted_delegate->shouldRenderItem(item);
    }, [this](Item *item) {
        return painted_delegate->shouldRenderHole(item);
    });
}

// The optimized case without any transformations at all.
// It can paint only the requested region and can use clipping
// to reduce painting and improve performance.
void WorkspaceScene::paintSimpleScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int, const Region &deviceRegion)
{
    // This is the occlusion culling pass
    Region visible = deviceRegion;
    for (int i = m_paintContext.phase2Data.size() - 1; i >= 0; --i) {
        Phase2Data *data = &m_paintContext.phase2Data[i];
        data->deviceRegion = visible & viewport.deviceRect();

        if (!(data->mask & PAINT_WINDOW_TRANSFORMED)) {
            data->deviceRegion &= viewport.mapToDeviceCoordinatesAligned(data->item->mapToScene(data->item->boundingRect()));

            // TODO change effects API, so occlusion culling is per item, rather than per window
            const bool canCover = painted_delegate->shouldRenderItem(data->item->surfaceItem())
                || painted_delegate->shouldRenderHole(data->item->surfaceItem());
            if (!(data->mask & PAINT_WINDOW_TRANSLUCENT) && canCover) {
                visible -= data->deviceOpaque;
            }
        }
    }

    m_renderer->renderBackground(renderTarget, viewport, visible);

    for (const Phase2Data &paintData : std::as_const(m_paintContext.phase2Data)) {
        paintWindow(renderTarget, viewport, paintData.item, paintData.mask, paintData.deviceRegion);
    }

    const Rect bounds = viewport.mapToDeviceCoordinates(m_overlayItem->mapToScene(m_overlayItem->boundingRect())).toRect();
    const Region deviceRepaint = deviceRegion & bounds;
    if (!deviceRepaint.isEmpty()) {
        m_renderer->renderItem(renderTarget, viewport, m_overlayItem.get(), PAINT_SCREEN_TRANSFORMED, deviceRepaint, WindowPaintData{}, [this](Item *item) {
            return !painted_delegate->shouldRenderItem(item);
        }, [this](Item *item) {
            return painted_delegate->shouldRenderHole(item);
        });
    }
}

void WorkspaceScene::createStackingOrder()
{
    QList<Item *> items = m_containerItem->sortedChildItems();
    for (Item *item : std::as_const(items)) {
        WindowItem *windowItem = static_cast<WindowItem *>(item);
        if (painted_delegate && painted_delegate->shouldHideWindow(windowItem->window())) {
            continue;
        }
        if (windowItem->isVisible()) {
            windowItem->window()->ref();
            stacking_order.append(windowItem);
        }
    }
}

void WorkspaceScene::clearStackingOrder()
{
    for (WindowItem *windowItem : std::as_const(stacking_order)) {
        windowItem->window()->unref();
    }
    stacking_order.clear();
}

void WorkspaceScene::paintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, WindowItem *item, int mask, const Region &deviceRegion)
{
    if (deviceRegion.isEmpty()) { // completely clipped
        return;
    }

    WindowPaintData data;
    EffectWindow *effectWindow = item->effectWindow();
    const QVariant forceBlur = effectWindow->data(WindowForceBlurRole);
    const StereoEye eye = m_renderer->stereoEye();
    const int parallax = eye == StereoEye::None ? 0 : stereoParallax(item->window());
    const int shift = StereoDepth::eyeShift(parallax, eye);
    const bool depthPainted = eye != StereoEye::None && stereoDepthPainted(item->window());
    if (depthPainted) {
        m_depthWindow = item->window();
        // blur follows a window that is moved by its level
        effectWindow->setData(WindowForceBlurRole, true);
    }
    if (shift) {
        // the window at its level: its copy in this eye, the other eye's the other way. The effects
        // get it with the window's paint data.
        data.translate(shift / viewport.scale());
    }
    if (shift && item->window()->isDesktop()) {
        // a background behind the screen reaches past the edges of the output in both eyes: wider
        // by what each eye's copy moves, around the middle, which keeps the level's parallax
        const int extra = 2 * ((std::abs(parallax) + 1) / 2);
        const qreal width = item->window()->frameGeometry().width() * viewport.scale();
        data.setXScale((width + extra) / width);
        data.translate(-extra / 2 / viewport.scale());
    }
    effects->paintWindow(renderTarget, viewport, effectWindow, mask, deviceRegion, data);
    if (depthPainted) {
        m_depthWindow = nullptr;
        effectWindow->setData(WindowForceBlurRole, forceBlur);
    }
}

// the function that'll be eventually called by paintWindow() above
void WorkspaceScene::finalPaintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    effects->drawWindow(renderTarget, viewport, w, mask, deviceRegion, data);
}

void WorkspaceScene::repaintStereoDepth()
{
    if (!StereoDepth::isEnabled()) {
        return;
    }
    const auto outputs = workspace()->outputs();
    if (std::ranges::any_of(outputs, [](LogicalOutput *output) {
            return output->hasStereoEyes();
        })) {
        addRepaintFull();
    }
}

bool WorkspaceScene::stereoDepthPainted(const Window *window) const
{
    return stereoParallax(window) != 0 || (painted_screen && painted_screen->hasStereoEyes() && StereoDepth::isEnabled() && window->shadow());
}

int WorkspaceScene::stereoParallax(const Window *window) const
{
    if (!painted_screen || !painted_screen->hasStereoEyes() || !StereoDepth::isEnabled()) {
        return 0;
    }
    return StereoDepth::parallax(window, painted_delegate->deviceRect().width());
}

// A shadow lies on what it falls on: drawn for each receiver, in the receiver's visible part, at the
// receiver's shift, so over something at the screen's level it has no parallax and over something
// sunk it has that thing's. The caster itself stays where its own level put it.
void WorkspaceScene::drawStereoShadow(const RenderTarget &renderTarget, const RenderViewport &viewport, WindowItem *caster, int mask, const Region &deviceRegion, const WindowPaintData &data)
{
    const StereoEye eye = m_renderer->stereoEye();
    ShadowItem *shadow = caster->shadowItem();
    const int casterShift = StereoDepth::eyeShift(stereoParallax(caster->window()), eye);
    const auto drawOn = [&](const Region &part, int shift) {
        WindowPaintData shadowData(data);
        shadowData.translate((shift - casterShift) / viewport.scale());
        m_renderer->renderItem(renderTarget, viewport, caster, mask, part, shadowData, [this, caster, shadow](Item *item) {
            return (item != caster && item != shadow) || (painted_delegate && !painted_delegate->shouldRenderItem(item));
        }, [this](Item *item) {
            return painted_delegate && painted_delegate->shouldRenderHole(item);
        });
    };

    // from the top down, each window beneath the caster takes what is left of its place
    Region open = deviceRegion;
    for (int i = stacking_order.indexOf(caster) - 1; i >= 0 && !open.isEmpty(); --i) {
        const Window *receiver = stacking_order[i]->window();
        const int shift = StereoDepth::eyeShift(stereoParallax(receiver), eye);
        const Region place = receiver->isDesktop() ? Region(painted_delegate->deviceRect())
                                                   : Region(viewport.mapToDeviceCoordinatesAligned(receiver->frameGeometry())).translated(shift, 0);
        if (open.intersects(place)) {
            drawOn(open.intersected(place), shift);
            open = open.subtracted(place);
        }
    }
    // what is left falls on nothing: the screen's level
    if (!open.isEmpty()) {
        drawOn(open, 0);
    }
}

// will be eventually called from drawWindow()
void WorkspaceScene::finalDrawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    WindowItem *windowItem = w->windowItem();
    ShadowItem *shadow = windowItem->shadowItem();
    const bool ownShadow = shadow && w->window() == m_depthWindow && shadow->isVisible();
    if (ownShadow) {
        drawStereoShadow(renderTarget, viewport, windowItem, mask, deviceRegion, data);
    }
    // TODO: Reconsider how the CrossFadeEffect captures the initial window contents to remove
    // null pointer delegate checks in "should render item" and "should render hole" checks.
    m_renderer->renderItem(renderTarget, viewport, windowItem, mask, deviceRegion, data, [this, shadow, ownShadow](Item *item) {
        return (ownShadow && item == shadow) || (painted_delegate && !painted_delegate->shouldRenderItem(item));
    }, [this](Item *item) {
        return painted_delegate && painted_delegate->shouldRenderHole(item);
    });
}

EglContext *WorkspaceScene::openglContext() const
{
    if (auto eglBackend = qobject_cast<EglBackend *>(Compositor::self()->backend())) {
        return eglBackend->openglContext();
    }
    return nullptr;
}

bool WorkspaceScene::animationsSupported() const
{
    const auto context = openglContext();
    return context && !context->isSoftwareRenderer();
}

void WorkspaceScene::setLayerDebugging(bool enable)
{
    m_layerDebugging = enable;
    if (m_renderer) {
        m_renderer->setLayerDebugging(enable);
    }
}

} // namespace

#include "moc_workspacescene.cpp"
