cmake_minimum_required(VERSION 3.5)
project(KinAMP)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK IMPORTED_TARGET REQUIRED gtk+-2.0)
pkg_check_modules(XML IMPORTED_TARGET REQUIRED libxml-2.0)

find_package(Threads REQUIRED)

add_subdirectory(miniaudio)

set(MPEG4_SOURCES
    mpeg4/mp4read.c
    mpeg4/audio.c
    mpeg4/unicode_support.c
)

add_executable(${PROJECT_NAME}
    music_player.cpp
    music_backend.cpp
    ${MPEG4_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    .
    mpeg4
    /usr/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::GTK
    PkgConfig::XML
    Threads::Threads
    miniaudio
    gstreamer-0.10
    gthread-2.0
    lipc
    faad
    dl
    rt
)

add_executable(KinAMP-minimal
    cli_player.cpp
    music_backend.cpp
    ${MPEG4_SOURCES}
)

target_include_directories(KinAMP-minimal PRIVATE
    .
    mpeg4
    /usr/include
    ${GLIB_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
)

target_link_libraries(KinAMP-minimal PRIVATE
    PkgConfig::GTK
    PkgConfig::XML
    gstreamer-0.10
    Threads::Threads
    miniaudio
    faad
    dl
    rt
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

add_executable(radio_cli radio_cli.cpp)
target_compile_options(radio_cli PRIVATE -Wall -Wextra)
