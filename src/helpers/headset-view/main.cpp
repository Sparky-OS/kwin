/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include <KLocalizedString>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QPainter>
#include <QPixmap>
#include "../../core/headsetview.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("kwin");
    QDBusInterface kwin(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"));
    QDialog dialog;
    dialog.setWindowTitle(i18n("Headset View"));
    auto layout = new QFormLayout(&dialog);
    auto outputs = new QComboBox(&dialog);
    auto mode = new QComboBox(&dialog);
    mode->addItems({i18n("Fit the whole desktop"), i18n("Full height with panning"), i18n("Zoom a region")});
    auto zoom = new QDoubleSpinBox(&dialog);
    zoom->setRange(1, 8);
    zoom->setSingleStep(0.25);
    auto horizontal = new QDoubleSpinBox(&dialog);
    auto vertical = new QDoubleSpinBox(&dialog);
    for (auto spin : {horizontal, vertical}) {
        spin->setRange(0, 100);
        spin->setSuffix(QStringLiteral("%"));
    }
    auto follow = new QCheckBox(i18n("Follow the pointer"), &dialog);
    auto active = new QCheckBox(i18n("Center the active window in the headset view"), &dialog);
    auto status = new QLabel(&dialog);
    status->setWordWrap(true);
    auto preview = new QPushButton(i18n("Preview the view size…"), &dialog);
    auto displays = new QPushButton(i18n("Standalone display and KDE scale…"), &dialog);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, &dialog);
    layout->addRow(i18n("Headset:"), outputs);
    layout->addRow(i18n("Mirrored view:"), mode);
    layout->addRow(i18n("Zoom:"), zoom);
    layout->addRow(i18n("Horizontal center:"), horizontal);
    layout->addRow(i18n("Vertical center:"), vertical);
    layout->addRow(follow);
    layout->addRow(active);
    layout->addRow(preview);
    layout->addRow(displays);
    layout->addRow(status);
    layout->addRow(buttons);
    const QDBusReply<QVariantMap> reply = kwin.call(QStringLiteral("headsetViews"));
    if (reply.isValid()) {
        const QVariantMap states = reply.value();
        for (const auto &[uuid, value] : states.asKeyValueRange()) {
            const auto state = qdbus_cast<QVariantMap>(value);
            outputs->addItem(state.value(QStringLiteral("name")).toString(), uuid);
            outputs->setItemData(outputs->count() - 1, state, Qt::UserRole + 1);
        }
    }
    auto load = [&] {
        const auto state = outputs->currentData(Qt::UserRole + 1).toMap();
        mode->setCurrentIndex(state.value(QStringLiteral("mode"), 0).toInt());
        zoom->setValue(state.value(QStringLiteral("zoom"), 1.0).toDouble());
        horizontal->setValue(state.value(QStringLiteral("centerX"), 0.5).toDouble() * 100);
        vertical->setValue(state.value(QStringLiteral("centerY"), 0.5).toDouble() * 100);
        follow->setChecked(state.value(QStringLiteral("followPointer"), true).toBool());
        active->setChecked(state.value(QStringLiteral("centerActiveWindow"), false).toBool());
    };
    QObject::connect(outputs, &QComboBox::currentIndexChanged, &dialog, load);
    load();
    buttons->button(QDialogButtonBox::Apply)->setEnabled(outputs->count() > 0);
    status->setText(outputs->count() ? i18n("These settings affect the headset's mirrored view. The real monitor and its windows keep their placement.")
                                    : i18n("Connect a headset display, then open this window again."));
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        const QDBusReply<bool> result = kwin.call(QStringLiteral("setHeadsetView"), outputs->currentData().toString(),
                                                mode->currentIndex(), zoom->value(), horizontal->value() / 100,
                                                vertical->value() / 100, follow->isChecked(), active->isChecked());
        status->setText(result.isValid() && result.value() ? i18n("Headset view saved.") : i18n("The headset view could not be saved. Check that it is still connected."));
    });
    QObject::connect(preview, &QPushButton::clicked, &dialog, [&] {
        const auto state = outputs->currentData(Qt::UserRole + 1).toMap();
        const QRectF source(0, 0, state.value(QStringLiteral("sourceWidth")).toDouble(), state.value(QStringLiteral("sourceHeight")).toDouble());
        const QSize eye(state.value(QStringLiteral("eyeWidth")).toInt(), state.value(QStringLiteral("eyeHeight")).toInt());
        if (source.isEmpty() || eye.isEmpty()) {
            return;
        }
        const KWin::HeadsetView view{KWin::HeadsetView::Mode(mode->currentIndex()), zoom->value(), QPointF(horizontal->value() / 100, vertical->value() / 100)};
        const auto crop = view.viewport(source, eye, QPointF(source.width() * view.center.x(), source.height() * view.center.y()));
        QPixmap picture(480, 320);
        picture.fill(Qt::black);
        QPainter painter(&picture);
        const double scale = std::min(440.0 / source.width(), 260.0 / source.height());
        painter.translate(20, 20);
        painter.scale(scale, scale);
        painter.fillRect(source, QColor(40, 60, 90));
        painter.setPen(QPen(Qt::white, 2.0 / scale));
        painter.drawRect(crop);
        painter.end();
        QDialog previewDialog(&dialog);
        previewDialog.setWindowTitle(i18n("Headset view preview"));
        QVBoxLayout layout(&previewDialog);
        QLabel image;
        image.setPixmap(picture);
        QLabel explanation(i18n("Blue is the source desktop. White is the selected region. Pointer or active-window following moves this region. This diagram changes no display settings."));
        explanation.setWordWrap(true);
        layout.addWidget(&image);
        layout.addWidget(&explanation);
        previewDialog.exec();
    });
    QObject::connect(displays, &QPushButton::clicked, &dialog, [] {
        QProcess::startDetached(QStringLiteral("kcmshell6"), {QStringLiteral("kcm_kscreen")});
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.show();
    return app.exec();
}
