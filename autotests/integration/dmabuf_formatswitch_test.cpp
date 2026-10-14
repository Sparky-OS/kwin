// SPDX-License-Identifier: GPL-2.0-or-later

#include "kwin_wayland_test.h"
#include "generic_scene_opengl_test.h"

#include "core/graphicsbuffer.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/workspacescene.h"
#include "wayland-client/linuxdmabuf.h"
#include "window.h"

#include <KWayland/Client/surface.h>
#include <QDir>
#include <QScopeGuard>

#include <algorithm>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace KWin
{

class FormatSwitchBuffer : public GraphicsBuffer
{
public:
    DmaBufAttributes attributes;

    QSize size() const override
    {
        return QSize(attributes.width, attributes.height);
    }
    bool hasAlphaChannel() const override
    {
        return attributes.format == DRM_FORMAT_ARGB8888;
    }
    const DmaBufAttributes *dmabufAttributes() const override
    {
        return &attributes;
    }
};

static std::unique_ptr<FormatSwitchBuffer> createBuffer(int device, bool nv12)
{
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int bytes = width * height * 4;
    FileDescriptor memory(memfd_create("kwin-dmabuf-format-test", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!memory.isValid() || ftruncate(memory.get(), bytes) != 0) {
        return nullptr;
    }
    auto data = static_cast<uint8_t *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memory.get(), 0));
    if (data == MAP_FAILED) {
        return nullptr;
    }
    const auto unmap = qScopeGuard([&] {
        munmap(data, bytes);
    });
    if (nv12) {
        const uint8_t levels[] = {16, 89, 162, 235};
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                data[y * width + x] = levels[(y >= height / 2) * 2 + (x >= width / 2)];
            }
        }
        std::fill(data + width * height, data + width * height * 3 / 2, 128);
    } else {
        const QRgb colors[] = {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 255)};
        auto pixels = reinterpret_cast<QRgb *>(data);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                pixels[y * width + x] = colors[(y >= height / 2) * 2 + (x >= width / 2)];
            }
        }
    }
    if (fcntl(memory.get(), F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        return nullptr;
    }
    struct udmabuf_create request = {};
    request.memfd = memory.get();
    request.flags = UDMABUF_FLAGS_CLOEXEC;
    request.size = bytes;
    FileDescriptor dma(ioctl(device, UDMABUF_CREATE, &request));
    if (!dma.isValid()) {
        return nullptr;
    }
    auto buffer = std::make_unique<FormatSwitchBuffer>();
    auto &attributes = buffer->attributes;
    attributes.width = width;
    attributes.height = height;
    attributes.format = nv12 ? DRM_FORMAT_NV12 : DRM_FORMAT_ARGB8888;
    attributes.modifier = DRM_FORMAT_MOD_LINEAR;
    attributes.planeCount = nv12 ? 2 : 1;
    attributes.device = 0;
    attributes.pitch[0] = nv12 ? width : width * 4;
    attributes.fd[0] = std::move(dma);
    if (nv12) {
        attributes.fd[1] = attributes.fd[0].duplicate();
        attributes.offset[1] = width * height;
        attributes.pitch[1] = width;
    }
    return buffer;
}

class DmabufFormatSwitchTest : public GenericSceneOpenGLTest
{
    Q_OBJECT

public:
    DmabufFormatSwitchTest()
        : GenericSceneOpenGLTest(QByteArrayLiteral("O2"))
    {
    }

private Q_SLOTS:
    void init()
    {
        QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::LinuxDmabuf | Test::AdditionalWaylandInterface::ColorRepresentation));
        QVERIFY(Test::linuxDmabuf());
        QVERIFY(Test::colorRepresentation());
    }

    void switchFormats_data()
    {
        QTest::addColumn<bool>("startNv12");
        QTest::newRow("argb-to-nv12") << false;
        QTest::newRow("nv12-to-argb") << true;
    }

    void switchFormats()
    {
        QFETCH(bool, startNv12);
        FileDescriptor device(open("/dev/udmabuf", O_RDWR | O_CLOEXEC));
        if (!device.isValid()) {
            QSKIP("This regression requires the isolated guest's udmabuf device");
        }
        std::vector<std::unique_ptr<FormatSwitchBuffer>> buffers;
        std::vector<wl_buffer *> clientBuffers;
        const auto cleanup = qScopeGuard([&] {
            for (auto buffer : clientBuffers) {
                wl_buffer_destroy(buffer);
            }
        });
        auto surface = Test::createSurface();
        auto shell = Test::createXdgToplevelSurface(surface.get());
        Test::ColorRepresentationSurfaceV1 representation(Test::colorRepresentation()->get_surface(*surface));
        Window *window = nullptr;
        ScreenShotManager screenshot;
        for (int step = 0; step < 4; ++step) {
            const bool nv12 = startNv12 != bool(step % 2);
            auto buffer = createBuffer(device.get(), nv12);
            QVERIFY2(buffer, "Could not allocate the real DMA-BUF fixture");
            auto clientBuffer = Test::linuxDmabuf()->importBuffer(buffer.get());
            QVERIFY(clientBuffer);
            clientBuffers.push_back(clientBuffer);
            buffers.push_back(std::move(buffer));
            using Representation = QtWayland::wp_color_representation_surface_v1;
            representation.set_coefficients_and_range(nv12 ? Representation::coefficients_bt709 : Representation::coefficients_identity,
                                                      nv12 ? Representation::range_limited : Representation::range_full);
            QSignalSpy rendered(kwinApp()->scene(), &WorkspaceScene::frameRendered);
            surface->attachBuffer(clientBuffer);
            surface->damage(QRect(0, 0, 64, 64));
            surface->commit();
            if (!window) {
                window = Test::waitForWaylandWindowShown();
                QVERIFY(window);
            }
            QVERIFY(Test::waylandSync());
            kwinApp()->scene()->addRepaintFull();
            QVERIFY(rendered.wait());
            auto image = screenshot.takeScreenShot(window);
            QVERIFY(image);
            QCOMPARE(image->size(), QSize(64, 64));
            const QString directory = qEnvironmentVariable("KWIN_DMABUF_PROOF_DIR");
            if (!directory.isEmpty()) {
                QVERIFY(QDir().mkpath(directory));
                QVERIFY(image->save(directory + QStringLiteral("/%1-%2-%3.png").arg(QLatin1StringView(QTest::currentDataTag())).arg(step).arg(nv12 ? QLatin1StringView("nv12") : QLatin1StringView("argb"))));
            }
            const QColor rgb[] = {Qt::red, Qt::green, Qt::blue, Qt::white};
            int errors = 0;
            int maxError = 0;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const int quadrant = (y >= 32) * 2 + (x >= 32);
                    const QColor expected = nv12 ? QColor(quadrant * 85, quadrant * 85, quadrant * 85) : rgb[quadrant];
                    const QColor actual = image->pixelColor(x, y);
                    const int error = std::max({std::abs(actual.red() - expected.red()), std::abs(actual.green() - expected.green()), std::abs(actual.blue() - expected.blue())});
                    maxError = std::max(maxError, error);
                    errors += error > (nv12 ? 1 : 0);
                }
            }
            qInfo() << "step" << step << (nv12 ? "NV12" : "ARGB") << "max channel error" << maxError << "incorrect pixels" << errors;
            QCOMPARE(errors, 0);
        }
    }
};

}

WAYLANDTEST_MAIN(KWin::DmabufFormatSwitchTest)
#include "dmabuf_formatswitch_test.moc"
