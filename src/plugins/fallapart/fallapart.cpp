/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "fallapart.h"
#include "effect/effecthandler.h"
#include "effect/effectwindow.h"
#include "scene/stereodepth.h"
// KConfigSkeleton
#include "fallapartconfig.h"

#include <QEasingCurve>

#include <cmath>

using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(KWIN_FALLAPART, "kwin_effect_fallapart", QtWarningMsg)

namespace KWin
{

// a piece within this many pixels of the output's edge goes flat, so that what is in front of the
// screen is never cut by the screen's edge
static constexpr qreal s_edgeMargin = 40;

// how far a piece strays sideways in Dive, of what it does in Fly: the pieces fall into the
// depth and stay in view
static constexpr qreal s_diveScatter = 0.03;

// the pieces fall for this part of the animation, then bounce on the floor, each bounce half
// as fast and a quarter as high as the one before
static constexpr qreal s_diveFall = 0.4;
static constexpr qreal s_diveBounce = 0.5;

/**
 * How far down a piece in Dive is at @a progress, from 0 at the start to 1 at the floor, where
 * it falls for @a fall of the animation and bounces after that.
 */
static qreal diveProfile(qreal progress, qreal fall)
{
    if (progress < fall) {
        const qreal x = progress / fall;
        return x * x;
    }
    qreal rest = progress - fall;
    qreal duration = 2 * fall * s_diveBounce;
    qreal height = s_diveBounce * s_diveBounce;
    while (rest > duration) {
        rest -= duration;
        duration *= s_diveBounce;
        height *= s_diveBounce * s_diveBounce;
    }
    const qreal x = rest / duration;
    return 1 - height * 4 * x * (1 - x);
}

bool FallApartEffect::supported()
{
    return OffscreenEffect::supported() && effects->animationsSupported();
}

FallApartEffect::FallApartEffect()
{
    FallApartConfig::instance(effects->config());
    reconfigure(ReconfigureAll);
    connect(effects, &EffectsHandler::windowClosed, this, &FallApartEffect::slotWindowClosed);
    connect(effects, &EffectsHandler::windowDataChanged, this, &FallApartEffect::slotWindowDataChanged);
}

void FallApartEffect::reconfigure(ReconfigureFlags)
{
    FallApartConfig::self()->read();
    blockSize = FallApartConfig::blockSize();
    dive = FallApartConfig::mode() == FallApartConfig::EnumMode::Dive;
    depthSpread = FallApartConfig::depthSpread() / 100.0;
}

qreal FallApartEffect::currentProgress() const
{
    qreal progress = 0;
    for (const FallApartAnimation &animation : windows) {
        progress = std::max(progress, animation.progress);
    }
    return progress;
}

void FallApartEffect::setCurrentProgress(qreal progress)
{
    for (FallApartAnimation &animation : windows) {
        animation.progress = progress;
    }
    effects->addRepaintFull();
}

void FallApartEffect::prePaintScreen(ScreenPrePaintData &data)
{
    if (!windows.isEmpty()) {
        data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;
    }
    effects->prePaintScreen(data);
}

void FallApartEffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data)
{
    auto animationIt = windows.find(w);
    if (animationIt != windows.end() && isRealWindow(w)) {
        const int time = animationIt->clock.tick(view).count();
        animationIt->progress += double(time) / animationTime(1s).count();
        data.setTransformed();
    }
    effects->prePaintWindow(view, w, data);
}

void FallApartEffect::apply(EffectWindow *w, int mask, WindowPaintData &data, WindowQuadList &quads)
{
    auto animationIt = windows.constFind(w);
    if (animationIt != windows.constEnd() && isRealWindow(w)) {
        QEasingCurve easing(QEasingCurve::InCubic);
        const qreal t = easing.valueForProgress(animationIt->progress);
        // Request the window to be divided into cells
        quads = quads.makeGrid(blockSize);
        const Window *window = w->window();
        const bool depth = StereoDepth::effectsEnabled() && window;
        const RectF screen = w->screen() ? w->screen()->geometryF() : RectF();
        const qreal progress = std::clamp(animationIt->progress, qreal(0), qreal(1));
        int cnt = 0;
        for (WindowQuad &quad : quads) {
            // make fragments move in various directions, based on where
            // they are (left pieces generally move to the left, etc.)
            QPointF p1(quad[0].x(), quad[0].y());
            double xdiff = 0;
            if (p1.x() < w->width() / 2) {
                xdiff = -(w->width() / 2 - p1.x()) / w->width() * 100;
            }
            if (p1.x() > w->width() / 2) {
                xdiff = (p1.x() - w->width() / 2) / w->width() * 100;
            }
            double ydiff = 0;
            if (p1.y() < w->height() / 2) {
                ydiff = -(w->height() / 2 - p1.y()) / w->height() * 100;
            }
            if (p1.y() > w->height() / 2) {
                ydiff = (p1.y() - w->height() / 2) / w->height() * 100;
            }
            double modif = dive ? s_diveScatter * 64 * (1 - (1 - progress) * (1 - progress)) : t * 64;
            srandom(cnt); // change direction randomly but consistently
            xdiff += (rand() % 21 - 10);
            ydiff += (rand() % 21 - 10);
            for (int j = 0;
                 j < 4;
                 ++j) {
                quad[j].move(quad[j].x() + xdiff * modif, quad[j].y() + ydiff * modif);
            }
            // also make the fragments rotate around their center
            QPointF center((quad[0].x() + quad[1].x() + quad[2].x() + quad[3].x()) / 4,
                           (quad[0].y() + quad[1].y() + quad[2].y() + quad[3].y()) / 4);
            double adiff = (rand() % 720 - 360) / 360. * 2 * M_PI; // spin randomly
            for (int j = 0;
                 j < 4;
                 ++j) {
                double x = quad[j].x() - center.x();
                double y = quad[j].y() - center.y();
                double angle = atan2(y, x);
                angle += animationIt->progress * adiff;
                double dist = sqrt(x * x + y * y);
                x = dist * cos(angle);
                y = dist * sin(angle);
                quad[j].move(center.x() + x, center.y() + y);
            }
            if (depth) {
                // one z for the whole piece, from the same seed: how far it has come or gone
                const qreal spread = 1 - depthSpread * (rand() % 1000) / 1000.0;
                const RectF area = quad.bounds().translated(w->pos());
                qreal z;
                if (dive) {
                    // down to the nearest thing beneath it that it covers, no further: nothing is
                    // behind what it covers
                    const qreal floor = std::min(qreal(0), std::max(-StereoDepth::effectSunkLimit(), StereoDepth::coveredDepth(area, window) - StereoDepth::levelDepth(window)));
                    z = floor * diveProfile(progress, s_diveFall * (2 - spread));
                } else {
                    // towards the viewer, but not out past the edge of the output
                    const qreal edge = std::min({area.left() - screen.left(), area.top() - screen.top(), screen.right() - area.right(), screen.bottom() - area.bottom()});
                    z = StereoDepth::effectPoppedLimit() * t * spread * std::clamp(edge / s_edgeMargin, qreal(0), qreal(1));
                }
                for (int j = 0; j < 4; ++j) {
                    quad[j].setZ(z);
                }
            }
            ++cnt;
        }
        data.multiplyOpacity(interpolate(1.0, 0.0, t));
    }
}

void FallApartEffect::postPaintScreen()
{
    for (auto it = windows.begin(); it != windows.end();) {
        if (it->progress < 1) {
            ++it;
        } else {
            unredirect(it.key());
            it = windows.erase(it);
        }
    }

    effects->addRepaintFull();
    effects->postPaintScreen();
}

bool FallApartEffect::isRealWindow(EffectWindow *w)
{
    // TODO: isSpecialWindow is rather generic, maybe tell windowtypes separately?
    /*
    qCDebug(KWIN_FALLAPART) << "--" << w->caption() << "--------------------------------";
    qCDebug(KWIN_FALLAPART) << "Tooltip:" << w->isTooltip();
    qCDebug(KWIN_FALLAPART) << "Toolbar:" << w->isToolbar();
    qCDebug(KWIN_FALLAPART) << "Desktop:" << w->isDesktop();
    qCDebug(KWIN_FALLAPART) << "Special:" << w->isSpecialWindow();
    qCDebug(KWIN_FALLAPART) << "TopMenu:" << w->isTopMenu();
    qCDebug(KWIN_FALLAPART) << "Notific:" << w->isNotification();
    qCDebug(KWIN_FALLAPART) << "Splash:" << w->isSplash();
    qCDebug(KWIN_FALLAPART) << "Normal:" << w->isNormalWindow();
    */
    if (w->isPopupWindow()) {
        return false;
    }
    if (w->isOutline()) {
        return false;
    }
    if (w->isLockScreen()) {
        return false;
    }
    if (w->isX11Client() && !w->isManaged()) {
        return false;
    }
    if (!w->isNormalWindow()) {
        return false;
    }
    return true;
}

void FallApartEffect::slotWindowClosed(EffectWindow *c)
{
    if (effects->activeFullScreenEffect()) {
        return;
    }
    if (!isRealWindow(c)) {
        return;
    }
    if (!c->isVisible()) {
        return;
    }
    const void *e = c->data(WindowClosedGrabRole).value<void *>();
    if (e && e != this) {
        return;
    }
    c->setData(WindowClosedGrabRole, QVariant::fromValue(static_cast<void *>(this)));

    FallApartAnimation &animation = windows[c];
    animation.progress = 0;
    animation.deletedRef = EffectWindowDeletedRef(c);

    redirect(c);
}

void FallApartEffect::slotWindowDataChanged(EffectWindow *w, int role)
{
    if (role != WindowClosedGrabRole) {
        return;
    }

    if (w->data(role).value<void *>() == this) {
        return;
    }

    auto it = windows.find(w);
    if (it != windows.end()) {
        unredirect(it.key());
        windows.erase(it);
    }
}

bool FallApartEffect::isActive() const
{
    return !windows.isEmpty();
}

} // namespace

#include "moc_fallapart.cpp"
