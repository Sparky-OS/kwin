// SPDX-License-Identifier: GPL-2.0-or-later
#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/Output>
#include <KScreen/SetConfigOperation>
#include <QGuiApplication>
#include <QDebug>
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
    QTimer::singleShot(60000, &app, [] { qFatal("Settings round trip timed out"); });
    auto config = readConfig();
    auto outputs = config->outputs();
    if (outputs.size() != 2) {
        qFatal("Expected two isolated VKMS outputs");
    }
    const int first = outputs.firstKey();
    const int second = outputs.lastKey();
    const bool other = outputs[second]->stereoSwapEyes();
    if (!outputs[first]->capabilities().testFlag(KScreen::Output::Capability::StereoSwapEyes)) {
        qFatal("Swap eyes capability missing");
    }
    for (bool swap : {true, false}) {
        auto change = config->clone();
        change->output(first)->setStereoSwapEyes(swap);
        auto op = new KScreen::SetConfigOperation(change);
        if (!op->exec()) {
            qFatal("Write failed: %s", qPrintable(op->errorString()));
        }
        config = readConfig();
        if (config->output(first)->stereoSwapEyes() != swap || config->output(second)->stereoSwapEyes() != other) {
            qFatal("Per-output swap readback differs");
        }
        qInfo() << "PASS libkscreen readback" << config->output(first)->name() << swap << "other unchanged" << other;
    }
    return 0;
}
