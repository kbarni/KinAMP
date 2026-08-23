cmake_minimum_required(VERSION 3.5)

# Keep in sync with the GIT release tag: the "Check for updates" dialog compares
# this against the tag name of the latest GitHub release.
project(KinAMP VERSION 2.9)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK IMPORTED_TARGET REQUIRED gtk+-2.0)
pkg_check_modules(XML IMPORTED_TARGET REQUIRED libxml-2.0)

find_package(Threads REQUIRED)

option(GST10 "Link against gstreamer-1.0 instead of gstreamer-0.10" OFF)
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
    PkgConfig::GTK
    PkgConfig::XML
    PkgConfig::GST
    Threads::Threads
    miniaudio
    faad
    dl
    rt
)
target_compile_definitions(KinAMP-minimal PRIVATE $<$<BOOL:${GST10}>:GST10>)

if(LIPC_LIBRARY)
    target_compile_definitions(KinAMP-minimal PRIVATE KINAMP_HAVE_LIPC=1)
    target_link_libraries(KinAMP-minimal PRIVATE ${LIPC_LIBRARY})
else()
    message(STATUS "KinAMP: liblipc not found, KinAMP-minimal built without the Bluetooth keepalive")
endif()

kinamp_check_abi(KinAMP-minimal)
