/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kcmkwinstereodepth.h"

#include <qqml.h>

#include <KLocalizedString>
#include <KPluginFactory>

#include <QDBusConnection>
#include <QDBusMessage>

#include <kwinstereodepthdata.h>

K_PLUGIN_FACTORY_WITH_JSON(KcmStereoDepthFactory, "kcm_kwinstereodepth.json", registerPlugin<KcmStereoDepth>(); registerPlugin<KWinStereoDepthData>();)

KcmStereoDepth::KcmStereoDepth(QObject *parent, const KPluginMetaData &metaData)
    : KQuickManagedConfigModule(parent, metaData)
    , m_data(new KWinStereoDepthData(this))
    , m_settings(new KWinStereoDepthSettings(m_data))
{
    registerSettings(m_settings);
    qmlRegisterAnonymousType<KWinStereoDepthSettings>("org.kde.kwin.kwinstereodepthsettings", 1);
}

KcmStereoDepth::~KcmStereoDepth() = default;

void KcmStereoDepth::save()
{
    KQuickManagedConfigModule::save();
    // KWin reads the limits again and repaints, so the change shows at once
    QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reloadConfig"));
    QDBusConnection::sessionBus().send(message);
}

#include "kcmkwinstereodepth.moc"
#include "moc_kcmkwinstereodepth.cpp"
