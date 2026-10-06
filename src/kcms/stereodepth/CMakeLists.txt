#SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
#SPDX-License-Identifier: BSD-3-Clause

add_definitions(-DTRANSLATION_DOMAIN=\"kcm_kwinstereodepth\")

kcmutils_generate_module_data(
    kcm_kwinstereodepth_PART_SRCS
    MODULE_DATA_HEADER kwinstereodepthdata.h
    MODULE_DATA_CLASS_NAME KWinStereoDepthData
    SETTINGS_HEADERS kwinstereodepthsettings.h
    SETTINGS_CLASSES KWinStereoDepthSettings
)

kconfig_add_kcfg_files(kcm_kwinstereodepth_PART_SRCS kwinstereodepthsettings.kcfgc GENERATE_MOC)
kcmutils_add_qml_kcm(kcm_kwinstereodepth SOURCES kcmkwinstereodepth.cpp ${kcm_kwinstereodepth_PART_SRCS})

target_link_libraries(kcm_kwinstereodepth PRIVATE
    Qt::Qml
    KF6::I18n
    KF6::KCMUtils
    KF6::KCMUtilsQuick
    Qt::DBus
)
