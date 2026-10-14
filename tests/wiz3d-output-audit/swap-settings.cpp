// SPDX-License-Identifier: GPL-2.0-or-later
#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/Output>
#include <KScreen/SetConfigOperation>
#include <QGuiApplication>
#include <QDebug>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QProcess>
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
    QTimer::singleShot(180000, &app, [] { qFatal("Settings round trip timed out"); });
    auto config = readConfig();
    auto outputs = config->outputs();
    if (outputs.size() != 2) {
        qFatal("Expected two isolated VKMS outputs");
    }
    int first = -1;
    int second = -1;
    for (const auto &output : outputs) {
        if (output->name() == QStringLiteral("HDMI-A-1")) {
            first = output->id();
        } else if (output->name() == QStringLiteral("HDMI-A-2")) {
            second = output->id();
        }
    }
    if (first < 0 || second < 0) {
        qFatal("Expected the two named VKMS connectors");
    }
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
        QElapsedTimer deadline;
        deadline.start();
        do {
            QEventLoop settle;
            QTimer::singleShot(100, &settle, &QEventLoop::quit);
            settle.exec();
            config = readConfig();
        } while (config->output(first)->stereoSwapEyes() != swap && deadline.elapsed() < 30000);
        qInfo() << "READBACK" << first << config->output(first)->stereoSwapEyes() << "expected" << swap
                << "second" << second << config->output(second)->stereoSwapEyes() << "expected" << other;
        if (config->output(first)->stereoSwapEyes() != swap || config->output(second)->stereoSwapEyes() != other) {
            qFatal("Per-output swap readback differs");
        }
        qInfo() << "PASS libkscreen readback" << config->output(first)->name() << swap << "other unchanged" << other;
        if (argc == 2) {
            QEventLoop settle;
            QTimer::singleShot(2000, &settle, &QEventLoop::quit);
            settle.exec();
            const QString folder = QString::fromLocal8Bit(argv[1]);
            const QString tag = swap ? QStringLiteral("libkscreen-first") : QStringLiteral("libkscreen-off");
            QProcess grab;
            grab.setStandardOutputFile(folder + QStringLiteral("/grab-") + tag + QStringLiteral(".txt"));
            grab.setProcessChannelMode(QProcess::MergedChannels);
            grab.start(QStringLiteral("/tmp/build/drm-grab"), {QStringLiteral("/dev/dri/card0"), folder + QLatin1Char('/') + tag + QStringLiteral(".ppm")});
            if (!grab.waitForFinished(30000) || grab.exitStatus() != QProcess::NormalExit || grab.exitCode() != 0) {
                qFatal("Static fixture capture failed");
            }
        }
    }
    return 0;
}
