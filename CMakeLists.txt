cmake_minimum_required(VERSION 3.24)
project(P2PCall VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(P2P_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network DBus Gui Quick ShaderTools Test)
qt_standard_project_setup(REQUIRES 6.7)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
# Prefer a local libopus built with DRED (tools/build-opus-dred.sh) when present.
set(P2P_OPUS_PREFIX "${CMAKE_SOURCE_DIR}/build/third_party/opus" CACHE PATH "libopus with DRED")
if(EXISTS "${P2P_OPUS_PREFIX}/lib/pkgconfig/opus.pc")
    set(ENV{PKG_CONFIG_PATH} "${P2P_OPUS_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    message(STATUS "Using libopus from ${P2P_OPUS_PREFIX}")
endif()
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus>=1.5)
pkg_check_modules(WEBRTC_APM IMPORTED_TARGET webrtc-audio-processing-1)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio)

# ---------------------------------------------------------------------------
# Core library: everything except the GUI entry point, so tests can link it.
# ---------------------------------------------------------------------------
add_library(p2pcore STATIC
    src/core/clock.h
    src/net/crypto.h            src/net/crypto.cpp
    src/net/control_channel.h   src/net/control_channel.cpp
    src/net/discovery.h         src/net/discovery.cpp
    src/net/media_transport.h   src/net/media_transport.cpp
    src/net/fec.h                 src/net/fec.cpp
    src/net/cc/transport_feedback.h    src/net/cc/transport_feedback.cpp
    src/net/cc/congestion_controller.h src/net/cc/congestion_controller.cpp
    src/audio/jitter_buffer.h   src/audio/jitter_buffer.cpp
    src/audio/drift_resampler.h src/audio/drift_resampler.cpp
    src/audio/opus_codec.h      src/audio/opus_codec.cpp
    src/audio/audio_processor.h src/audio/audio_processor.cpp
    src/audio/audio_io.h        src/audio/audio_io.cpp
    src/audio/audio_pipeline.h  src/audio/audio_pipeline.cpp
    src/app/media_session.h     src/app/media_session.cpp
    src/media/codec_caps.h      src/media/codec_caps.cpp
    src/video/codec_probe.h
    src/video/video_frame.h       src/video/video_frame.cpp
    src/video/video_source.h      src/video/test_pattern.cpp
    src/video/video_packetizer.h  src/video/video_packetizer.cpp
    src/video/video_encoder.h     src/video/video_encoder.cpp
    src/video/hevc_headers.h      src/video/hevc_headers.cpp
    src/video/window_fec.h        src/video/window_fec.cpp
    src/video/video_decoder.h     src/video/video_decoder.cpp
    src/video/rate_ladder.h       src/video/rate_ladder.cpp
    src/video/latency_stages.cpp
    src/video/flash_source.cpp
    src/video/video_stream.h      src/video/video_stream.cpp
    src/app/call_controller.h   src/app/call_controller.cpp
)
target_include_directories(p2pcore PUBLIC src)
target_link_libraries(p2pcore
    PUBLIC  Qt6::Core Qt6::Network PkgConfig::SODIUM
    PRIVATE PkgConfig::FFMPEG PkgConfig::OPUS)
target_compile_options(p2pcore PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(p2pcore PUBLIC Threads::Threads)

if(WEBRTC_APM_FOUND)
    target_compile_definitions(p2pcore PRIVATE P2P_HAVE_WEBRTC_APM)
    target_link_libraries(p2pcore PRIVATE PkgConfig::WEBRTC_APM)
    message(STATUS "WebRTC audio processing: ${WEBRTC_APM_VERSION}")
else()
    message(WARNING "webrtc-audio-processing-1 not found: echo cancellation disabled")
endif()

if(MINIAUDIO_INCLUDE_DIR)
    # miniaudio's implementation lives in its own TU (third-party code, quiet warnings).
    target_sources(p2pcore PRIVATE src/audio/audio_device_miniaudio.cpp)
    target_include_directories(p2pcore SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR})
    target_link_libraries(p2pcore PRIVATE ${CMAKE_DL_LIBS} m)
    set_source_files_properties(src/audio/audio_device_miniaudio.cpp PROPERTIES COMPILE_OPTIONS "-w")
    if(APPLE)
        target_link_libraries(p2pcore PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
    endif()
    message(STATUS "miniaudio: ${MINIAUDIO_INCLUDE_DIR}")
else()
    target_sources(p2pcore PRIVATE src/audio/audio_device_none.cpp)
    message(WARNING "miniaudio.h not found: only the simulated audio device is available")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva libva-drm)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_sources(p2pcore PRIVATE src/video/codec_probe_linux.cpp src/video/camera_v4l2.cpp src/video/jpeg_decode.cpp src/video/vaapi_device.cpp
                                   src/video/hevc_vaapi_encoder.cpp
                                   src/video/screen_capture_pipewire.cpp)
    target_link_libraries(p2pcore PRIVATE PkgConfig::LIBVA PkgConfig::PIPEWIRE Qt6::DBus)
    # SPA's C headers use designated initialisers and casts g++ warns about.
    set_source_files_properties(src/video/screen_capture_pipewire.cpp PROPERTIES COMPILE_OPTIONS
        "-Wno-missing-field-initializers;-Wno-pedantic")
    # Per-pixel colour conversion on the camera path: optimise even in Debug.
    set_source_files_properties(src/video/jpeg_decode.cpp PROPERTIES COMPILE_OPTIONS "-O3")
else()
    # macOS VideoToolbox probe arrives in the macOS port phase.
    target_sources(p2pcore PRIVATE src/video/codec_probe_generic.cpp src/video/camera_none.cpp
                                   src/video/screen_none.cpp)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
qt_add_executable(p2pcall src/app/main.cpp)
qt_add_qml_module(p2pcall
    URI P2PCall
    VERSION 1.0
    QML_FILES qml/Main.qml
    SOURCES src/app/video_view.h src/app/video_view.cpp)
qt_add_shaders(p2pcall "p2pcall_shaders"
    PREFIX "/"
    FILES shaders/nv12.vert shaders/nv12.frag)
# QRhi (used by the video renderer) is semi-public: on some distros its
# headers only come with the private dev package / GuiPrivate target.
# (Qt 6.8 defines Qt6::GuiPrivate with Gui; 6.9+ makes it a separate component.)
if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()
target_link_libraries(p2pcall PRIVATE p2pcore Qt6::Gui Qt6::GuiPrivate Qt6::Quick)
target_include_directories(p2pcall PRIVATE src/app) # qmltyperegistrar includes headers by bare name
target_compile_options(p2pcall PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# Glass-to-glass latency probe (tools/latency_probe/main.cpp).
qt_add_executable(latency_probe tools/latency_probe/main.cpp)
target_link_libraries(latency_probe PRIVATE p2pcore Qt6::Gui)
target_compile_options(latency_probe PRIVATE -O2) # the analysis is a brute-force lag search

if(P2P_BUILD_TESTS)
    enable_testing()
    foreach(t test_crypto test_negotiation test_jitter_buffer test_drift_resampler test_transport test_fec test_window_fec test_video_packetizer test_congestion test_call)
        qt_add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE p2pcore Qt6::Test)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        qt_add_executable(test_jpeg_decode tests/test_jpeg_decode.cpp)
        target_link_libraries(test_jpeg_decode PRIVATE p2pcore Qt6::Test PkgConfig::FFMPEG)
        add_test(NAME test_jpeg_decode COMMAND test_jpeg_decode)
        qt_add_executable(test_hevc_encoder tests/test_hevc_encoder.cpp)
        target_link_libraries(test_hevc_encoder PRIVATE p2pcore Qt6::Test)
        add_test(NAME test_hevc_encoder COMMAND test_hevc_encoder)
    endif()
endif()
