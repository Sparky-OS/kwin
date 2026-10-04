/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2010 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2010 Nokia Corporation and /or its subsidiary(-ies)
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "screenshot.h"
#include "screenshotdbusinterface2.h"

#include "compositor.h"
#include "core/output.h"
#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "opengl/eglbackend.h"
#include "opengl/glplatform.h"
#include "opengl/glutils.h"
#include "scene/decorationitem.h"
#include "scene/item.h"
#include "scene/itemrenderer.h"
#include "scene/shadowitem.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "screenshotlayer.h"
#include "window.h"
#include "workspace.h"

#include <QPainter>
#include <algorithm>
#include <functional>
#include <numeric>
#include <ranges>

namespace KWin
{

static bool shouldFilterWindowFromCapture(Window *window, std::optional<pid_t> pidToHide)
{
    return window->excludeFromCapture() || (pidToHide.has_value() && window->pid() == *pidToHide);
}

static void convertFromGLImage(QImage &img, int w, int h, const OutputTransform &renderTargetTransformation)
{
    // from QtOpenGL/qgl.cpp
    // SPDX-FileCopyrightText: 2010 Nokia Corporation and /or its subsidiary(-ies)
    // see https://github.com/qt/qtbase/blob/dev/src/opengl/qgl.cpp
    if (QSysInfo::ByteOrder == QSysInfo::BigEndian) {
        // OpenGL gives RGBA; Qt wants ARGB
        uint *p = reinterpret_cast<uint *>(img.bits());
        uint *end = p + w * h;
        while (p < end) {
            uint a = *p << 24;
            *p = (*p >> 8) | a;
            p++;
        }
    } else {
        // OpenGL gives ABGR (i.e. RGBA backwards); Qt wants ARGB
        for (int y = 0; y < h; y++) {
            uint *q = reinterpret_cast<uint *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint pixel = *q;
                *q = ((pixel << 16) & 0xff0000) | ((pixel >> 16) & 0xff)
                    | (pixel & 0xff00ff00);

                q++;
            }
        }
    }

    QMatrix4x4 matrix;
    // apply render target transformation
    matrix *= renderTargetTransformation.inverted().toMatrix();
    // OpenGL textures are flipped vs QImage
    matrix.scale(1, -1);
    img = img.transformed(matrix.toTransform());
}

static QImage readFramebuffer(EglContext *context, GLFramebuffer *target, const GLTexture *texture)
{
    GLFramebuffer::pushFramebuffer(target);
    QImage snapshot = QImage(texture->size(), QImage::Format_ARGB32_Premultiplied);
    context->glReadnPixels(0, 0, snapshot.width(), snapshot.height(), GL_RGBA, GL_UNSIGNED_BYTE, snapshot.sizeInBytes(), static_cast<GLvoid *>(snapshot.bits()));
    convertFromGLImage(snapshot, snapshot.width(), snapshot.height(), OutputTransform::Normal);
    GLFramebuffer::popFramebuffer();
    return snapshot;
}

static bool showsStereoContent(const RectF &area)
{
    return std::ranges::any_of(kwinApp()->scene()->containerItem()->childItems(), [&area](Item *item) {
        const auto windowItem = qobject_cast<WindowItem *>(item);
        return windowItem && windowItem->isVisible()
            && windowItem->window()->stereoContent() != StereoContentNone
            && windowItem->mapToScene(windowItem->boundingRect()).intersects(area);
    });
}

bool screenShotIsStereo(LogicalOutput *screen)
{
    return screen->hasStereoEyes() || showsStereoContent(screen->geometryF());
}

bool screenShotIsStereo(const Rect &area)
{
    return std::ranges::any_of(workspace()->outputs(), [&area](LogicalOutput *output) {
        return output->hasStereoEyes() && output->geometry().intersects(area);
    }) || showsStereoContent(RectF(area));
}

bool screenShotIsStereo(Window *window)
{
    return window->stereoContent() != StereoContentNone;
}

class StereoCaptureScope
{
public:
    explicit StereoCaptureScope(ItemRenderer *renderer)
        : m_renderer(renderer)
    {
        m_renderer->setStereoCapture(true);
    }

    ~StereoCaptureScope()
    {
        m_renderer->setStereoEye(StereoEye::None);
        m_renderer->setStereoCapture(false);
    }

    ItemRenderer *const m_renderer;
};

static QImage stereoImage(const QImage &left, const QImage &right)
{
    Q_ASSERT(left.size() == right.size());
    QImage image(QSize(left.width() * 2, left.height()), left.format());
    QPainter painter(&image);
    painter.drawImage(QPoint(0, 0), left);
    painter.drawImage(QPoint(left.width(), 0), right);
    painter.end();
    return image;
}

static QImage renderStereoImage(ItemRenderer *renderer, const std::function<QImage()> &render)
{
    StereoCaptureScope eyeScope(renderer);
    renderer->setStereoEye(StereoEye::Left);
    const QImage left = render();

    renderer->setStereoEye(StereoEye::Right);
    const QImage right = render();

    return stereoImage(left, right);
}

ScreenShotManager::ScreenShotManager()
    : m_dbusInterface2(new ScreenShotDBusInterface2(this))
{
}

ScreenShotManager::~ScreenShotManager()
{
}

// TODO share code with the screencast plugin?

std::optional<QImage> ScreenShotManager::takeScreenShot(LogicalOutput *screen, ScreenShotFlags flags, std::optional<pid_t> pidToHide)
{
    const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
    if (!eglBackend) {
        return std::nullopt;
    }
    const auto context = eglBackend->openglContext();
    if (!context || !context->makeCurrent()) {
        return std::nullopt;
    }

    qreal scale = 1.0;
    if (flags & ScreenShotNativeResolution) {
        scale = screen->scale();
    }
    const QSize nativeSize = (screen->geometryF().size() * scale).toSize();

    const auto offscreenTexture = GLTexture::allocate(GL_RGBA8, nativeSize);
    if (!offscreenTexture) {
        return std::nullopt;
    }
    offscreenTexture->setFilter(GL_LINEAR);
    offscreenTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    const auto target = std::make_unique<GLFramebuffer>(offscreenTexture.get());
    if (!target->valid()) {
        return std::nullopt;
    }

    ScreenshotLayer layer(screen, target.get());
    if (!layer.preparePresentationTest()) {
        return std::nullopt;
    }
    const auto beginInfo = layer.beginFrame();
    if (!beginInfo) {
        return std::nullopt;
    }
    SceneView sceneView(kwinApp()->scene(), screen, nullptr, &layer);
    std::unique_ptr<ItemTreeView> cursorView;
    if (!(flags & ScreenShotIncludeCursor)) {
        cursorView = std::make_unique<ItemTreeView>(&sceneView, kwinApp()->scene()->cursorItem(), workspace()->outputs().front(), nullptr, nullptr);
        cursorView->setExclusive(true);
    }
    sceneView.addWindowFilter([pidToHide](Window *window) {
        return shouldFilterWindowFromCapture(window, pidToHide);
    });
    const Rect fullDamage = Rect(QPoint(), target->size());
    sceneView.setViewport(screen->geometryF());
    sceneView.setScale(scale);
    sceneView.prePaint();
    ItemRenderer *renderer = kwinApp()->scene()->renderer();
    QImage snapshot;
    const bool stereo = screenShotIsStereo(screen);
    if (stereo) {
        snapshot = renderStereoImage(renderer, [&sceneView, &beginInfo, &fullDamage, context, target = target.get(), texture = offscreenTexture.get()] {
            sceneView.paint(beginInfo->renderTarget, QPoint(), fullDamage);
            return readFramebuffer(context, target, texture);
        });
    } else {
        sceneView.paint(beginInfo->renderTarget, QPoint(), fullDamage);
    }
    sceneView.postPaint();
    if (!layer.endFrame(fullDamage, fullDamage, nullptr)) {
        return std::nullopt;
    }
    if (!stereo) {
        snapshot = readFramebuffer(context, target.get(), offscreenTexture.get());
    }

    snapshot.setDevicePixelRatio(scale);
    return snapshot;
}

std::optional<QImage> ScreenShotManager::takeScreenShot(const Rect &area, ScreenShotFlags flags, std::optional<pid_t> pidToHide)
{
    const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
    if (!eglBackend) {
        return std::nullopt;
    }
    const auto context = eglBackend->openglContext();
    if (!context || !context->makeCurrent()) {
        return std::nullopt;
    }

    qreal scale = 1.0;
    if (flags & ScreenShotNativeResolution) {
        const auto outputs = workspace()->outputs();
        for (LogicalOutput *output : outputs) {
            scale = std::max(scale, output->scale());
        }
    }
    const QSize nativeSize = area.size() * scale;

    const auto offscreenTexture = GLTexture::allocate(GL_RGBA8, nativeSize);
    if (!offscreenTexture) {
        return std::nullopt;
    }
    offscreenTexture->setFilter(GL_LINEAR);
    offscreenTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    const auto target = std::make_unique<GLFramebuffer>(offscreenTexture.get());
    if (!target->valid()) {
        return std::nullopt;
    }

    ScreenshotLayer layer(workspace()->outputs().front(), target.get());
    if (!layer.preparePresentationTest()) {
        return std::nullopt;
    }
    const auto beginInfo = layer.beginFrame();
    if (!beginInfo) {
        return std::nullopt;
    }
    SceneView sceneView(kwinApp()->scene(), workspace()->outputs().front(), nullptr, &layer);
    std::unique_ptr<ItemTreeView> cursorView;
    if (!(flags & ScreenShotIncludeCursor)) {
        cursorView = std::make_unique<ItemTreeView>(&sceneView, kwinApp()->scene()->cursorItem(), workspace()->outputs().front(), nullptr, nullptr);
        cursorView->setExclusive(true);
    }
    sceneView.addWindowFilter([pidToHide](Window *window) {
        return shouldFilterWindowFromCapture(window, pidToHide);
    });
    const Rect fullDamage = Rect(QPoint(), target->size());
    sceneView.setViewport(area);
    sceneView.setScale(scale);
    sceneView.prePaint();
    ItemRenderer *renderer = kwinApp()->scene()->renderer();
    QImage snapshot;
    const bool stereo = screenShotIsStereo(area);
    if (stereo) {
        snapshot = renderStereoImage(renderer, [&sceneView, &beginInfo, &fullDamage, context, target = target.get(), texture = offscreenTexture.get()] {
            sceneView.paint(beginInfo->renderTarget, QPoint(), fullDamage);
            return readFramebuffer(context, target, texture);
        });
    } else {
        sceneView.paint(beginInfo->renderTarget, QPoint(), fullDamage);
    }
    sceneView.postPaint();
    if (!layer.endFrame(fullDamage, fullDamage, nullptr)) {
        return std::nullopt;
    }
    if (!stereo) {
        snapshot = readFramebuffer(context, target.get(), offscreenTexture.get());
    }

    snapshot.setDevicePixelRatio(scale);
    return snapshot;
}

std::optional<QImage> ScreenShotManager::takeScreenShotWorkspace(ScreenShotFlags flags, std::optional<pid_t> pidToHide)
{
    const auto outputs = workspace()->outputs();
    if (outputs.isEmpty()) {
        return std::nullopt;
    }

    Rect workspaceGeometry = outputs.front()->geometry();
    for (LogicalOutput *output : outputs | std::views::drop(1)) {
        workspaceGeometry = workspaceGeometry.united(output->geometry());
    }

    if (std::none_of(outputs.begin(), outputs.end(), [](LogicalOutput *output) {
            return screenShotIsStereo(output);
        })) {
        return takeScreenShot(workspaceGeometry, flags, pidToHide);
    }

    const qreal scale = flags & ScreenShotNativeResolution
        ? std::ranges::max(outputs, {}, &LogicalOutput::scale)->scale()
        : 1.0;
    struct Section {
        QImage image;
        int x;
        int y;
        int width;
        int height;
    };
    QList<Section> sections;
    int resultWidth = 0;
    int resultHeight = workspaceGeometry.height();
    for (LogicalOutput *output : outputs) {
        const bool stereo = screenShotIsStereo(output);
        const auto image = takeScreenShot(output, flags, pidToHide);
        if (!image) {
            return std::nullopt;
        }

        const Rect geometry = output->geometry();
        const int leftStereoWidth = std::accumulate(outputs.begin(), outputs.end(), 0, [output](int width, LogicalOutput *other) {
            return width + (screenShotIsStereo(other) && other->geometry().right() <= output->geometry().x()
                                ? other->geometry().width()
                                : 0);
        });
        const int x = geometry.x() - workspaceGeometry.x() + leftStereoWidth;
        const int width = geometry.width() * (stereo ? 2 : 1);
        const int y = geometry.y() - workspaceGeometry.y();
        resultWidth = std::max(resultWidth, x + width);
        resultHeight = std::max(resultHeight, y + geometry.height());
        sections.append(Section{*image, x, y, width, geometry.height()});
    }

    QImage result(QSize(qRound(resultWidth * scale), qRound(resultHeight * scale)), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    result.setDevicePixelRatio(scale);
    QPainter painter(&result);
    for (const Section &section : sections) {
        const QRect target(qRound(section.x * scale), qRound(section.y * scale),
                           qRound(section.width * scale), qRound(section.height * scale));
        painter.drawImage(target, section.image);
    }
    painter.end();
    return result;
}

std::optional<QImage> ScreenShotManager::takeScreenShot(Window *window, ScreenShotFlags flags)
{
    if (window->excludeFromCapture()) {
        return std::nullopt;
    }

    const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
    if (!eglBackend) {
        return std::nullopt;
    }
    const auto context = eglBackend->openglContext();
    if (!context || !context->makeCurrent()) {
        return std::nullopt;
    }

    const qreal scale = window->targetScale();
    RectF geometry = window->visibleGeometry();
    if (window->windowItem()->decorationItem() && !(flags & ScreenShotIncludeDecoration)) {
        geometry = window->clientGeometry();
    } else if (!(flags & ScreenShotIncludeShadow)) {
        geometry = window->frameGeometry();
    }
    const QSize nativeSize = (geometry.size() * scale).toSize();
    const auto offscreenTexture = GLTexture::allocate(GL_RGBA8, nativeSize);
    if (!offscreenTexture) {
        return std::nullopt;
    }

    GLFramebuffer offscreenTarget(offscreenTexture.get());

    RenderTarget renderTarget(&offscreenTarget);
    RenderViewport viewport(geometry, scale, renderTarget, QPoint());

    WorkspaceScene *scene = kwinApp()->scene();

    ItemRenderer *renderer = scene->renderer();
    const auto renderEye = [&renderTarget, &viewport, renderer, window, flags, scene, context, &offscreenTarget, texture = offscreenTexture.get()] {
        renderer->beginFrame(renderTarget, viewport);
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer->renderItem(renderTarget, viewport, window->windowItem(), Scene::PAINT_WINDOW_TRANSFORMED, Region::infinite(), WindowPaintData{}, [flags, w = window->windowItem()](Item *item) {
            const bool deco = flags & ScreenShotFlag::ScreenShotIncludeDecoration;
            const bool shadow = deco && (flags & ScreenShotFlag::ScreenShotIncludeShadow);
            return (!deco && item == w->decorationItem())
                || (!shadow && item == w->shadowItem());
        }, {});
        if ((flags & ScreenShotFlag::ScreenShotIncludeCursor) && scene->cursorItem()->isVisible()) {
            renderer->renderItem(renderTarget, viewport, scene->cursorItem(), 0, Region::infinite(), WindowPaintData{}, {}, {});
        }
        renderer->endFrame();
        return readFramebuffer(context, &offscreenTarget, texture);
    };
    QImage snapshot;
    if (screenShotIsStereo(window)) {
        snapshot = renderStereoImage(renderer, renderEye);
    } else {
        snapshot = renderEye();
    }

    snapshot.setDevicePixelRatio(scale);
    return snapshot;
}

} // namespace KWin

#include "moc_screenshot.cpp"
