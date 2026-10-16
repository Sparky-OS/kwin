/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "stereorotation.h"

#include <QTransform>

namespace KWin
{

StereoRotation::StereoRotation(QObject *parent)
    : QQuickTransform(parent)
{
}

QVector3D StereoRotation::origin() const
{
    return m_origin;
}

void StereoRotation::setOrigin(const QVector3D &origin)
{
    if (m_origin != origin) {
        m_origin = origin;
        update();
        Q_EMIT changed();
    }
}

qreal StereoRotation::angle() const
{
    return m_angle;
}

void StereoRotation::setAngle(qreal angle)
{
    if (m_angle != angle) {
        m_angle = angle;
        update();
        Q_EMIT changed();
    }
}

QVector3D StereoRotation::axis() const
{
    return m_axis;
}

void StereoRotation::setAxis(const QVector3D &axis)
{
    if (m_axis != axis) {
        m_axis = axis;
        update();
        Q_EMIT changed();
    }
}

qreal StereoRotation::distanceToPlane() const
{
    return m_distanceToPlane;
}

void StereoRotation::setDistanceToPlane(qreal distance)
{
    if (m_distanceToPlane != distance) {
        m_distanceToPlane = distance;
        update();
        Q_EMIT changed();
    }
}

StereoRotation::Eye StereoRotation::eye() const
{
    return m_eye;
}

void StereoRotation::setEye(Eye eye)
{
    if (m_eye != eye) {
        m_eye = eye;
        update();
        Q_EMIT changed();
    }
}

qreal StereoRotation::parallax() const
{
    return m_parallax;
}

void StereoRotation::setParallax(qreal parallax)
{
    if (m_parallax != parallax) {
        m_parallax = parallax;
        update();
        Q_EMIT changed();
    }
}

qreal StereoRotation::parallaxPerDepth() const
{
    return m_parallaxPerDepth;
}

void StereoRotation::setParallaxPerDepth(qreal parallaxPerDepth)
{
    if (m_parallaxPerDepth != parallaxPerDepth) {
        m_parallaxPerDepth = parallaxPerDepth;
        update();
        Q_EMIT changed();
    }
}

qreal StereoRotation::side(Eye eye) const
{
    switch (eye) {
    case LeftEye:
        return 0.5;
    case RightEye:
        return -0.5;
    default:
        return 0;
    }
}

static void apply(QMatrix4x4 *matrix, const QVector3D &origin, qreal angle, const QVector3D &axis, qreal distanceToPlane,
                  qreal side, qreal parallax, qreal parallaxPerDepth)
{
    if (side != 0) {
        // the origin at its own depth, after the rotation, where the eye sees it
        matrix->translate(side * parallax, 0);
    }
    if (angle == 0 || axis.isNull()) {
        return;
    }
    matrix->translate(origin);
    if (side == 0 || parallaxPerDepth == 0) {
        matrix->projectedRotate(angle, axis.x(), axis.y(), axis.z(), distanceToPlane);
    } else {
        // the depth of each point is the rotation's own before it is flattened onto the screen;
        // the eye sees it moved by its share of the parallax, scaled with the perspective like the point
        QMatrix4x4 rotation;
        rotation.projectedRotate(angle, axis.x(), axis.y(), axis.z(), distanceToPlane);
        QMatrix4x4 depth;
        depth.rotate(angle, axis);
        const float shear = side * parallaxPerDepth;
        rotation(0, 0) += shear * depth(2, 0);
        rotation(0, 1) += shear * depth(2, 1);
        *matrix *= rotation;
    }
    matrix->translate(-origin);
}

void StereoRotation::applyTo(QMatrix4x4 *matrix) const
{
    apply(matrix, m_origin, m_angle, m_axis, m_distanceToPlane, side(m_eye), m_parallax, m_parallaxPerDepth);
}

QMatrix4x4 StereoRotation::matrix(const QVector3D &origin, qreal angle, const QVector3D &axis, qreal distanceToPlane, qreal side, qreal parallax, qreal parallaxPerDepth)
{
    QMatrix4x4 matrix;
    apply(&matrix, origin, angle, axis, distanceToPlane, side, parallax, parallaxPerDepth);
    return matrix;
}

QPointF StereoRotation::offset(const QMatrix4x4 &seen, const QMatrix4x4 &plain, const QPointF &point, qreal shift)
{
    bool invertible = false;
    const QTransform back = seen.toTransform().inverted(&invertible);
    if (!invertible) {
        return QPointF(shift, 0);
    }
    return back.map(plain.toTransform().map(point) + QPointF(shift, 0)) - point;
}

QPointF StereoRotation::eyeOffset(const QPointF &point, qreal shift) const
{
    return offset(matrix(m_origin, m_angle, m_axis, m_distanceToPlane, side(m_eye), m_parallax, m_parallaxPerDepth),
                  matrix(m_origin, m_angle, m_axis, m_distanceToPlane, 0, m_parallax, m_parallaxPerDepth), point, shift);
}

} // namespace KWin

#include "moc_stereorotation.cpp"
