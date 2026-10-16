/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "switcheritem.h"
// KWin
#include "core/output.h"
#include "options.h"
#include "scene/stereodepth.h"
#include "scripting/stereorotation.h"
#include "tabboxhandler.h"
#include "workspace.h"
// Qt
#include <QAbstractItemModel>
#include <QWindow>

namespace KWin
{
namespace TabBox
{

SwitcherItem::SwitcherItem(QObject *parent)
    : QObject(parent)
    , m_model(nullptr)
    , m_item(nullptr)
    , m_visible(false)
    , m_allDesktops(false)
    , m_currentIndex(0)
{
    m_selectedIndexConnection = connect(tabBox, &TabBoxHandler::selectedIndexChanged, this, [this] {
        if (isVisible()) {
            setCurrentIndex(tabBox->currentIndex().row());
        }
    });
    connect(workspace(), &Workspace::outputsChanged, this, &SwitcherItem::screenGeometryChanged);
    connect(workspace(), &Workspace::outputsChanged, this, &SwitcherItem::updateStereo);
    connect(options, &Options::stereoDepthChanged, this, &SwitcherItem::updateStereo);
}

SwitcherItem::~SwitcherItem()
{
    disconnect(m_selectedIndexConnection);
}

void SwitcherItem::setItem(QObject *item)
{
    if (m_item == item) {
        return;
    }
    m_item = item;
    Q_EMIT itemChanged();
    updateStereo();
}

void SwitcherItem::setModel(QAbstractItemModel *model)
{
    m_model = model;
    Q_EMIT modelChanged();
}

void SwitcherItem::setVisible(bool visible)
{
    if (m_visible == visible) {
        return;
    }
    if (visible) {
        Q_EMIT screenGeometryChanged();
        updateStereo();
    }
    m_visible = visible;
    Q_EMIT visibleChanged();
}

LogicalOutput *SwitcherItem::output() const
{
    switch (tabBox->config().showScreenMode()) {
    case TabBoxConfig::ShowOnPrimaryScreen:
        if (!workspace()->outputOrder().isEmpty()) {
            return workspace()->outputOrder().front();
        }
        Q_FALLTHROUGH();
    case TabBoxConfig::ShowOnActiveScreen:
        return workspace()->activeOutput();
    }
    Q_UNREACHABLE();
}

QRect SwitcherItem::screenGeometry() const
{
    return output()->geometry();
}

void SwitcherItem::setCurrentIndex(int index)
{
    if (m_currentIndex == index) {
        return;
    }
    m_currentIndex = index;
    if (m_model) {
        tabBox->setCurrentIndex(m_model->index(index, 0));
    }
    Q_EMIT currentIndexChanged(m_currentIndex);
}

void SwitcherItem::setAllDesktops(bool all)
{
    if (m_allDesktops == all) {
        return;
    }
    m_allDesktops = all;
    Q_EMIT allDesktopsChanged();
}

void SwitcherItem::setNoModifierGrab(bool set)
{
    if (m_noModifierGrab == set) {
        return;
    }
    m_noModifierGrab = set;
    Q_EMIT noModifierGrabChanged();
}

bool SwitcherItem::automaticallyHide() const
{
    return m_automaticallyHide;
}

void SwitcherItem::setAutomaticallyHide(bool value)
{
    if (m_automaticallyHide == value) {
        return;
    }
    m_automaticallyHide = value;
    Q_EMIT automaticallyHideChanged();
}

bool SwitcherItem::compositing()
{
    return true;
}

bool SwitcherItem::isStereo() const
{
    return m_stereo;
}

void SwitcherItem::setStereo(bool stereo)
{
    if (m_stereo != stereo) {
        m_stereo = stereo;
        updateStereo();
    }
}

bool SwitcherItem::hasStereoEyes() const
{
    return m_stereo && output()->hasStereoEyes() && StereoDepth::effectsEnabled();
}

qreal SwitcherItem::stereoPopped() const
{
    return hasStereoEyes() ? StereoDepth::effectPoppedLimit() * output()->geometry().width() / 1920 : 0;
}

qreal SwitcherItem::stereoSunk() const
{
    return hasStereoEyes() ? StereoDepth::effectSunkLimit() * output()->geometry().width() / 1920 : 0;
}

QMatrix4x4 SwitcherItem::stereoRotation(const QVector3D &origin, qreal angle, const QVector3D &axis, qreal side, qreal parallax, qreal parallaxPerDepth) const
{
    return StereoRotation::matrix(origin, angle, axis, 1024, side, parallax, parallaxPerDepth);
}

QPointF SwitcherItem::eyeOffset(const QMatrix4x4 &seen, const QMatrix4x4 &plain, const QPointF &point, qreal shift) const
{
    return StereoRotation::offset(seen, plain, point, shift);
}

// The switcher's window says that it draws both eyes; KWin then shows it one eye wide.
void SwitcherItem::updateStereo()
{
    if (QWindow *window = qobject_cast<QWindow *>(m_item)) {
        window->setProperty("kwin_stereo", hasStereoEyes());
    }
    Q_EMIT stereoChanged();
}

}
}

#include "moc_switcheritem.cpp"
