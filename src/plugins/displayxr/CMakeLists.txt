kcoreaddons_add_plugin(displayxr INSTALL_NAMESPACE "kwin/plugins")
target_sources(displayxr PRIVATE
    captureexclusion.cpp
    main.cpp
    windowgeometryservice.cpp
    windowplacement.cpp
)

set(displayxr_xml_SOURCES)
qt_add_dbus_adaptor(displayxr_xml_SOURCES org.displayxr.WindowGeometry1.xml windowgeometryservice.h KWin::WindowGeometryService)
qt_add_dbus_adaptor(displayxr_xml_SOURCES org.displayxr.WindowPlacement1.xml windowplacement.h KWin::WindowPlacement)
qt_add_dbus_adaptor(displayxr_xml_SOURCES org.displayxr.CaptureExclusion1.xml captureexclusion.h KWin::CaptureExclusion)
target_sources(displayxr PRIVATE ${displayxr_xml_SOURCES})

target_link_libraries(displayxr kwin Qt::DBus)
if (KWIN_BUILD_X11)
    target_link_libraries(displayxr XCB::XCB)
endif()
