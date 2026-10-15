# SPDX-License-Identifier: BSD-2-Clause
add_executable(kwin-headset-view main.cpp)
target_link_libraries(kwin-headset-view Qt::Widgets Qt::DBus KF6::I18n)
install(TARGETS kwin-headset-view ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES org.kde.kwin.headsetview.desktop DESTINATION ${KDE_INSTALL_APPDIR})
