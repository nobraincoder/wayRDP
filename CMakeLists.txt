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

include_directories(
    /usr/include/KPipeWire
    /usr/include/pipewire-0.3
    /usr/include/spa-0.2
    /usr/include/freerdp3
    /usr/include/winpr3
    /usr/include/KF6/KGuiAddons
)

# Add source files
set(SOURCES
    src/main.cpp
    src/RdpServer.cpp
    src/KWinVirtualDisplay.cpp
    src/PipeWireStreamController.cpp
    src/QtAudioController.cpp
    src/EiConnection.cpp
    src/SystemInputSettings.cpp
    src/SessionLifecycleController.cpp
    src/IVirtualDisplayBackend.cpp
)

add_executable(wayrdp ${SOURCES})

target_link_libraries(wayrdp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::Network
    Qt6::Multimedia
    KF6::GuiAddons
    PkgConfig::FREERDP
    PkgConfig::LIBEI
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
