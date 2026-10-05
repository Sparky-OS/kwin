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

public:
    explicit KcmStereoDepth(QObject *parent, const KPluginMetaData &metaData);
    ~KcmStereoDepth() override;

    KWinStereoDepthSettings *settings() const
    {
        return m_settings;
    }

    void save() override;

private:
    KWinStereoDepthData *const m_data;
    KWinStereoDepthSettings *const m_settings;
};
