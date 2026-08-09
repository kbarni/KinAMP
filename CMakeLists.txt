cmake_minimum_required(VERSION 3.5)

# Keep in sync with the GIT release tag: the "Check for updates" dialog compares
# this against the tag name of the latest GitHub release.
project(KinAMP VERSION 2.3)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK IMPORTED_TARGET REQUIRED gtk+-2.0)
pkg_check_modules(XML IMPORTED_TARGET REQUIRED libxml-2.0)

find_package(Threads REQUIRED)

# --- GStreamer ---
# The Kindle ships GStreamer 0.10; any modern desktop has 1.0. Detect which one
# is available and let music_backend.cpp adapt via KINAMP_GST_API. The 0.10 path
# keeps the historical bare -lgstreamer-0.10 link because the Kindle sysroot has
# the headers on the default include path but no gstreamer-0.10.pc.
pkg_check_modules(GST1 IMPORTED_TARGET gstreamer-1.0)
if(GST1_FOUND)
    set(KINAMP_GST_LIBS PkgConfig::GST1)
    set(KINAMP_GST_API 1)
    # autoaudiosink picks whatever the desktop actually uses (pulse/pipewire/alsa).
    set(KINAMP_AUDIO_SINK "audioconvert ! audioresample ! autoaudiosink" CACHE STRING
        "GStreamer sink chain appended to the playback pipeline")
    message(STATUS "KinAMP: using GStreamer ${GST1_VERSION} (sink: ${KINAMP_AUDIO_SINK})")
else()
    set(KINAMP_GST_LIBS gstreamer-0.10)
    set(KINAMP_GST_API 0)
    # mixersink is Lab126's sink; it is the only one that mixes with system audio.
    set(KINAMP_AUDIO_SINK "mixersink" CACHE STRING
        "GStreamer sink chain appended to the playback pipeline")
    message(STATUS "KinAMP: using GStreamer 0.10 (sink: ${KINAMP_AUDIO_SINK})")
endif()

# liblipc only exists on the Kindle; without it the GTK player cannot be built.
find_library(LIPC_LIBRARY NAMES lipc)

add_subdirectory(miniaudio)

set(MPEG4_SOURCES
    mpeg4/mp4read.c
    mpeg4/audio.c
    mpeg4/unicode_support.c
)

if(LIPC_LIBRARY)
    add_executable(${PROJECT_NAME}
        music_player.cpp
        music_backend.cpp
        icy_client.cpp
        tags.cpp
        ${MPEG4_SOURCES}
    )

    target_include_directories(${PROJECT_NAME} PRIVATE
        .
        mpeg4
        /usr/include
    )

    target_compile_definitions(${PROJECT_NAME} PRIVATE
        KINAMP_VERSION="${PROJECT_VERSION}"
        KINAMP_GST_API=${KINAMP_GST_API}
        KINAMP_AUDIO_SINK="${KINAMP_AUDIO_SINK}"
    )

    target_link_libraries(${PROJECT_NAME} PRIVATE
        PkgConfig::GTK
        PkgConfig::XML
        Threads::Threads
        miniaudio
        ${KINAMP_GST_LIBS}
        gthread-2.0
        ${LIPC_LIBRARY}
        faad
        dl
        rt
    )

    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
else()
    message(STATUS "KinAMP: liblipc not found, skipping the GTK player (KinAMP-minimal still builds)")
endif()

add_executable(KinAMP-minimal
    cli_player.cpp
    music_backend.cpp
    icy_client.cpp
    tags.cpp
    ${MPEG4_SOURCES}
)

target_include_directories(KinAMP-minimal PRIVATE
    .
    mpeg4
    /usr/include
)

target_compile_definitions(KinAMP-minimal PRIVATE
    KINAMP_VERSION="${PROJECT_VERSION}"
    KINAMP_GST_API=${KINAMP_GST_API}
    KINAMP_AUDIO_SINK="${KINAMP_AUDIO_SINK}"
)

target_link_libraries(KinAMP-minimal PRIVATE
    PkgConfig::GTK
    PkgConfig::XML
    ${KINAMP_GST_LIBS}
    Threads::Threads
    miniaudio
    faad
    dl
    rt
)

add_executable(radio_cli radio_cli.cpp)
target_compile_options(radio_cli PRIVATE -Wall -Wextra)
