/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "core/renderloop.h"
#include "wayland-client/viewporter.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/surface.h>

using namespace std::chrono_literals;

namespace KWin
{

/**
 * What a stereo window repaints is the outputs it is on. A film in full side by side, full screen
 * on one output, makes that output render at the film's rate and no other: another output, in a 3D
 * mode or not, shows nothing of it and has nothing to redraw (STEREO3D.md: outputs are filters of
 * one scene, an output the window is not on is not asked for a frame).
 */
class StereoOutputRepaintTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testOtherOutputStaysIdle_data();
    void testOtherOutputStaysIdle();
};

static const QSize s_outputSize(640, 360);

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

// the 3D twins of the plain mode a monitor gets from "Anaglyph", and the HDMI 3D modes of its EDID
static OutputModeline stereoMode(OutputModeline::Flag layout, bool virtualTwin)
{
    return OutputModeline(s_outputSize, 60000, virtualTwin ? OutputModeline::Flag::VirtualStereo | layout : OutputModeline::Flags(layout));
}

void StereoOutputRepaintTest::initTestCase()
{
    qRegisterMetaType<Window *>();
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
}

void StereoOutputRepaintTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::StereoContentV1 | Test::AdditionalWaylandInterface::Viewporter));
}

void StereoOutputRepaintTest::cleanup()
{
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void StereoOutputRepaintTest::testOtherOutputStaysIdle_data()
{
    QTest::addColumn<int>("secondMode"); // 0 plain, 1 frame packing, 2 anaglyph
    QTest::addColumn<bool>("stereoFilm");
    QTest::addColumn<bool>("fullScreen");
    QTest::addColumn<bool>("otherLeft"); // the other output is to the left of the film's
    QTest::addColumn<bool>("filmAnaglyph"); // the film's own output is in a 3D mode too
    QTest::addColumn<int>("eyeScale"); // 0: the buffer is the surface (or twice it); n: n * 3 / 2 times it, through a viewport

    QTest::addRow("2D second output, 3D film full screen") << 0 << true << true << false << false << 0;
    QTest::addRow("frame packing second output, 3D film full screen") << 1 << true << true << false << false << 0;
    QTest::addRow("anaglyph second output, 3D film full screen") << 2 << true << true << false << false << 0;
    QTest::addRow("frame packing second output, 3D film in a window") << 1 << true << false << false << false << 0;
    QTest::addRow("frame packing second output, 2D film full screen") << 1 << false << true << false << false << 0;
    // the A10: a film larger than the output, drawn through a viewport, the other output to the left, both outputs in a 3D mode
    QTest::addRow("frame packing second output, 3D film bigger than the output through a viewport") << 1 << true << true << false << false << 1;
    QTest::addRow("frame packing second output on the left, 3D film bigger than the output") << 1 << true << true << true << false << 1;
    QTest::addRow("frame packing second output on the left, film output in anaglyph") << 1 << true << true << true << true << 1;
    QTest::addRow("frame packing second output on the left, film output in anaglyph, film as large as the output") << 1 << true << true << true << true << 0;
    QTest::addRow("frame packing second output on the left, 2D film bigger than the output") << 1 << false << true << true << true << 1;
    QTest::addRow("frame packing second output, film smaller than the output") << 1 << true << true << false << false << -1;
}

void StereoOutputRepaintTest::testOtherOutputStaysIdle()
{
    QFETCH(int, secondMode);
    QFETCH(bool, stereoFilm);
    QFETCH(bool, fullScreen);
    QFETCH(bool, otherLeft);
    QFETCH(bool, filmAnaglyph);
    QFETCH(int, eyeScale);

    using Flag = OutputModeline::Flag;
    const auto stereoModes = [](bool plain) {
        return plain ? QList<OutputModeline>{plainMode()} : QList<OutputModeline>{plainMode(), stereoMode(Flag::Stereo3DFramePacking, false), stereoMode(Flag::Stereo3DAnaglyphModern, true)};
    };
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(QPoint(otherLeft ? s_outputSize.width() : 0, 0), s_outputSize),
            .modes = stereoModes(!filmAnaglyph),
        },
        Test::OutputInfo{
            .geometry = Rect(QPoint(otherLeft ? 0 : s_outputSize.width(), 0), s_outputSize),
            .modes = stereoModes(false),
        },
    });
    const QPoint firstTopLeft(otherLeft ? s_outputSize.width() : 0, 0);
    workspace()->setActiveOutput(firstTopLeft + QPoint(10, 10));

    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 2);
    LogicalOutput *first = outputs[0];
    LogicalOutput *second = outputs[1];
    QCOMPARE(first->geometry().topLeft(), firstTopLeft);

    if (secondMode != 0) {
        OutputConfiguration configuration;
        const auto &modes = second->backendOutput()->modes();
        const auto want = secondMode == 1 ? OutputModeline::Flag::Stereo3DFramePacking : OutputModeline::Flag::Stereo3DAnaglyphModern;
        const auto it = std::ranges::find_if(modes, [want](const auto &mode) {
            return mode->flags() & want;
        });
        QVERIFY(it != modes.end());
        configuration.changeSet(second->backendOutput())->currentMode = (*it)->modeline();
        workspace()->applyOutputConfiguration(configuration);
        QVERIFY(Test::waylandSync());
        QVERIFY(second->hasStereoEyes());
    }
    if (filmAnaglyph) {
        OutputConfiguration configuration;
        const auto &modes = first->backendOutput()->modes();
        const auto it = std::ranges::find_if(modes, [](const auto &mode) {
            return mode->flags() & OutputModeline::Flag::Stereo3DAnaglyphModern;
        });
        QVERIFY(it != modes.end());
        configuration.changeSet(first->backendOutput())->currentMode = (*it)->modeline();
        workspace()->applyOutputConfiguration(configuration);
        QVERIFY(Test::waylandSync());
        QVERIFY(first->hasStereoEyes());
    } else {
        QVERIFY(!first->hasStereoEyes());
    }

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    std::unique_ptr<Test::StereoContentV1> declaration;
    if (stereoFilm) {
        declaration = std::make_unique<Test::StereoContentV1>(Test::stereoContentManager()->create(*surface));
        declaration->set_content(Test::StereoContentV1::content_side_by_side_full);
    }
    // a film bigger or smaller than its surface is drawn through a viewport, as a player does
    // (eyeScale 1: the eye is 3/2 of the surface, -1: 1/2 of it)
    std::unique_ptr<WaylandClient::Viewport> viewport;
    if (eyeScale != 0) {
        viewport = Test::viewporter()->createViewport(*surface);
    }
    // a full side by side buffer is both eyes, so twice as wide as the surface
    const auto frameSize = [&](const QSize &size) {
        const QSize eye = eyeScale == 0 ? size : (eyeScale > 0 ? size * 3 / 2 : size / 2);
        if (viewport) {
            viewport->setDestination(size);
        }
        return stereoFilm ? QSize(eye.width() * 2, eye.height()) : eye;
    };
    Window *window = Test::renderAndWaitForShown(surface.get(), frameSize(QSize(320, 180)), Qt::blue);
    QVERIFY(window);
    QCOMPARE(window->output(), first);

    if (fullScreen) {
        QSignalSpy configureRequested(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
        QSignalSpy toplevelConfigureRequested(shellSurface.get(), &Test::XdgToplevel::configureRequested);
        shellSurface->set_fullscreen(nullptr);
        QVERIFY(Test::waylandSync());
        QTRY_VERIFY(!toplevelConfigureRequested.isEmpty() && toplevelConfigureRequested.last().at(0).value<QSize>() == s_outputSize);
        shellSurface->xdgSurface()->ack_configure(configureRequested.last().at(0).value<quint32>());
        Test::render(surface.get(), frameSize(toplevelConfigureRequested.last().at(0).value<QSize>()), Qt::red);
        QTRY_VERIFY(window->isFullScreen());
    }
    QCOMPARE(window->output(), first);
    QVERIFY(Test::waylandSync());

    // let what the setup scheduled settle
    QTest::qWait(300);

    RenderLoop *firstLoop = first->backendOutput()->renderLoop();
    RenderLoop *secondLoop = second->backendOutput()->renderLoop();
    QSignalSpy firstFrames(firstLoop, &RenderLoop::frameRequested);
    QSignalSpy secondFrames(secondLoop, &RenderLoop::frameRequested);

    // the film: a new picture about every 40 ms
    const QSize size = frameSize(fullScreen ? s_outputSize : QSize(320, 180));
    for (int i = 0; i < 25; ++i) {
        Test::render(surface.get(), size, i % 2 ? Qt::red : Qt::green);
        QTest::qWait(40);
    }

    // the output the film is on follows its pictures; no other output draws anything for them
    QVERIFY(firstFrames.count() >= 10);
    QCOMPARE(secondFrames.count(), 0);
}

}

WAYLANDTEST_MAIN(KWin::StereoOutputRepaintTest)
#include "stereo_output_repaint_test.moc"
