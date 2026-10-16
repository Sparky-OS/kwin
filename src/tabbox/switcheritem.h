/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QMatrix4x4>
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QVector3D>

class QAbstractItemModel;

namespace KWin
{

class LogicalOutput;

namespace TabBox
{

class SwitcherItem : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *model READ model NOTIFY modelChanged)
    Q_PROPERTY(QRect screenGeometry READ screenGeometry NOTIFY screenGeometryChanged)
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged)
    Q_PROPERTY(bool allDesktops READ isAllDesktops NOTIFY allDesktopsChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(bool noModifierGrab READ noModifierGrab NOTIFY noModifierGrabChanged)
    Q_PROPERTY(bool compositing READ compositing NOTIFY compositingChanged)
    Q_PROPERTY(bool automaticallyHide READ automaticallyHide WRITE setAutomaticallyHide NOTIFY automaticallyHideChanged)

    /**
     * Whether the switcher can draw both eyes: on a stereo output it then draws them full side by
     * side, left eye first, in a window twice as wide as the screen, and KWin shows each eye its own.
     */
    Q_PROPERTY(bool stereo READ isStereo WRITE setStereo NOTIFY stereoChanged)

    /**
     * Whether the switcher draws both eyes now: it can, its screen has two eyes and effects may use
     * depth.
     */
    Q_PROPERTY(bool stereoEyes READ hasStereoEyes NOTIFY stereoChanged)

    /**
     * How far the switcher may draw in front of and behind the screen: the effects' own limits, as
     * parallax in pixels at the width of the screen.
     */
    Q_PROPERTY(qreal stereoPopped READ stereoPopped NOTIFY stereoChanged)
    Q_PROPERTY(qreal stereoSunk READ stereoSunk NOTIFY stereoChanged)

    /**
     * The main QML item that will be displayed in the Dialog
     */
    Q_PROPERTY(QObject *item READ item WRITE setItem NOTIFY itemChanged)

    Q_CLASSINFO("DefaultProperty", "item")

public:
    SwitcherItem(QObject *parent = nullptr);
    ~SwitcherItem() override;

    QAbstractItemModel *model() const;
    QRect screenGeometry() const;
    bool isVisible() const;
    bool isAllDesktops() const;
    int currentIndex() const;
    void setCurrentIndex(int index);
    QObject *item() const;
    void setItem(QObject *item);
    bool noModifierGrab() const
    {
        return m_noModifierGrab;
    }
    bool compositing();
    bool automaticallyHide() const;
    bool isStereo() const;
    void setStereo(bool stereo);
    bool hasStereoEyes() const;
    qreal stereoPopped() const;
    qreal stereoSunk() const;

    /**
     * The transform of Qt Quick's Rotation with an origin, angle and axis, as @a side of the eyes
     * sees it: +0.5 for the left eye, -0.5 for the right one (StereoRotation). For a switcher that
     * fakes depth with rotations and draws itself once for each eye with Matrix4x4 transforms.
     */
    Q_INVOKABLE QMatrix4x4 stereoRotation(const QVector3D &origin, qreal angle, const QVector3D &axis, qreal side, qreal parallax, qreal parallaxPerDepth) const;

    /**
     * The offset that brings @a point from where @a seen draws it to where @a plain draws it, moved
     * @a shift pixels sideways: how a child of a rotated item is placed at its own depth in an eye.
     */
    Q_INVOKABLE QPointF eyeOffset(const QMatrix4x4 &seen, const QMatrix4x4 &plain, const QPointF &point, qreal shift) const;

    // for usage from outside
    void setModel(QAbstractItemModel *model);
    void setAllDesktops(bool all);
    void setVisible(bool visible);
    void setNoModifierGrab(bool set);
    void setAutomaticallyHide(bool value);

Q_SIGNALS:
    void visibleChanged();
    void currentIndexChanged(int index);
    void modelChanged();
    void allDesktopsChanged();
    void screenGeometryChanged();
    void itemChanged();
    void noModifierGrabChanged();
    void compositingChanged();
    void automaticallyHideChanged();
    void stereoChanged();

    void aboutToShow();
    void aboutToHide();

private:
    LogicalOutput *output() const;
    void updateStereo();

    QAbstractItemModel *m_model;
    QObject *m_item;
    bool m_visible;
    bool m_allDesktops;
    int m_currentIndex;
    QMetaObject::Connection m_selectedIndexConnection;
    bool m_noModifierGrab = false;
    bool m_automaticallyHide = true;
    bool m_stereo = false;
};

inline QAbstractItemModel *SwitcherItem::model() const
{
    return m_model;
}

inline bool SwitcherItem::isVisible() const
{
    return m_visible;
}

inline bool SwitcherItem::isAllDesktops() const
{
    return m_allDesktops;
}

inline int SwitcherItem::currentIndex() const
{
    return m_currentIndex;
}

inline QObject *SwitcherItem::item() const
{
    return m_item;
}

} // TabBox
} // KWin
