/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "compositor.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/backendoutput.h"
#include "wayland-client/viewporter.h"
#include "generic_scene_opengl_test.h"
#include "opengl/glplatform.h"
#include "pointer_input.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/output.h>
#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>
#include <PipeWireSourceStream>
#include <QPainter>
#include <QDir>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QScreen>

#define QCOMPAREIMG(actual, expected, id)                                                        \
    {                                                                                            \
        if ((actual) != (expected)) {                                                            \
            const auto actualFile = QStringLiteral("appium_artifact_actual_%1.png").arg(id);     \
            const auto expectedFile = QStringLiteral("appium_artifact_expected_%1.png").arg(id); \
            (actual).save(actualFile);                                                           \
            (expected).save(expectedFile);                                                       \
            qDebug() << "Generated failed file" << actualFile << expectedFile;                   \
        }                                                                                        \
        QCOMPARE(actual, expected);                                                              \
    }

namespace KWin
{

class ScreencastingTest : public GenericSceneOpenGLTest
{
    Q_OBJECT

public:
    ScreencastingTest()
        : GenericSceneOpenGLTest(QByteArrayLiteral("O2"))
    {
        auto wrap = [this](const QString &process, const QStringList &arguments = {}) {
            // Make sure PipeWire is running. If it's already running it will just exit
            QProcess *p = new QProcess(this);
            p->setProcessChannelMode(QProcess::MergedChannels);
            p->setArguments(arguments);
            connect(this, &QObject::destroyed, p, [p] {
                p->terminate();
                p->waitForFinished();
                p->kill();
            });
            connect(p, &QProcess::errorOccurred, p, [p](auto status) {
                qDebug() << "error" << status << p->program();
            });
            connect(p, &QProcess::finished, p, [p](int code, auto status) {
                if (code != 0) {
                    qDebug() << p->readAll();
                }
                qDebug() << "finished" << code << status << p->program();
            });
            p->setProgram(process);
            p->start();
        };

        // If I run this outside the CI, it breaks the system's pipewire
        if (qgetenv("KDECI_BUILD") == "TRUE") {
            wrap("pipewire");
            wrap("dbus-launch", {"wireplumber"});
        }
    }
private Q_SLOTS:
    void init();
    void testWindowCasting();
    void testWindowWithPopup();
    void testWindowWithPopupDynamic();
    void testOutputCasting();
#ifdef ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_VIRTUAL_OUTPUT_WITH_PROPERTIES_SINCE_VERSION
    void testNativePanel_data();
    void testNativePanel();
#endif

private:
    std::optional<QImage> oneFrameAndClose(Test::ScreencastingStreamV1 *stream);
};

void ScreencastingTest::init()
{
    if (qgetenv("KDECI_BUILD") == "TRUE") {
        QSKIP("CI has pipewire 1.2 that has known process callback issues"); // TODO: Remove it later when CI ships pipewire 1.2 with the fix
    }

    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::ScreencastingV1 | Test::AdditionalWaylandInterface::PresentationTime | Test::AdditionalWaylandInterface::StereoContentV1 | Test::AdditionalWaylandInterface::Viewporter));
    QVERIFY(KWin::Test::screencasting());
    Cursors::self()->hideCursor();
}

std::optional<QImage> ScreencastingTest::oneFrameAndClose(Test::ScreencastingStreamV1 *stream)
{
    Q_ASSERT(stream);
    PipeWireSourceStream pwStream;
    qDebug() << "start" << stream;
    connect(stream, &Test::ScreencastingStreamV1::failed, qGuiApp, [](const QString &error) {
        qDebug() << "stream failed with error" << error;
        Q_ASSERT(false);
    });
    connect(stream, &Test::ScreencastingStreamV1::closed, qGuiApp, [&pwStream] {
        pwStream.setActive(false);
    });
    connect(stream, &Test::ScreencastingStreamV1::created, qGuiApp, [&pwStream](quint32 nodeId) {
        pwStream.createStream(nodeId, 0);
    });

    std::optional<QImage> img;
    connect(&pwStream, &PipeWireSourceStream::frameReceived, qGuiApp, [&img](const PipeWireFrame &frame) {
        if (frame.dataFrame) {
            img = frame.dataFrame->toImage();
        }
    });

    QSignalSpy spy(&pwStream, &PipeWireSourceStream::frameReceived);
    if (!spy.wait()) {
        qDebug() << "Did not receive any frames";
    }
    pwStream.stopStreaming();
    return img;
}

void ScreencastingTest::testWindowCasting()
{
    QImage sourceImage(QSize(30, 10), QImage::Format_RGBA8888_Premultiplied);
    sourceImage.fill(Qt::red);
    Test::XdgToplevelWindow window;
    QVERIFY(window.show(sourceImage));

    auto stream = KWin::Test::screencasting()->createWindowStream(window.m_window->internalId().toString(), QtWayland::zkde_screencast_unstable_v1::pointer_hidden);

    std::optional<QImage> img = oneFrameAndClose(stream);
    QVERIFY(img);
    img->convertTo(sourceImage.format());
    QCOMPAREIMG(*img, sourceImage, QLatin1StringView("window_cast"));
}

void ScreencastingTest::testWindowWithPopup()
{
    QImage windowImage(QSize(30, 10), QImage::Format_RGBA8888_Premultiplied);
    windowImage.fill(Qt::red);
    Test::XdgToplevelWindow window;
    QVERIFY(window.show(windowImage));

    QSize popupSize(5, 5);
    Rect anchoRect(0, 0, 1, 1);
    std::unique_ptr<KWayland::Client::Surface> popupSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(popupSize.width(), popupSize.height());
    positioner->set_anchor_rect(anchoRect.x(), anchoRect.y(), anchoRect.width(), anchoRect.height());
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(popupSurface.get(), window.m_toplevel->xdgSurface(), positioner.get()));

    QImage popupImage(popupSize, QImage::Format_RGBA8888_Premultiplied);
    popupImage.fill(Qt::blue);
    auto popupWindow = Test::renderAndWaitForShown(popupSurface.get(), popupImage);
    QVERIFY(popupWindow);

    std::unique_ptr<Test::ScreencastingStreamV1> stream(KWin::Test::screencasting()->createWindowStream(window.m_window->internalId().toString(), Test::ScreencastingV1::pointer_hidden));

    QImage expectedImage = windowImage;
    QPainter(&expectedImage).drawImage(anchoRect.bottomRight(), popupImage);

    std::optional<QImage> img = oneFrameAndClose(stream.get());
    QVERIFY(img);
    img->convertTo(expectedImage.format());
    QCOMPAREIMG(*img, expectedImage, QLatin1StringView("window_popup"));
}

void ScreencastingTest::testWindowWithPopupDynamic()
{
    QImage windowImage(QSize(30, 10), QImage::Format_RGBA8888_Premultiplied);
    windowImage.fill(Qt::red);
    Test::XdgToplevelWindow window;
    QVERIFY(window.show(windowImage));

    std::unique_ptr<Test::ScreencastingStreamV1> stream(KWin::Test::screencasting()->createWindowStream(window.m_window->internalId().toString(), Test::ScreencastingV1::pointer_hidden));

    PipeWireSourceStream pwStream;
    connect(stream.get(), &Test::ScreencastingStreamV1::failed, qGuiApp, [](const QString &error) {
        QFAIL("Creating stream failed: " + error.toUtf8());
    });
    connect(stream.get(), &Test::ScreencastingStreamV1::closed, qGuiApp, [&pwStream] {
        pwStream.setActive(false);
    });
    QSignalSpy createdSpy(stream.get(), &Test::ScreencastingStreamV1::created);
    QVERIFY(createdSpy.wait());
    pwStream.createStream(createdSpy.first().first().toUInt(), 0);

    auto fetchNextImageFrame = [&pwStream](QImage::Format format) {
        QSignalSpy frameSpy(&pwStream, &PipeWireSourceStream::frameReceived);
        if (!frameSpy.wait()) {
            Q_ASSERT_X(false, "", "no frame received");
            return QImage();
        }
        auto frame = frameSpy.first().first().value<PipeWireFrame>();
        return frame.dataFrame->toImage().convertedTo(format);
    };

    QImage expectedImage = windowImage;
    auto image = fetchNextImageFrame(expectedImage.format());
    QCOMPAREIMG(image, expectedImage, "dynamic_initial");

    QSize popupSize(5, 5);
    Rect anchoRect(0, 0, 1, 1);
    std::unique_ptr<KWayland::Client::Surface> popupSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(popupSize.width(), popupSize.height());
    positioner->set_anchor_rect(anchoRect.x(), anchoRect.y(), anchoRect.width(), anchoRect.height());
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(popupSurface.get(), window.m_toplevel->xdgSurface(), positioner.get()));

    QImage popupImage(popupSize, QImage::Format_RGBA8888_Premultiplied);
    popupImage.fill(Qt::blue);
    auto popupWindow = Test::renderAndWaitForShown(popupSurface.get(), popupImage);
    QVERIFY(popupWindow);

    expectedImage = windowImage;
    QPainter(&expectedImage).drawImage(anchoRect.bottomRight(), popupImage);
    image = fetchNextImageFrame(expectedImage.format());
    QCOMPAREIMG(image, expectedImage, "dynamic_popup");

    popup.reset();

    expectedImage = windowImage;
    image = fetchNextImageFrame(expectedImage.format());
    QCOMPAREIMG(image, expectedImage, "dynamic_popup_closed");
}

#ifdef ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_VIRTUAL_OUTPUT_WITH_PROPERTIES_SINCE_VERSION
void ScreencastingTest::testNativePanel_data()
{
    QTest::addColumn<bool>("stereo");
    QTest::newRow("mono-duplicated") << false;
    QTest::newRow("declared-full-sbs") << true;
}

void ScreencastingTest::testNativePanel()
{
    QFETCH(bool, stereo);
    const QSize panelSize(2400, 1080);
    const QSize eyeSize(1200, 1080);
    const auto outputCount = workspace()->outputs().size();
    auto stream = std::make_unique<Test::ScreencastingStreamV1>(nullptr);
    stream->init(Test::screencasting()->stream_virtual_output_with_properties(QStringLiteral("native-phone"), QStringLiteral("Native phone panel"),
        panelSize.width(), panelSize.height(), 59940, wl_fixed_from_double(1), 150, 68, 1, QtWayland::zkde_screencast_unstable_v1::pointer_hidden));
    QSignalSpy created(stream.get(), &Test::ScreencastingStreamV1::created);
    QSignalSpy failed(stream.get(), &Test::ScreencastingStreamV1::failed);
    QVERIFY(created.wait());
    QCOMPARE(failed.count(), 0);
    QTRY_COMPARE(workspace()->outputs().size(), outputCount + 1);
    auto backendOutput = kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-native-phone"));
    QVERIFY(backendOutput);
    auto output = workspace()->findOutput(backendOutput);
    QVERIFY(output);
    QCOMPARE(output->pixelSize(), eyeSize);
    QCOMPARE(backendOutput->modeSize(), panelSize);
    QCOMPARE(output->refreshRate(), 59940);
    QVERIFY(output->hasStereoEyes());
    QCOMPARE(backendOutput->edid().identifier(), QByteArray("native-phone"));

    QImage expected(panelSize, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < eyeSize.height(); ++y) {
        for (int x = 0; x < eyeSize.width(); ++x) {
            expected.setPixelColor(x, y, QColor((x % 32) * 8, (y % 32) * 8, 32));
            expected.setPixelColor(x + eyeSize.width(), y, QColor((x % 32) * 8, (y % 32) * 8, stereo ? 224 : 32));
        }
    }
    QTemporaryDir fixtureDirectory;
    QVERIFY(fixtureDirectory.isValid());
    const auto fixturePath = fixtureDirectory.filePath(QStringLiteral("fixture.png"));
    QVERIFY((stereo ? expected : expected.copy(QRect(QPoint(), eyeSize))).save(fixturePath));
    QProcess producer;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), waylandServer()->socketName());
    producer.setProcessEnvironment(environment);
    producer.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/capturefixture"));
    producer.setArguments({fixturePath, QString::number(eyeSize.width()), QString::number(eyeSize.height()), stereo ? QStringLiteral("stereo") : QStringLiteral("mono")});
    const auto stopProducer = qScopeGuard([&] {
        producer.terminate();
        producer.waitForFinished();
    });
    producer.start();
    Window *window = Test::waitForWaylandWindowShown();
    QVERIFY2(window, producer.readAllStandardError().constData());
    QVERIFY(window->pid() != QCoreApplication::applicationPid());
    window->move(output->geometry().topLeft());
    QCOMPARE(window->frameGeometry().size(), QSizeF(eyeSize));

    PipeWireSourceStream receiver;
    std::optional<QImage> image;
    connect(&receiver, &PipeWireSourceStream::frameReceived, this, [&](const PipeWireFrame &frame) {
        if (frame.dataFrame) {
            image = frame.dataFrame->toImage().convertedTo(expected.format());
        }
    });
    receiver.createStream(stream->nodeId(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(image.has_value(), 10000);
    const auto rate = receiver.framerate();
    QVERIFY(rate.denominator != 0);
    QCOMPARE(quint64(rate.numerator) * 1000, quint64(rate.denominator) * 59940);
    QCOMPAREIMG(*image, expected, stereo ? QLatin1StringView("native_stereo") : QLatin1StringView("native_mono"));
    const QString proofDirectory = qEnvironmentVariable("KWIN_PHONE_PROOF_DIR");
    if (!proofDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(proofDirectory));
        QVERIFY(image->save(proofDirectory + (stereo ? QStringLiteral("/native-stereo.png") : QStringLiteral("/native-mono.png"))));
    }
    receiver.stopStreaming();
    stream.reset();
    QTRY_COMPARE(workspace()->outputs().size(), outputCount);
    QVERIFY(!kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-native-phone")));
}
#endif

void ScreencastingTest::testOutputCasting()
{
    QVERIFY(Test::waylandSync());
    auto theOutput = Test::waylandOutput(workspace()->activeOutput()->name());
    QVERIFY(theOutput);
    QTRY_VERIFY(!theOutput->pixelSize().isEmpty());

    QImage sourceImage(theOutput->pixelSize(), QImage::Format_RGBA8888_Premultiplied);
    sourceImage.fill(Qt::green);
    {
        QPainter p(&sourceImage);
        p.drawRect(100, 100, 100, 100);
    }

    QTemporaryDir fixtureDirectory;
    QVERIFY(fixtureDirectory.isValid());
    const auto fixturePath = fixtureDirectory.filePath(QStringLiteral("fixture.png"));
    QVERIFY(sourceImage.save(fixturePath));
    QProcess producer;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), waylandServer()->socketName());
    producer.setProcessEnvironment(environment);
    producer.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/capturefixture"));
    producer.setArguments({fixturePath, QString::number(sourceImage.width()), QString::number(sourceImage.height()), QStringLiteral("fullscreen")});
    const auto stopProducer = qScopeGuard([&] {
        producer.terminate();
        producer.waitForFinished();
    });
    producer.start();
    Window *window = Test::waitForWaylandWindowShown();
    QVERIFY2(window, producer.readAllStandardError().constData());
    QVERIFY(window->pid() != QCoreApplication::applicationPid());
    QTRY_VERIFY(window->isFullScreen());
    QCOMPARE(window->frameGeometry(), RectF(window->output()->geometry()));

    auto stream = KWin::Test::screencasting()->createOutputStream(theOutput->output(), QtWayland::zkde_screencast_unstable_v1::pointer_hidden);

    std::optional<QImage> img = oneFrameAndClose(stream);
    QVERIFY(img);
    img->convertTo(sourceImage.format());
    QCOMPAREIMG(*img, sourceImage, QLatin1StringView("output_cast"));
}

}

WAYLANDTEST_MAIN(KWin::ScreencastingTest)
#include "screencasting_test.moc"
