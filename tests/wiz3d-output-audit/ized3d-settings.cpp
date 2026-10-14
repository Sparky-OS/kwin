// SPDX-License-Identifier: GPL-2.0-or-later
#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/Output>
#include <KScreen/SetConfigOperation>
#include <QDebug>
#include <QGuiApplication>
#include <QTimer>

static KScreen::ConfigPtr readConfig()
{
    auto op = new KScreen::GetConfigOperation(KScreen::ConfigOperation::NoEDID);
    if (!op->exec()) {
        qFatal("Read failed: %s", qPrintable(op->errorString()));
    }
    return op->config();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTimer::singleShot(180000, &app, [] { qFatal("iZ3D settings round trip timed out"); });
    auto config = readConfig();
    const auto outputs = config->outputs().values();
    if (outputs.size() != 2) {
        qFatal("Expected two isolated VKMS outputs");
    }
    for (const auto &output : outputs) {
        if (!output->capabilities().testFlag(KScreen::Output::Capability::Ized3dOptions)
            || output->ized3dMode() != KScreen::Output::Ized3dMode::CF
            || output->ized3dTransfer() != KScreen::Output::Ized3dTransfer::Linear) {
            qFatal("Expected advertised iZ3D options with CF and linear defaults");
        }
    }
    for (int mode = 0; mode < 6; ++mode) {
        for (int transfer = 0; transfer < 2; ++transfer) {
            auto change = config->clone();
            for (int index = 0; index < 2; ++index) {
                const auto output = change->output(outputs[index]->id());
                output->setIzed3dMode(KScreen::Output::Ized3dMode((mode + index) % 6));
                output->setIzed3dTransfer(KScreen::Output::Ized3dTransfer(transfer ^ index));
            }
            auto op = new KScreen::SetConfigOperation(change);
            if (!op->exec()) {
                qFatal("Write failed: %s", qPrintable(op->errorString()));
            }
            config = readConfig();
            for (int index = 0; index < 2; ++index) {
                const auto output = config->output(outputs[index]->id());
                if (int(output->ized3dMode()) != (mode + index) % 6 || int(output->ized3dTransfer()) != (transfer ^ index)) {
                    qFatal("Independent iZ3D settings readback differs");
                }
            }
            qInfo() << "PASS independent libkscreen iZ3D readback" << mode << transfer;
        }
    }
}
