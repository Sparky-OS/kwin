/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Roman Gilg <subdiff@gmail.com>
    SPDX-FileCopyrightText: 2021 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "kwin_export.h"

#include <QPoint>

#include <QSize>

#include "core/backendoutput.h"
#include "drm_blob.h"
#include "drm_object.h"
#include "drm_pointer.h"
#include "utils/edid.h"

namespace KWin
{

class DrmConnector;
class DrmCrtc;
class DrmFramebuffer;

/**
 * The DrmConnectorMode class represents a native mode and the associated blob.
 */
class DrmConnectorMode : public OutputMode
{
public:
    DrmConnectorMode(DrmConnector *connector, drmModeModeInfo nativeMode, OutputModeline::Flags additionalFlags, bool ycbcr420Only = false);

    DrmConnectorMode(const std::shared_ptr<DrmConnectorMode> &base, OutputModeline::Flag layout);
    std::shared_ptr<DrmConnectorMode> virtualBase() const;
    bool requiresYcbcr420() const;
    /**
     * Whether the kernel takes this 2D mode in DisplayPort's frame sequential stereo on its connector,
     * as a TEST_ONLY commit found (DrmGpu::probeFrameSequential()); nullopt before the test.
     */
    std::optional<bool> frameSequentialAccepted() const;
    void setFrameSequentialAccepted(bool accepted);
    static std::shared_ptr<OutputMode> resolveVirtualMode(const std::shared_ptr<OutputMode> &current, const QList<std::shared_ptr<OutputMode>> &modes);

    drmModeModeInfo *nativeMode();
    virtual std::shared_ptr<DrmBlob> blob();
    std::chrono::nanoseconds vblankTime() const;

    bool operator==(const DrmConnectorMode &otherMode) const;
    bool operator==(const drmModeModeInfo &otherMode) const;

private:
    std::shared_ptr<DrmConnectorMode> m_virtualBase;
    DrmConnector *m_connector;
    drmModeModeInfo m_nativeMode;
    std::shared_ptr<DrmBlob> m_blob;
    bool m_ycbcr420Only = false;
    std::optional<bool> m_frameSequentialAccepted;
};

class KWIN_EXPORT DrmConnector : public DrmObject
{
public:
    DrmConnector(DrmGpu *gpu, uint32_t connectorId);

    bool init();

    bool updateProperties() override;
    void disable(DrmAtomicCommit *commit) override;

    bool isCrtcSupported(DrmCrtc *crtc) const;
    bool isConnected() const;
    bool isNonDesktop() const;
    bool isInternal() const;

    const Edid *edid() const;
    QString connectorName() const;
    QString modelName() const;
    QSize physicalSize() const;
    /**
     * @returns the mst path of the connector. Is empty if invalid
     */
    QByteArray mstPath() const;

    QList<std::shared_ptr<DrmConnectorMode>> modes() const;
    static QList<std::shared_ptr<OutputMode>> withVirtualStereoModes(const QList<std::shared_ptr<OutputMode>> &baseModes,
                                                                 const QList<std::shared_ptr<OutputMode>> &previousModes,
                                                                 bool anaglyph, bool otherStereoFormats);
    std::shared_ptr<DrmConnectorMode> generateMode(const QSize &size, float refreshRate, OutputModeline::Flags flags);
    /**
     * A TEST_ONLY commit of @p mode, a mode with DRM_MODE_FLAG_3D_FRAME_SEQUENTIAL, on @p crtc with its
     * primary plane covering the whole frame of both eyes from @p frame: the kernel accepts it only on
     * connectors whose driver alternates the eyes and signals them.
     */
    bool testFrameSequential(DrmCrtc *crtc, const std::shared_ptr<DrmConnectorMode> &mode, const std::shared_ptr<DrmFramebuffer> &frame);

    BackendOutput::SubPixel subpixel() const;

    enum class UnderscanOptions : uint64_t {
        Off = 0,
        On = 1,
        Auto = 2,
    };
    enum class BroadcastRgbOptions : uint64_t {
        Automatic = 0,
        Full = 1,
        Limited = 2,
    };
    enum class LinkStatus : uint64_t {
        Good = 0,
        Bad = 1,
    };
    enum class DrmContentType : uint64_t {
        None = 0,
        Graphics = 1,
        Photo = 2,
        Cinema = 3,
        Game = 4,
    };
    enum class PanelOrientation : uint64_t {
        Normal = 0,
        UpsideDown = 1,
        LeftUp = 2,
        RightUp = 3,
    };
    enum class ScalingMode : uint64_t {
        None = 0,
        Full = 1,
        Center = 2,
        Full_Aspect = 3,
    };
    enum class Colorspace : uint64_t {
        Default,
        BT709_YCC,
        opRGB,
        BT2020_RGB,
        BT2020_YCC,
    };
    enum class AbmLevel : uint64_t {
        Sysfs,
        Off,
        Min,
        Min_Bias,
        Max_Bias,
        Max,
    };
    // "side by side full timing", HDMI connectors of kernels with the edition's HDMI 3D series
    enum class SideBySideFullTiming : uint64_t {
        WholeLineDoubled,
        BlankingKept,
    };

    DrmProperty crtcId;
    DrmProperty nonDesktop;
    DrmProperty dpms;
    DrmProperty edidProp;
    DrmProperty overscan;
    DrmProperty vrrCapable;
    DrmEnumProperty<UnderscanOptions> underscan;
    DrmProperty underscanVBorder;
    DrmProperty underscanHBorder;
    DrmEnumProperty<BroadcastRgbOptions> broadcastRGB;
    DrmProperty maxBpc;
    DrmEnumProperty<LinkStatus> linkStatus;
    DrmEnumProperty<DrmContentType> contentType;
    DrmEnumProperty<PanelOrientation> panelOrientation;
    DrmProperty hdrMetadata;
    DrmEnumProperty<ScalingMode> scalingMode;
    DrmEnumProperty<Colorspace> colorspace;
    DrmProperty path;
    DrmEnumProperty<AbmLevel> abmLevel;
    DrmEnumProperty<SideBySideFullTiming> sideBySideFullTiming;
    // "HDMI 3D metadata", a blob of struct drm_hdmi_3d_metadata (see drm_stereo.h)
    DrmProperty hdmi3dMetadata;

    static DrmContentType kwinToDrmContentType(ContentType type);
    static OutputTransform toKWinTransform(PanelOrientation orientation);
    static BroadcastRgbOptions rgbRangeToBroadcastRgb(BackendOutput::RgbRange rgbRange);
    static BackendOutput::RgbRange broadcastRgbToRgbRange(BroadcastRgbOptions rgbRange);
    static uint32_t refreshRateForMode(_drmModeModeInfo *m);

private:
    QList<std::shared_ptr<DrmConnectorMode>> generateCommonModes();

    DrmUniquePtr<drmModeConnector> m_conn;
    Edid m_edid;
    QSize m_physicalSize = QSize(-1, -1);
    QList<std::shared_ptr<DrmConnectorMode>> m_driverModes;
    QList<std::shared_ptr<DrmConnectorMode>> m_modes;
    uint32_t m_possibleCrtcs = 0;
    QByteArray m_mstPath;

    friend QDebug &operator<<(QDebug &s, const KWin::DrmConnector *obj);
};

QDebug &operator<<(QDebug &s, const KWin::DrmConnector *obj);

}
