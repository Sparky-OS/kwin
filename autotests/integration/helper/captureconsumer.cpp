// SPDX-License-Identifier: GPL-2.0-or-later
#include <DmaBufHandler>
#include <PipeWireSourceStream>
#include <QGuiApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 4) {
        return 2;
    }
    PipeWireSourceStream stream;
    DmaBufHandler download;
    stream.setAllowDmaBuf(true);
    QObject::connect(&stream, &PipeWireSourceStream::frameReceived, &app, [&](const PipeWireFrame &frame) {
        if (!frame.dmabuf) {
            app.exit(3);
            return;
        }
        QImage image(QSize(frame.dmabuf->width, frame.dmabuf->height), QImage::Format_RGBA8888_Premultiplied);
        if (!download.downloadFrame(image, frame) || !image.save(QString::fromLocal8Bit(argv[2]))) {
            app.exit(4);
            return;
        }
        const auto rate = stream.framerate();
        QFile result(QString::fromLocal8Bit(argv[3]));
        if (!result.open(QIODevice::WriteOnly)) {
            app.exit(5);
            return;
        }
        result.write(QJsonDocument(QJsonObject{{QStringLiteral("numerator"), int(rate.numerator)}, {QStringLiteral("denominator"), int(rate.denominator)}}).toJson());
        app.exit(0);
    });
    QTimer::singleShot(10000, &app, [&] { app.exit(6); });
    stream.createStream(QString::fromLocal8Bit(argv[1]).toUInt(), 0);
    return app.exec();
}
