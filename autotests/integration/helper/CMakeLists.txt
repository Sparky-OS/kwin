add_executable(kill kill.cpp)
target_link_libraries(kill Qt::Widgets)
ecm_mark_as_test(kill)

add_executable(capturefixture capturefixture.cpp)
qt6_generate_wayland_protocol_client_sources(capturefixture
    PRIVATE_CODE
    FILES
        ${WaylandProtocols_DATADIR}/stable/xdg-shell/xdg-shell.xml
        ${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml
        ${KWIN_STEREO_PROTOCOLS_DIR}/kde-stereo-content-v1.xml
)
target_link_libraries(capturefixture Qt::Gui Qt::WaylandClient Wayland::Client)
ecm_mark_as_test(capturefixture)

if (TARGET K::KPipeWireDmaBuf)
    add_executable(captureconsumer captureconsumer.cpp)
    target_link_libraries(captureconsumer Qt::Gui K::KPipeWire K::KPipeWireDmaBuf)
    ecm_mark_as_test(captureconsumer)
endif()
