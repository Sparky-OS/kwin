/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QQuickItem>
#include <QVector3D>

namespace KWin
{

/**
 * Qt Quick's Rotation as one eye sees it. A scene that fakes depth with rotations is drawn once per
 * eye, and in each copy what the rotation turns towards the viewer moves apart from what it turns
 * away, by @c parallaxPerDepth pixels of parallax per pixel of depth, half each way, the left eye's
 * copy to the right when nearer. The rotation's own origin lands @c parallax pixels apart. Without
 * an eye it is exactly Qt Quick's Rotation.
 */
class StereoRotation : public QQuickTransform
{
    Q_OBJECT
    Q_PROPERTY(QVector3D origin READ origin WRITE setOrigin NOTIFY changed)
    Q_PROPERTY(qreal angle READ angle WRITE setAngle NOTIFY changed)
    Q_PROPERTY(QVector3D axis READ axis WRITE setAxis NOTIFY changed)
    Q_PROPERTY(qreal distanceToPlane READ distanceToPlane WRITE setDistanceToPlane NOTIFY changed)
    Q_PROPERTY(Eye eye READ eye WRITE setEye NOTIFY changed)
    Q_PROPERTY(qreal parallax READ parallax WRITE setParallax NOTIFY changed)
    Q_PROPERTY(qreal parallaxPerDepth READ parallaxPerDepth WRITE setParallaxPerDepth NOTIFY changed)

public:
    enum Eye {
        NoEye,
        LeftEye,
        RightEye,
    };
    Q_ENUM(Eye)

    explicit StereoRotation(QObject *parent = nullptr);

    QVector3D origin() const;
    void setOrigin(const QVector3D &origin);
    qreal angle() const;
    void setAngle(qreal angle);
    QVector3D axis() const;
    void setAxis(const QVector3D &axis);
    qreal distanceToPlane() const;
    void setDistanceToPlane(qreal distance);
    Eye eye() const;
    void setEye(Eye eye);
    qreal parallax() const;
    void setParallax(qreal parallax);
    qreal parallaxPerDepth() const;
    void setParallaxPerDepth(qreal parallaxPerDepth);

    void applyTo(QMatrix4x4 *matrix) const override;

    /**
     * The offset that brings @a point, in the coordinates the rotation applies to, to where it is
     * drawn without an eye, moved @a shift pixels sideways: how a child of a rotated item is placed
     * at its own depth in this eye.
     */
    Q_INVOKABLE QPointF eyeOffset(const QPointF &point, qreal shift) const;

    /**
     * The same transform as a matrix, for @a side of the eyes: +0.5 for the left eye, -0.5 for the
     * right one, 0 for none.
     */
    static QMatrix4x4 matrix(const QVector3D &origin, qreal angle, const QVector3D &axis, qreal distanceToPlane, qreal side, qreal parallax, qreal parallaxPerDepth);

    /**
     * The offset that brings @a point from where @a seen draws it to where @a plain draws it, moved
     * @a shift pixels sideways.
     */
    static QPointF offset(const QMatrix4x4 &seen, const QMatrix4x4 &plain, const QPointF &point, qreal shift);

Q_SIGNALS:
    void changed();

private:
    qreal side(Eye eye) const;

    QVector3D m_origin;
    qreal m_angle = 0;
    QVector3D m_axis = QVector3D(0, 0, 1);
    qreal m_distanceToPlane = 1024;
    Eye m_eye = NoEye;
    qreal m_parallax = 0;
    qreal m_parallaxPerDepth = 0;
};

} // namespace KWin
