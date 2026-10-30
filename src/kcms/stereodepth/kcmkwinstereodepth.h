/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <KQuickManagedConfigModule>

#include <kwinstereodepthsettings.h>

class KWinStereoDepthData;

class KcmStereoDepth : public KQuickManagedConfigModule
{
    Q_OBJECT
    Q_PROPERTY(KWinStereoDepthSettings *settings READ settings CONSTANT)
    /**
     * How many columns each screen's wallpaper loses at each side to the sunk effect with the
     * Background depth shown on the page, as KWin works it out from the screen's own width and
     * mode: a list of maps with the screen's "name" and the "cut", 0 on a screen in 2D.
     */
    Q_PROPERTY(QVariantList wallpaperCuts READ wallpaperCuts NOTIFY wallpaperCutsChanged)

public:
    explicit KcmStereoDepth(QObject *parent, const KPluginMetaData &metaData);
    ~KcmStereoDepth() override;

    KWinStereoDepthSettings *settings() const
    {
        return m_settings;
    }

    void save() override;

    QVariantList wallpaperCuts() const
    {
        return m_wallpaperCuts;
    }

Q_SIGNALS:
    void wallpaperCutsChanged();

private Q_SLOTS:
    void updateWallpaperCuts();

private:
    KWinStereoDepthData *const m_data;
    KWinStereoDepthSettings *const m_settings;
    QVariantList m_wallpaperCuts;
};
