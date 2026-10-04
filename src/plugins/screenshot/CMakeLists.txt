set(screenshot_COMMON_SOURCES
    screenshot.cpp
    screenshotdbusinterface2.cpp
    screenshotlayer.cpp
)

qt_add_dbus_adaptor(screenshot_COMMON_SOURCES org.kde.KWin.ScreenShot2.xml screenshotdbusinterface2.h KWin::ScreenShotDBusInterface2)

kcoreaddons_add_plugin(screenshot SOURCES main.cpp ${screenshot_COMMON_SOURCES} INSTALL_NAMESPACE "kwin/plugins")
target_link_libraries(screenshot PRIVATE
    kwin

    KF6::Service
    KF6::I18n

    Qt::DBus
)

if(BUILD_TESTING)
    add_library(screenshot_test_support STATIC EXCLUDE_FROM_ALL
        ${screenshot_COMMON_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/screenshotlogging.cpp
    )
    target_include_directories(screenshot_test_support PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(screenshot_test_support PRIVATE
        kwin

        KF6::Service
        KF6::I18n

        Qt::DBus
    )
endif()

ecm_qt_declare_logging_category(screenshot
    HEADER screenshotlogging.h
    IDENTIFIER KWIN_SCREENSHOT
    CATEGORY_NAME kwin_screenshot
    DEFAULT_SEVERITY Warning
)
