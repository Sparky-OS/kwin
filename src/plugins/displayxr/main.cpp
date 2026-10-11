/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "windowgeometryservice.h"

#include <KPluginFactory>

using namespace KWin;

class KWIN_EXPORT WindowGeometryServiceFactory : public PluginFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginFactory_iid FILE "metadata.json")
    Q_INTERFACES(KWin::PluginFactory)

public:
    explicit WindowGeometryServiceFactory() = default;

    std::unique_ptr<Plugin> create() const override;
};

std::unique_ptr<Plugin> WindowGeometryServiceFactory::create() const
{
    return std::make_unique<WindowGeometryService>();
}

#include "main.moc"
