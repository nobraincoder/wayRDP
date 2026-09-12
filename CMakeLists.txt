cmake_minimum_required(VERSION 3.16)
project(wayRDP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus Network Multimedia)

# Find KDE Frameworks components
find_package(KF6GuiAddons REQUIRED)

# Find PAM library
find_library(PAM_LIBRARY NAMES pam REQUIRED)

# We will use pkg-config to find FreeRDP, PipeWire and libei
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3 freerdp-server3 winpr3)
pkg_check_modules(LIBEI REQUIRED IMPORTED_TARGET libei-1.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)

find_path(KPIPEWIRE_INCLUDE_DIR NAMES pipewirebaseencodedstream.h PATH_SUFFIXES KPipeWire)
find_path(KF6GUIADDONS_INCLUDE_DIR NAMES KColorUtils PATH_SUFFIXES KF6/KGuiAddons)

# Add source files
set(SOURCES
    src/core/main.cpp
    src/core/RdpServer.cpp
    src/core/SystemInputSettings.cpp
    src/channels/RdpGfxChannel.cpp
    src/display/IVirtualDisplayBackend.cpp
    src/display/KWinVirtualDisplay.cpp
    src/input/EiConnection.cpp
    src/video/PipeWireStreamController.cpp
    src/audio/QtAudioController.cpp
    src/session/SessionLifecycleController.cpp
)

add_executable(wayrdp ${SOURCES})

target_include_directories(wayrdp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${KPIPEWIRE_INCLUDE_DIR}
    ${KF6GUIADDONS_INCLUDE_DIR}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SPA_INCLUDE_DIRS}
)

target_link_libraries(wayrdp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::Network
    Qt6::Multimedia
    KF6::GuiAddons
    PkgConfig::FREERDP
    PkgConfig::LIBEI
    PkgConfig::PIPEWIRE
    ${PAM_LIBRARY}
    KPipeWire
    KPipeWireDmaBuf
    KPipeWireRecord
    crypto
    pulse
    pulse-simple
    xkbcommon
)

install(TARGETS wayrdp DESTINATION bin)
