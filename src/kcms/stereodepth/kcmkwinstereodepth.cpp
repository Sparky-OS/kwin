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
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#include <kwinstereodepthdata.h>

K_PLUGIN_FACTORY_WITH_JSON(KcmStereoDepthFactory, "kcm_kwinstereodepth.json", registerPlugin<KcmStereoDepth>(); registerPlugin<KWinStereoDepthData>();)

KcmStereoDepth::KcmStereoDepth(QObject *parent, const KPluginMetaData &metaData)
    : KQuickManagedConfigModule(parent, metaData)
    , m_data(new KWinStereoDepthData(this))
    , m_settings(new KWinStereoDepthSettings(m_data))
{
    registerSettings(m_settings);
    qmlRegisterAnonymousType<KWinStereoDepthSettings>("org.kde.kwin.kwinstereodepthsettings", 1);

    // the wallpaper's cost follows the slider, and the screens as they change
    qDBusRegisterMetaType<QMap<QString, int>>();
    connect(m_settings, &KWinStereoDepthSettings::SunkLimitChanged, this, &KcmStereoDepth::updateWallpaperCuts);
    QDBusConnection::sessionBus().connect(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/StereoDepth"), QStringLiteral("org.kde.KWin.StereoDepth"),
                                          QStringLiteral("OutputsChanged"), this, SLOT(updateWallpaperCuts()));
    updateWallpaperCuts();
}

KcmStereoDepth::~KcmStereoDepth() = default;

void KcmStereoDepth::save()
{
    KQuickManagedConfigModule::save();
    // KWin reads the limits again and repaints, so the change shows at once
    QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reloadConfig"));
    QDBusConnection::sessionBus().send(message);
}

// KWin works out what each screen's wallpaper loses, from the screen's own width, so the page states
// KWin's number for the Background depth on the slider rather than a formula of its own
void KcmStereoDepth::updateWallpaperCuts()
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/StereoDepth"), QStringLiteral("org.kde.KWin.StereoDepth"), QStringLiteral("WallpaperCutsAt"));
    message << m_settings->sunkLimit();
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        const QDBusPendingReply<QMap<QString, int>> reply = *watcher;
        QVariantList cuts;
        if (!reply.isError()) {
            const QMap<QString, int> outputs = reply.value();
            for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
                cuts.append(QVariantMap{{QStringLiteral("name"), it.key()}, {QStringLiteral("cut"), it.value()}});
            }
        }
        if (cuts != m_wallpaperCuts) {
            m_wallpaperCuts = cuts;
            Q_EMIT wallpaperCutsChanged();
        }
    });
}

#include "kcmkwinstereodepth.moc"
#include "moc_kcmkwinstereodepth.cpp"
