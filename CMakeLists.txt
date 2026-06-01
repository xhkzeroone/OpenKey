cmake_minimum_required(VERSION 3.15)

project(fcitx5-openkey VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Config REQUIRED)
find_package(Fcitx5Utils REQUIRED)

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")
include(CTest)

add_library(openkey SHARED)
set_target_properties(openkey PROPERTIES
    PREFIX ""
    OUTPUT_NAME "openkey"
)

target_compile_features(openkey PRIVATE cxx_std_17)
target_compile_definitions(openkey PRIVATE
    LINUX=1
)

target_include_directories(openkey PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sources/OpenKey/engine"
)

target_sources(openkey PRIVATE
    src/openkey.cpp
    src/openkey.h
    src/openkey-addon.conf.in.in
    src/openkey.conf.in
    src/openkey_adapter.cpp
    src/openkey_adapter.h
    src/openkey_engine_globals.cpp
    src/surrounding_text_utils.cpp
    src/surrounding_text_utils.h

    Sources/OpenKey/engine/ConvertTool.cpp
    Sources/OpenKey/engine/Engine.cpp
    Sources/OpenKey/engine/Macro.cpp
    Sources/OpenKey/engine/Vietnamese.cpp
)

target_link_libraries(openkey PRIVATE
    Fcitx5::Core
    Fcitx5::Config
    Fcitx5::Utils
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/openkey-addon.conf.in.in"
    "${CMAKE_CURRENT_BINARY_DIR}/openkey-addon.conf"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/openkey.conf.in"
    "${CMAKE_CURRENT_BINARY_DIR}/openkey-inputmethod.conf"
    @ONLY
)

set(FCITX5_ADDON_LIBDIR "${FCITX_INSTALL_LIBDIR}/fcitx5")
install(TARGETS openkey DESTINATION "${FCITX5_ADDON_LIBDIR}")
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/openkey-addon.conf"
    RENAME openkey.conf
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/openkey-inputmethod.conf"
    RENAME openkey.conf
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod"
)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor/scalable/apps/openkey.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor/scalable/apps/fcitx-openkey.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor/scalable/apps/fcitx-openkey.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME "org.fcitx.Fcitx5.fcitx-openkey.svg"
)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor/symbolic/apps/fcitx-openkey-symbolic.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor/symbolic/apps/openkey-symbolic.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps"
)
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor/symbolic/apps/fcitx-openkey-symbolic.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps"
    RENAME "org.fcitx.Fcitx5.fcitx-openkey-symbolic.svg"
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(UNIX AND NOT APPLE)
    add_executable(openkey-uinput-server
        src/openkey_uinput_server.cpp
    )
    target_compile_features(openkey-uinput-server PRIVATE cxx_std_17)
    install(TARGETS openkey-uinput-server DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
