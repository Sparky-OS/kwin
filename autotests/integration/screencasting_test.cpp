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
#include "core/outputconfiguration.h"
#include "wayland-client/viewporter.h"
#include "generic_scene_opengl_test.h"
#include "opengl/glplatform.h"
#include "pointer_input.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
#include "wayland/display.h"
#include <KScreenLocker/KsldApp>

#include <KWayland/Client/output.h>
#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>
#include <PipeWireSourceStream>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
    void testNativePanelMirror_data();
    void testNativePanelMirror();
    void testNativePanelRefresh();
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
    if (!qEnvironmentVariableIsSet("PHONE_CURSOR_PROOF")) {
        Cursors::self()->hideCursor();
    }
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
    QTest::addColumn<bool>("dmaBuf");
    QTest::addColumn<QSize>("panelSize");
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("refresh");
    QTest::newRow("mono-duplicated") << false << false << QSize(2400, 1080) << 1.0 << 59940;
    QTest::newRow("declared-full-sbs") << true << false << QSize(2400, 1080) << 1.0 << 59940;
    QTest::newRow("mono-dmabuf") << false << true << QSize(2400, 1080) << 1.0 << 59940;
    QTest::newRow("declared-full-sbs-dmabuf") << true << true << QSize(2400, 1080) << 1.0 << 59940;
    QTest::newRow("fractional-native") << true << false << QSize(2520, 1260) << 1.75 << 60000;
    QTest::newRow("1440p-120hz") << true << false << QSize(2560, 1440) << 2.0 << 120000;
    QTest::newRow("4k-native") << true << false << QSize(3840, 2160) << 2.0 << 60000;
}

void ScreencastingTest::testNativePanel()
{
    QFETCH(bool, stereo);
    QFETCH(bool, dmaBuf);
    QFETCH(QSize, panelSize);
    QFETCH(double, scale);
    QFETCH(int, refresh);
    const QSize eyeSize(panelSize.width() / 2, panelSize.height());
    const QSize logicalSize(eyeSize.width() / scale, eyeSize.height() / scale);
    const auto outputCount = workspace()->outputs().size();
    auto stream = std::make_unique<Test::ScreencastingStreamV1>(nullptr);
    stream->init(Test::screencasting()->stream_virtual_output_with_properties(QStringLiteral("native-phone"), QStringLiteral("Native phone panel"),
        panelSize.width(), panelSize.height(), refresh, wl_fixed_from_double(1), 150, 68, 1, QtWayland::zkde_screencast_unstable_v1::pointer_hidden));
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
    QCOMPARE(output->refreshRate(), refresh);
    {
        OutputConfiguration configuration;
        configuration.source = OutputConfiguration::Source::User;
        configuration.changeSet(backendOutput)->scaleSetting = scale;
        QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
    }
    QCOMPARE(backendOutput->modeSize(), panelSize);
    QCOMPARE(output->geometry().size(), logicalSize);
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
    producer.setArguments({fixturePath, QString::number(logicalSize.width()), QString::number(logicalSize.height()), stereo ? QStringLiteral("stereo") : QStringLiteral("mono")});
    const auto stopProducer = qScopeGuard([&] {
        producer.terminate();
        producer.waitForFinished();
    });
    producer.start();
    Window *window = Test::waitForWaylandWindowShown();
    QVERIFY2(window, producer.readAllStandardError().constData());
    QVERIFY(window->pid() != QCoreApplication::applicationPid());
    window->move(output->geometry().topLeft());
    QCOMPARE(window->frameGeometry().size(), QSizeF(logicalSize));

    PipeWireSourceStream receiver;
    std::optional<QImage> image;
    if (dmaBuf) {
        QProcess consumer;
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        consumer.setProcessEnvironment(environment);
        consumer.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/captureconsumer"));
        const auto pixelsPath = fixtureDirectory.filePath(QStringLiteral("pixels.png"));
        const auto ratePath = fixtureDirectory.filePath(QStringLiteral("rate.json"));
        consumer.setArguments({QString::number(stream->nodeId()), pixelsPath, ratePath});
        QSignalSpy consumerFinished(&consumer, &QProcess::finished);
        consumer.start();
        QVERIFY2(consumerFinished.wait(15000), consumer.readAllStandardError().constData());
        QCOMPARE(consumer.exitStatus(), QProcess::NormalExit);
        QVERIFY2(consumer.exitCode() == 0, consumer.readAllStandardError().constData());
        image = QImage(pixelsPath).convertedTo(expected.format());
        QFile rateFile(ratePath);
        QVERIFY(rateFile.open(QIODevice::ReadOnly));
        const auto rate = QJsonDocument::fromJson(rateFile.readAll()).object();
        QVERIFY(rate[QStringLiteral("denominator")].toInt() > 0);
        QCOMPARE(rate[QStringLiteral("numerator")].toInt() * 1000, rate[QStringLiteral("denominator")].toInt() * refresh);
    } else {
        receiver.setAllowDmaBuf(false);
        connect(&receiver, &PipeWireSourceStream::frameReceived, this, [&](const PipeWireFrame &frame) {
            if (frame.dataFrame) {
                image = frame.dataFrame->toImage().convertedTo(expected.format());
            }
        });
        receiver.createStream(stream->nodeId(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(image.has_value(), 10000);
        const auto rate = receiver.framerate();
        QVERIFY(rate.denominator != 0);
        QCOMPARE(quint64(rate.numerator) * 1000, quint64(rate.denominator) * refresh);
    }
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

void ScreencastingTest::testNativePanelMirror_data()
{
    QTest::addColumn<bool>("stereo");
    QTest::addColumn<int>("viewMode");
    QTest::addColumn<double>("zoom");
    QTest::addColumn<double>("centerX");
    QTest::addColumn<QRectF>("expectedViewport");
    QTest::addColumn<QPointF>("pointer");
    QTest::addColumn<QPoint>("windowOffset");
    QTest::newRow("mono-duplicated") << false << 0 << 1.0 << 0.5 << QRectF(0, 0, 1920, 1080) << QPointF(-1, -1) << QPoint();
    QTest::newRow("declared-full-sbs") << true << 0 << 1.0 << 0.5 << QRectF(0, 0, 1920, 1080) << QPointF(-1, -1) << QPoint();
    QTest::newRow("full-height-center") << true << 1 << 1.0 << 0.5 << QRectF(375, 0, 1170, 1080) << QPointF(-1, -1) << QPoint();
    QTest::newRow("full-height-left") << true << 1 << 1.0 << 0.0 << QRectF(0, 0, 1170, 1080) << QPointF(-1, -1) << QPoint();
    QTest::newRow("full-height-right") << true << 1 << 1.0 << 1.0 << QRectF(750, 0, 1170, 1080) << QPointF(-1, -1) << QPoint();
    QTest::newRow("zoom-center") << true << 2 << 1.25 << 0.5 << QRectF(492, 108, 936, 864) << QPointF(-1, -1) << QPoint();
    QTest::newRow("zoom-left") << true << 2 << 1.25 << 0.0 << QRectF(0, 108, 936, 864) << QPointF(-1, -1) << QPoint();
    QTest::newRow("zoom-right") << true << 2 << 1.25 << 1.0 << QRectF(984, 108, 936, 864) << QPointF(-1, -1) << QPoint();
    QTest::newRow("pointer-left") << true << 1 << 1.0 << 0.5 << QRectF(0, 0, 1170, 1080) << QPointF(1, 540) << QPoint();
    QTest::newRow("pointer-right") << true << 1 << 1.0 << 0.5 << QRectF(750, 0, 1170, 1080) << QPointF(1918, 540) << QPoint();
    QTest::newRow("center-active-fit") << true << 0 << 1.0 << 0.5 << QRectF(100, 50, 1920, 1080) << QPointF(-1, -1) << QPoint(100, 50);
}



static QRect boundsOf(const QImage &image, const QRect &area, const std::function<bool(const QColor &)> &match)
{
    QRect bounds;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (match(image.pixelColor(x, y))) {
                bounds |= QRect(x, y, 1, 1);
            }
        }
    }
    return bounds;
}

static bool within(const QRect &measured, const QRect &expected, int tolerance)
{
    return std::abs(measured.left() - expected.left()) <= tolerance && std::abs(measured.top() - expected.top()) <= tolerance
        && std::abs(measured.right() - expected.right()) <= tolerance && std::abs(measured.bottom() - expected.bottom()) <= tolerance;
}

static QString describe(const QRect &rect)
{
    return QStringLiteral("%1,%2 to %3,%4").arg(rect.left()).arg(rect.top()).arg(rect.right()).arg(rect.bottom());
}

void ScreencastingTest::testNativePanelMirror()
{
    // a split-eye panel that replicates a 3D monitor shows each eye's view of the monitor,
    // scaled to fit the eye's viewport and centred, and keeps streaming across the change
    QFETCH(bool, stereo);
    QFETCH(int, viewMode);
    QFETCH(double, zoom);
    QFETCH(double, centerX);
    QFETCH(QRectF, expectedViewport);
    QFETCH(QPointF, pointer);
    QFETCH(QPoint, windowOffset);
    const QSize sourceSize(1920, 1080);
    const OutputModeline plainMode(sourceSize, 30000, OutputModeline::Flag::Preferred);
    const OutputModeline framePacking(sourceSize, 30000, OutputModeline::Flag::Stereo3DFramePacking);
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), sourceSize),
        .modes = {plainMode, framePacking, OutputModeline(QSize(1280, 720), 60000, OutputModeline::Flag::Stereo3DFramePacking)},
    }});
    const auto restoreOutputs = qScopeGuard([] {
        Test::setOutputConfig({
            Rect(0, 0, 1280, 1024),
            Rect(1280, 0, 1280, 1024),
        });
    });
    LogicalOutput *source = workspace()->outputs().front();
    {
        OutputConfiguration configuration;
        configuration.changeSet(source->backendOutput())->currentMode = framePacking;
        QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
    }
    QVERIFY(source->hasStereoEyes());

    // the monitor's content: red with a white block for the left eye, blue with the same block
    // for the right eye; a 2D window gives the left picture to both eyes
    const QRect block(896, 486, 128, 108);
    QImage fixture(stereo ? QSize(sourceSize.width() * 2, sourceSize.height()) : sourceSize, QImage::Format_RGBA8888_Premultiplied);
    fixture.fill(Qt::red);
    {
        QPainter painter(&fixture);
        if (stereo) {
            painter.fillRect(QRect(QPoint(sourceSize.width(), 0), sourceSize), Qt::blue);
            painter.fillRect(block.translated(sourceSize.width(), 0), Qt::white);
        }
        painter.fillRect(block, Qt::white);
    }
    QTemporaryDir fixtureDirectory;
    QVERIFY(fixtureDirectory.isValid());
    const auto fixturePath = fixtureDirectory.filePath(QStringLiteral("fixture.png"));
    QVERIFY(fixture.save(fixturePath));
    QProcess producer;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), waylandServer()->socketName());
    producer.setProcessEnvironment(environment);
    producer.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/capturefixture"));
    producer.setArguments({fixturePath, QString::number(sourceSize.width()), QString::number(sourceSize.height()), stereo ? QStringLiteral("stereo") : QStringLiteral("mono")});
    const auto stopProducer = qScopeGuard([&] {
        producer.terminate();
        producer.waitForFinished();
    });
    producer.start();
    Window *window = Test::waitForWaylandWindowShown();
    QVERIFY2(window, producer.readAllStandardError().constData());
    window->move(source->geometry().topLeft() + windowOffset);
    QCOMPARE(window->frameGeometry(), RectF(source->geometry().translated(windowOffset)));

    const QSize panelSize(2340, 1080);
    const QSize eyeSize(1170, 1080);
    const auto connectPanel = [&]() {
        auto stream = std::make_unique<Test::ScreencastingStreamV1>(nullptr);
        stream->init(Test::screencasting()->stream_virtual_output_with_properties(QStringLiteral("mirror-phone"), QStringLiteral("Mirror phone panel"),
            panelSize.width(), panelSize.height(), 60000, wl_fixed_from_double(1), 150, 68, 1, QtWayland::zkde_screencast_unstable_v1::pointer_hidden));
        return stream;
    };
    auto stream = connectPanel();
    QSignalSpy created(stream.get(), &Test::ScreencastingStreamV1::created);
    QSignalSpy failed(stream.get(), &Test::ScreencastingStreamV1::failed);
    QSignalSpy closed(stream.get(), &Test::ScreencastingStreamV1::closed);
    QVERIFY(created.wait());
    BackendOutput *panel = kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-mirror-phone"));
    QVERIFY(panel);
    QVERIFY(!panel->headsetView().centerActiveWindow);
    QVERIFY(workspace()->findOutput(panel) != source);

    if (pointer.x() >= 0) {
        Test::pointerMotion(pointer, 1);
    }
    {
        OutputConfiguration configuration;
        configuration.source = OutputConfiguration::Source::User;
        configuration.changeSet(panel)->replicationSource = source->backendOutput()->uuid();
        const int configuredMode = qEnvironmentVariableIsSet("KWIN_PHONE_VIEW_NEGATIVE") ? 0 : viewMode;
        configuration.changeSet(panel)->headsetView = HeadsetView{HeadsetView::Mode(configuredMode), zoom, QPointF(centerX, 0.5), pointer.x() >= 0, !windowOffset.isNull()};
        QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
    }
    {
        OutputConfiguration invalid;
        invalid.changeSet(panel)->headsetView = HeadsetView{HeadsetView::Mode(99)};
        QCOMPARE(workspace()->applyOutputConfiguration(invalid), OutputConfigurationError::Unknown);
    }
    QCOMPARE(panel->headsetView().centerActiveWindow, !windowOffset.isNull());
    QCOMPARE(workspace()->findOutput(panel), source);
    // fit 1920x1080 into one eye's 1170x1080: scale 1170 / 1920, 658 rows, centred
    const qreal scale = 1170.0 / 1920.0;
    QCOMPARE(panel->scale(), scale);
    QCOMPARE(panel->deviceOffset(), QPoint(0, 211));

    auto receiver = std::make_unique<PipeWireSourceStream>();
    std::optional<QImage> image;
    const auto receive = [&](quint32 nodeId) {
        receiver = std::make_unique<PipeWireSourceStream>();
        receiver->setAllowDmaBuf(false);
        connect(receiver.get(), &PipeWireSourceStream::frameReceived, this, [&](const PipeWireFrame &frame) {
            if (frame.dataFrame) {
                image = frame.dataFrame->toImage().convertedTo(QImage::Format_RGB32);
            }
        });
        image.reset();
        receiver->createStream(nodeId, 0);
    };
    receive(stream->nodeId());
    QTRY_VERIFY_WITH_TIMEOUT(image.has_value(), 10000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(closed.count(), 0);
    QCOMPARE(image->size(), panelSize);

    const auto isBlack = [](const QColor &c) {
        return c.red() < 8 && c.green() < 8 && c.blue() < 8;
    };
    const auto isWhite = [](const QColor &c) {
        return c.red() > 247 && c.green() > 247 && c.blue() > 247;
    };
    const auto isRed = [](const QColor &c) {
        return c.red() > 247 && c.green() < 8 && c.blue() < 8;
    };
    const auto isBlue = [](const QColor &c) {
        return c.red() < 8 && c.green() < 8 && c.blue() > 247;
    };
    // the prediction: the picture at 0,211 and 1170x658 in each eye, the block scaled into it
    const QRect picture = viewMode == 0 ? QRect(0, 211, 1170, 658) : QRect(0, 0, 1170, 1080);
    const double viewScale = 1170.0 / expectedViewport.width();
    const QRectF croppedBlock = QRectF(block.translated(windowOffset)).intersected(expectedViewport);
    QRect whiteBlock(QPoint(std::ceil((croppedBlock.left() - expectedViewport.left()) * viewScale), picture.y() + std::ceil((croppedBlock.top() - expectedViewport.top()) * viewScale)),
                           QPoint(std::ceil((croppedBlock.right() - expectedViewport.left()) * viewScale) - 1, picture.y() + std::ceil((croppedBlock.bottom() - expectedViewport.top()) * viewScale) - 1));
    const auto checkMirror = [&](const QString &when) {
        for (int eye = 0; eye < 2; ++eye) {
            const QRect eyeArea(QPoint(eye * eyeSize.width(), 0), eyeSize);
            const QRect content = boundsOf(*image, eyeArea, [&](const QColor &c) {
                return !isBlack(c);
            });
            QVERIFY2(within(content, picture.translated(eyeArea.topLeft()), 1), qPrintable(QStringLiteral("%1 eye %2 picture %3").arg(when).arg(eye).arg(describe(content))));
            const auto viewColour = eye == 0 || !stereo ? isRed : isBlue;
            const QRect view = boundsOf(*image, eyeArea, viewColour);
            QVERIFY2(within(view, picture.translated(eyeArea.topLeft()), 1), qPrintable(QStringLiteral("%1 eye %2 view %3").arg(when).arg(eye).arg(describe(view))));
            const QRect white = boundsOf(*image, eyeArea, isWhite);
            QVERIFY2(within(white, whiteBlock.translated(eyeArea.topLeft()), 1), qPrintable(QStringLiteral("%1 eye %2 block %3").arg(when).arg(eye).arg(describe(white))));
            const auto otherColour = eye == 0 || !stereo ? isBlue : isRed;
            QCOMPARE(boundsOf(*image, eyeArea, otherColour), QRect());
            qInfo() << when << "eye" << eye << "picture" << describe(content) << "view" << describe(view) << "block" << describe(white)
                    << "predicted picture" << describe(picture.translated(eyeArea.topLeft())) << "block" << describe(whiteBlock.translated(eyeArea.topLeft()));
        }
    };
    QCOMPARE(source->geometry(), Rect(QPoint(), sourceSize));
    QCOMPARE(window->frameGeometry(), RectF(source->geometry().translated(windowOffset)));
    checkMirror(QStringLiteral("mirrored"));
    if (QTest::currentTestFailed()) {
        return;
    }
    const QString proofDirectory = qEnvironmentVariable("KWIN_PHONE_PROOF_DIR");
    if (!proofDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(proofDirectory));
        QVERIFY(image->save(proofDirectory + (stereo ? QStringLiteral("/mirror-stereo.png") : QStringLiteral("/mirror-mono.png"))));
    }

    if (qEnvironmentVariableIsSet("PHONE_MODE_CYCLE")) {
        QVERIFY(stereo && viewMode == 0 && windowOffset.isNull());
        QVERIFY(image->save(proofDirectory + QStringLiteral("/mode-1080-before.png")));
        const bool changePanel = qEnvironmentVariable("PHONE_MODE_CYCLE") == QStringLiteral("panel");
        const QRect originalWhiteBlock = whiteBlock;
        for (const QSize size : {QSize(1280, 720), changePanel ? panelSize : QSize(1920, 1080)}) {
            OutputConfiguration configuration;
            configuration.source = OutputConfiguration::Source::User;
            auto changedOutput = changePanel ? panel : source->backendOutput();
            if (changePanel && size.height() == 720) {
                const CustomModeDefinition mode{size, 60000, OutputModeline::Flag::Stereo3DFramePacking};
                configuration.changeSet(panel)->customModes = QList<CustomModeDefinition>{mode};
                configuration.changeSet(panel)->currentMode = OutputModeline::custom(mode);
            } else {
                configuration.changeSet(changedOutput)->currentMode = OutputModeline(size,
                    changePanel || size.height() == 720 ? 60000 : 30000,
                    changePanel ? OutputModeline::Flag::Stereo3DSideBySideHalf : OutputModeline::Flag::Stereo3DFramePacking);
            }
            QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
            QCOMPARE(changedOutput->modeSize(), size);
            if (!changePanel) {
                QCOMPARE(source->geometry().size(), size);
            }
            // The fixture follows the source's logical geometry, as a fullscreen client does.
            // Keep the same PipeWire stream and the original packed image throughout.
            producer.terminate();
            QVERIFY(producer.waitForFinished(5000));
            // A new marker position makes stale pre-change frames fail the assertion.
            QImage nextFixture = fixture;
            const int markerShift = size.height() == 720 ? 128 : 0;
            {
                QPainter painter(&nextFixture);
                for (int eye = 0; eye < 2; ++eye) {
                    const QPoint origin(eye * sourceSize.width(), 0);
                    painter.fillRect(block.translated(origin), eye == 0 ? Qt::red : Qt::blue);
                    painter.fillRect(block.translated(origin + QPoint(markerShift, 0)), Qt::white);
                }
            }
            if (qEnvironmentVariableIsSet("PHONE_MODE_STALE_CONTROL")) nextFixture = fixture;
            QVERIFY(nextFixture.save(fixturePath));
            whiteBlock = originalWhiteBlock.translated(markerShift * 1170 / 1920, 0);
            producer.setArguments({fixturePath, QString::number(source->geometry().width()), QString::number(source->geometry().height()), QStringLiteral("stereo")});
            producer.start();
            window = Test::waitForWaylandWindowShown();
            QVERIFY(window);
            window->move(source->geometry().topLeft());
            image.reset();
            QTRY_VERIFY_WITH_TIMEOUT(image.has_value(), 10000);
            QTest::qWait(500);
            const QString name = QStringLiteral("mode-%1-after").arg(size.height());
            QVERIFY(image->save(proofDirectory + QLatin1Char('/') + name + QStringLiteral(".png")));
            qInfo() << name << "source" << source->geometry() << "panelMode" << panel->modeSize()
                    << "viewSize" << panel->viewSize() << "scale" << panel->scale()
                    << "offset" << panel->deviceOffset() << "stream" << image->size();
            if (!changePanel) {
                QCOMPARE(image->size(), panelSize);
                checkMirror(name);
            }
        }
        return;
    }

    // the phone reconnects: the panel comes back replicating, as it was left, and streams the same picture
    receiver->stopStreaming();
    stream.reset();
    QTRY_VERIFY(!kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-mirror-phone")));
    stream = connectPanel();
    QSignalSpy recreated(stream.get(), &Test::ScreencastingStreamV1::created);
    QSignalSpy reclosed(stream.get(), &Test::ScreencastingStreamV1::closed);
    QVERIFY(recreated.wait());
    panel = kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-mirror-phone"));
    QVERIFY(panel);
    QCOMPARE(workspace()->findOutput(panel), source);
    QCOMPARE(panel->deviceOffset(), QPoint(0, 211));
    QCOMPARE(int(panel->headsetView().mode), viewMode);
    QCOMPARE(panel->headsetView().zoom, zoom);
    receive(stream->nodeId());
    QTRY_VERIFY_WITH_TIMEOUT(image.has_value(), 10000);
    QCOMPARE(image->size(), panelSize);
    checkMirror(QStringLiteral("reconnected"));
    if (QTest::currentTestFailed()) {
        return;
    }

    // ending the replication gives the panel its own desktop again, on the same stream
    window->move(source->geometry().topLeft());
    QTRY_COMPARE(window->frameGeometry(), RectF(source->geometry()));
    {
        OutputConfiguration configuration;
        configuration.changeSet(panel)->replicationSource = QString();
        QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
    }
    QVERIFY(workspace()->findOutput(panel) != source);
    window->move(source->geometry().topLeft());
    QTRY_COMPARE(window->frameGeometry(), RectF(source->geometry()));
    QCOMPARE(panel->deviceOffset(), QPoint());
    image.reset();
    QTRY_VERIFY_WITH_TIMEOUT(image.has_value(), 10000);
    QCOMPARE(image->size(), panelSize);
    QCOMPARE(reclosed.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(boundsOf(*image, QRect(QPoint(), panelSize), isRed), QRect(), 5000);

    receiver->stopStreaming();
    stream.reset();
    QTRY_VERIFY(!kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-mirror-phone")));
}
#endif

void ScreencastingTest::testNativePanelRefresh()
{
    if (!qEnvironmentVariableIsSet("KWIN_PHONE_REFRESH_PROOF")) {
        QSKIP("Opt-in VAAPI/RDP endurance proof needs the isolated GPU rig");
    }
    const QString directory = qEnvironmentVariable("KWIN_PHONE_REFRESH_PROOF");
    ScreenLocker::KSldApp::self()->inhibit();
    const auto restoreIdleLock = qScopeGuard([] { ScreenLocker::KSldApp::self()->uninhibit(); });
    const QSize sourceSize(1920, 1080);
    const OutputModeline framePacking(sourceSize, 30000, OutputModeline::Flag::Stereo3DFramePacking);
    Test::setOutputConfig({Test::OutputInfo{.geometry = Rect(QPoint(), sourceSize), .modes = {framePacking, OutputModeline(QSize(1280, 720), 60000, OutputModeline::Flag::Stereo3DFramePacking)}}});
    LogicalOutput *source = workspace()->outputs().front();
    BackendOutput *const sourceBackend = source->backendOutput();
    QVERIFY(source->hasStereoEyes());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), waylandServer()->socketName());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    environment.insert(QStringLiteral("KWIN_CAPTURE_LATENCY_PROOF"), QStringLiteral("1"));
    QProcess producer, server, client, xvfb;
    for (auto process : {&producer, &server, &client, &xvfb}) {
        process->setProcessEnvironment(environment);
    }
    const auto cleanup = qScopeGuard([&] {
        for (auto process : {&client, &server, &producer, &xvfb}) {
            process->terminate();
            if (!process->waitForFinished(5000)) {
                process->kill();
                process->waitForFinished();
            }
        }
    });
    producer.setStandardErrorFile(directory + QStringLiteral("/fixture.log"));
    producer.setStandardOutputFile(directory + QStringLiteral("/markers.log"));
    producer.start(QCoreApplication::applicationDirPath() + QStringLiteral("/capturefixture"),
                   {directory + QStringLiteral("/chart.png"), QStringLiteral("1920"), QStringLiteral("1080"), QStringLiteral("stereo")});
    Window *window = Test::waitForWaylandWindowShown();
    QVERIFY(window);
    window->move(source->geometry().topLeft());
    QTimer animation;
    int marker = 0;
    connect(&animation, &QTimer::timeout, this, [&] {
        producer.write(QByteArray(1, 'A' + ((marker++) % 15)));
        waylandServer()->display()->flush();
    });
    animation.start(50);
    xvfb.setStandardErrorFile(directory + QStringLiteral("/xvfb.log"));
    xvfb.start(QStringLiteral("Xvfb"), {QStringLiteral(":7"), QStringLiteral("-screen"), QStringLiteral("0"), QStringLiteral("2400x1200x24"), QStringLiteral("-nolisten"), QStringLiteral("tcp")});
    QVERIFY(xvfb.waitForStarted());
    server.setStandardOutputFile(directory + QStringLiteral("/server.log"));
    server.setProcessChannelMode(QProcess::MergedChannels);
    server.start(QStringLiteral("krdpserver"), {QStringLiteral("--plasma"), QStringLiteral("--panel-descriptor"), directory + QStringLiteral("/panel.json"),
        QStringLiteral("-u"), QStringLiteral("bench"), QStringLiteral("-p"), QStringLiteral("bench-pass"), QStringLiteral("--port"), QStringLiteral("3391"),
        QStringLiteral("--certificate"), directory + QStringLiteral("/cert.pem"), QStringLiteral("--certificate-key"), directory + QStringLiteral("/key.pem")});
    QVERIFY(server.waitForStarted());
    environment.insert(QStringLiteral("DISPLAY"), QStringLiteral(":7"));
    client.setProcessEnvironment(environment);
    client.setStandardOutputFile(directory + QStringLiteral("/client.log"));
    client.setProcessChannelMode(QProcess::MergedChannels);
    const auto connectPhone = [&] {
        client.start(QStringLiteral("xfreerdp3"), {QStringLiteral("/v:127.0.0.1:3391"), QStringLiteral("/u:bench"), QStringLiteral("/p:bench-pass"),
            QStringLiteral("/cert:ignore"), QStringLiteral("/gfx:AVC420"), QStringLiteral("/size:2340x1080"), QStringLiteral("/bpp:32"),
            QStringLiteral("/network:lan"), QStringLiteral("-grab-keyboard")});
    };
    QTest::qWait(2500);
    connectPhone();
    BackendOutput *panel = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((panel = kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-refresh-phone"))), 30000);
    window->move(workspace()->findOutput(panel)->geometry().topLeft());
    QFile steps(directory + QStringLiteral("/steps.log"));
    QVERIFY(steps.open(QIODevice::WriteOnly));
    QElapsedTimer elapsed;
    elapsed.start();
    const auto note = [&](const char *message) {
        steps.write(QByteArray::number(elapsed.elapsed()) + ' ' + message + '\n');
        steps.flush();
    };
    const auto mirror = [&](bool enabled) {
        window->move(source->geometry().topLeft());
        OutputConfiguration configuration;
        configuration.changeSet(panel)->replicationSource = enabled ? sourceBackend->uuid() : QString();
        QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
        source = workspace()->findOutput(sourceBackend);
        QVERIFY(source);
        QCOMPARE(workspace()->findOutput(panel) == source, enabled);
        if (!enabled) {
            window->move(workspace()->findOutput(panel)->geometry().topLeft());
        }
        note(enabled ? "mirror" : "independent");
    };
    note("connected-independent");
    if (qEnvironmentVariableIsSet("PHONE_CURSOR_PROOF")) {
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(directory + QStringLiteral("/cursor-independent.done")), 120000);
    }
    QTest::qWait(5000);
    if (qEnvironmentVariableIsSet("PHONE_CURSOR_PROOF")) {
        const auto geometry = workspace()->findOutput(panel)->geometry();
        Test::pointerMotion(QPointF(geometry.right() - 1, geometry.bottom() - 1), 1);
    }
    mirror(true);
    if (qEnvironmentVariableIsSet("PHONE_CURSOR_PROOF")) {
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(directory + QStringLiteral("/cursor-mirror.done")), 120000);
    }
    QTest::qWait(5000);
    if (qEnvironmentVariableIsSet("KWIN_PHONE_LOCK_CYCLE")) {
        note("private-rig-lock");
        ScreenLocker::KSldApp::self()->lock(ScreenLocker::EstablishLock::Immediate);
        QTest::qWait(1000);
        ScreenLocker::KSldApp::self()->unlock();
        note("private-rig-unlock");
    }
    QTest::qWait(15000);
    if (qEnvironmentVariableIsSet("PHONE_MODE_CYCLE")) {
        note("mode-1080-before");
        QTest::qWait(5000);
        for (const QSize size : {QSize(1280, 720), QSize(1920, 1080)}) {
            OutputConfiguration configuration;
            configuration.source = OutputConfiguration::Source::User;
            configuration.changeSet(sourceBackend)->currentMode = OutputModeline(size, size.height() == 720 ? 60000 : 30000, OutputModeline::Flag::Stereo3DFramePacking);
            QCOMPARE(workspace()->applyOutputConfiguration(configuration), OutputConfigurationError::None);
            source = workspace()->findOutput(sourceBackend);
            QVERIFY(source);
            QCOMPARE(source->geometry().size(), size);
            QCOMPARE(panel->modeSize(), QSize(2340, 1080));
            QCOMPARE(panel->scale(), 1170.0 / size.width());
            QCOMPARE(panel->deviceOffset(), QPoint(0, 211));
            producer.terminate();
            QVERIFY(producer.waitForFinished(5000));
            producer.start(QCoreApplication::applicationDirPath() + QStringLiteral("/capturefixture"),
                           {directory + QStringLiteral("/chart.png"), QString::number(size.width()), QString::number(size.height()), QStringLiteral("stereo")});
            window = Test::waitForWaylandWindowShown();
            QVERIFY(window);
            window->move(source->geometry().topLeft());
            note(size.height() == 720 ? "mode-720-after" : "mode-1080-after");
            QTest::qWait(5000);
        }
    }
    mirror(false);
    QTest::qWait(5000);
    mirror(true);
    QTest::qWait(5000);
    note("disconnect");
    client.terminate();
    QVERIFY(client.waitForFinished(5000));
    QTRY_VERIFY(!kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-refresh-phone")));
    connectPhone();
    QTRY_VERIFY_WITH_TIMEOUT((panel = kwinApp()->outputBackend()->findOutput(QStringLiteral("Virtual-refresh-phone"))), 30000);
    mirror(true);
    note("reconnected");
    QTest::qWait(qEnvironmentVariableIntValue("KWIN_PHONE_REFRESH_SECONDS") * 1000);
    note("complete");
    QCOMPARE(server.state(), QProcess::Running);
    QCOMPARE(client.state(), QProcess::Running);
}

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
