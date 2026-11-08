/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2010 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once
#include "effect/effect.h"
#include "effect/timeline.h"
#include <KConfigWatcher>

#if KWIN_BUILD_X11
#include <KStartupInfo>
#endif
#include <QIcon>
#include <QObject>

#include <chrono>

#include "scene/stereodepth.h"

namespace KWin
{

class GLShader;
class GLTexture;

class StartupFeedbackEffect
    : public Effect
{
    Q_OBJECT
    Q_PROPERTY(int type READ type)
    Q_PROPERTY(int frame READ currentFrame WRITE setCurrentFrame)

public:
    StartupFeedbackEffect();
    ~StartupFeedbackEffect() override;

    void reconfigure(ReconfigureFlags flags) override;
    void prePaintScreen(ScreenPrePaintData &data) override;
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen) override;
    void postPaintScreen() override;
    bool isActive() const override;

    int requestedEffectChainPosition() const override
    {
        return 90;
    }

    int type() const
    {
        return int(m_type);
    }

    // the frame of the bounce (0 to 19; the animation is at its highest at 0), for tests: a frame from 0 holds the animation there, -1 lets it run
    int currentFrame() const
    {
        return m_frame;
    }
    void setCurrentFrame(int frame);

    static bool supported();

private Q_SLOTS:
    void gotNewStartup(const QString &id, const QIcon &icon);
    void gotRemoveStartup(const QString &id);
    void gotStartupChange(const QString &id, const QIcon &icon);
    void slotMouseChanged(const QPointF &pos, const QPointF &oldpos, Qt::MouseButtons buttons, Qt::MouseButtons oldbuttons, Qt::KeyboardModifiers modifiers, Qt::KeyboardModifiers oldmodifiers);

private:
    enum FeedbackType {
        NoFeedback,
        BouncingFeedback,
        BlinkingFeedback,
        PassiveFeedback,
    };

    struct Startup
    {
        QIcon icon;
        std::shared_ptr<QTimer> expiredTimer;
    };

    void start(const Startup &startup);
    void stop();
    QImage scalePixmap(const QPixmap &pm, const QSize &size) const;
    void prepareTextures(const QPixmap &pix);
    Rect feedbackRect() const;
    // Whether the pointer is on an output that shows stereo depth: there the icon is placed in depth, as part of the pointer
    bool stereoAtPointer() const;
    // The icon's depth in pixels of a view 1920 wide: the pointer's own plane, and for the bounce that plane up to the popped limit and back
    qreal iconDepth(const LogicalOutput *output) const;
    // the icon's frame with the room its parallax needs to either side
    Rect paintedArea() const;
    QSize feedbackIconSize() const;

    qreal m_bounceSizesRatio;
#if KWIN_BUILD_X11
    KStartupInfo *m_startupInfo;
#endif
    QString m_currentStartup;
    QMap<QString, Startup> m_startups;
    bool m_active;
    int m_frame;
    bool m_frameHeld = false;
    int m_progress;
    StereoDepth::Motion m_pointerDepth;
    std::chrono::nanoseconds m_depthTime = std::chrono::nanoseconds::zero();
    AnimationClock m_clock;
    std::unique_ptr<GLTexture> m_bouncingTextures[5];
    std::unique_ptr<GLTexture> m_texture; // for passive and blinking
    FeedbackType m_type;
    Rect m_currentGeometry, m_dirtyRect;
    std::unique_ptr<GLShader> m_blinkingShader;
    int m_cursorSize;
    KConfigWatcher::Ptr m_configWatcher;
    bool m_splashVisible;
    std::chrono::seconds m_timeout;
};

} // namespace
