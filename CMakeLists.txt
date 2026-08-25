cmake_minimum_required(VERSION 3.5)

# Keep in sync with the GIT release tag: the "Check for updates" dialog compares
# this against the tag name of the latest GitHub release.
project(KinAMP VERSION 3.0.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Set by kobo-toolchain.cmake. A Kobo has no Lab126 anything: no liblipc, no
# GTK2, no `mixersink`, and nothing that would run the GUI player even if it
# built. Only KinAMP-minimal is wanted there, and it is what the KOReader
# plugin drives anyway.
option(KINAMP_KOBO "Build the Kobo flavour: KinAMP-minimal only" OFF)

# KinAMP-minimal needs glib (main loop, the command FIFO watch) but no toolkit.
# It used to reach glib through gtk+-2.0's include path, which is why it linked
# GTK it never called - a dependency no Kobo can satisfy.
pkg_check_modules(GLIB IMPORTED_TARGET REQUIRED glib-2.0 gobject-2.0)

# The GUI player's dependencies. Kindle-only, so not looked for elsewhere:
# a missing gtk+-2.0 must not fail a Kobo configure.
if(NOT KINAMP_KOBO)
    pkg_check_modules(GTK IMPORTED_TARGET REQUIRED gtk+-2.0)
    pkg_check_modules(XML IMPORTED_TARGET REQUIRED libxml-2.0)
endif()

find_package(Threads REQUIRED)

option(GST10 "Link against gstreamer-1.0 instead of gstreamer-0.10" OFF)
# Kobo firmware ships GStreamer 1.20; 0.10 is a Kindle-only fallback.
if(KINAMP_KOBO)
    set(GST10 ON)
endif()
# `cmake -DGST10` without a value defines it as an empty string; treat that as ON.
if(DEFINED GST10 AND GST10 STREQUAL "")
    set(GST10 ON)
endif()

if(GST10)
    set(GST_LIB gstreamer-1.0)
else()
    set(GST_LIB gstreamer-0.10)
endif()
message(STATUS "Using ${GST_LIB}")
pkg_check_modules(GST IMPORTED_TARGET REQUIRED ${GST_LIB})

# The tail of the playback pipeline, everything after the decoder's raw-PCM
# caps. Not just an element name: a Kobo needs a converter pair in front of its
# sink, where the Kindle's `mixersink` takes the caps as they come.
#
# Kindle: Lab126's `mixersink` is the only sink on the device.
# Kobo: audio is Bluetooth A2DP through PulseAudio (the firmware carries
#       pulseaudio 15 and bluez5, and no sound card - /var/lib/alsa/asound.state
#       is a stub), so `pulsesink` it is. audioconvert/audioresample cost
#       nothing when the format already matches and save the pipeline from
#       failing to negotiate when it doesn't.
# Overridable at runtime with KINAMP_SINK, which is the knob to reach for if a
# device turns out to want alsasink or autoaudiosink instead.
if(KINAMP_KOBO)
    set(KINAMP_AUDIO_SINK_DEFAULT "audioconvert ! audioresample ! pulsesink")
else()
    set(KINAMP_AUDIO_SINK_DEFAULT "mixersink")
endif()
set(KINAMP_AUDIO_SINK "${KINAMP_AUDIO_SINK_DEFAULT}" CACHE STRING
    "GStreamer pipeline tail that consumes the decoded PCM")
message(STATUS "Audio sink: ${KINAMP_AUDIO_SINK}")

# liblipc only exists on the Kindle; without it the GTK player cannot be built.
# KinAMP-minimal only needs it for the Bluetooth keepalive and still builds
# without it, so there it is optional rather than a precondition.
find_library(LIPC_LIBRARY NAMES lipc)

option(KINAMP_CHECK_ABI "Fail the build if a binary needs a newer libstdc++ than the device has" ON)
set(KINAMP_MAX_GLIBCXX "3.4.28" CACHE STRING "Highest GLIBCXX version the target device provides")

function(kinamp_check_abi target)
    if(KINAMP_CHECK_ABI AND CMAKE_CROSSCOMPILING AND CMAKE_READELF)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_glibcxx.sh
                    ${CMAKE_READELF} ${KINAMP_MAX_GLIBCXX} $<TARGET_FILE:${target}>
            VERBATIM)
    endif()
endfunction()

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
        station_db.cpp
        ${MPEG4_SOURCES}
    )

    target_include_directories(${PROJECT_NAME} PRIVATE
        .
        mpeg4
        /usr/include
    )

    target_compile_definitions(KinAMP PRIVATE $<$<BOOL:${GST10}>:GST10>)

    target_compile_definitions(${PROJECT_NAME} PRIVATE
        KINAMP_VERSION="${PROJECT_VERSION}"
        KINAMP_AUDIO_SINK="${KINAMP_AUDIO_SINK}"
    )

    target_link_libraries(${PROJECT_NAME} PRIVATE
        PkgConfig::GTK
        PkgConfig::XML
        Threads::Threads
        PkgConfig::GST
        miniaudio
        gthread-2.0
        ${LIPC_LIBRARY}
        faad
        dl
        rt
    )

    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
    kinamp_check_abi(${PROJECT_NAME})
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
)

target_link_libraries(KinAMP-minimal PRIVATE
    PkgConfig::GLIB
    PkgConfig::GST
    Threads::Threads
    miniaudio
    faad
    dl
    rt
)
target_compile_definitions(KinAMP-minimal PRIVATE $<$<BOOL:${GST10}>:GST10>)
target_compile_definitions(KinAMP-minimal PRIVATE
    KINAMP_AUDIO_SINK="${KINAMP_AUDIO_SINK}")

if(LIPC_LIBRARY)
    target_compile_definitions(KinAMP-minimal PRIVATE KINAMP_HAVE_LIPC=1)
    target_link_libraries(KinAMP-minimal PRIVATE ${LIPC_LIBRARY})
else()
    message(STATUS "KinAMP: liblipc not found, KinAMP-minimal built without the Bluetooth keepalive")
endif()

kinamp_check_abi(KinAMP-minimal)

add_executable(radio_cli radio_cli.cpp station_db.cpp)
target_compile_options(radio_cli PRIVATE -Wall -Wextra)
kinamp_check_abi(radio_cli)
