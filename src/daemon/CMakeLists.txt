set(AGENTSPHERE_DAEMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/daemon/main.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/cloud_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/cloud_tunnel.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/daemon_types.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/ffmpeg_video_encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/host_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/host_updater.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/kvm_doctor.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/llm_proxy.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/opus_audio_encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/remote_session.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/remote_webrtc.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/resource_monitor.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/rpc_server.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/runtime_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/vm_store.cpp
    ${CMAKE_SOURCE_DIR}/src/common/image_source.cpp
)

add_executable(agentsphered ${AGENTSPHERE_DAEMON_SOURCES})

option(AGENTSPHERE_FETCH_LIBDATACHANNEL "Fetch libdatachannel when no system package is available" ON)
option(AGENTSPHERE_ENABLE_LIBYUV "Enable libyuv pixel conversion fast path" ON)
# Release builds set this to pick up the static ffmpeg/x264/opus/libyuv/
# openssl/libcurl baked into packaging/build-base/Dockerfile.bullseye under
# /opt/tenbox-deps. Defaults to OFF so developer builds keep using the
# system shared libs.
option(AGENTSPHERE_STATIC_FFMPEG "Link ffmpeg/x264/opus/libyuv/openssl/libcurl statically from /opt/tenbox-deps" OFF)
if(AGENTSPHERE_STATIC_FFMPEG)
    # pkg-config queries against the static prefix must come first so
    # PkgConfig::FFMPEG / PkgConfig::OPUS resolve to the .a archives,
    # not to whatever the host happens to have installed for dev work.
    set(_tenbox_static_prefix "$ENV{AGENTSPHERE_DEPS_PREFIX}")
    if(NOT _tenbox_static_prefix)
        set(_tenbox_static_prefix "/opt/tenbox-deps")
    endif()
    set(ENV{PKG_CONFIG_PATH} "${_tenbox_static_prefix}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    # Ask pkg-config to expand --libs into the full transitive set so
    # libavcodec.a + libx264.a + libopus.a all land on the link line.
    set(PKG_CONFIG_USE_STATIC_LIBS TRUE)

    # Steer every find_package(OpenSSL) in the build tree (this file's
    # own call below, plus libdatachannel's usrsctp / libsrtp transitive
    # calls reached via FetchContent) at the static libssl.a / libcrypto.a
    # baked into the build-base image. Without OPENSSL_ROOT_DIR cmake
    # would happily mix in /usr/lib/x86_64-linux-gnu/libssl.so, which
    # defeats the whole point of building bullseye-compatible binaries
    # — bullseye only ships libssl1.1, while everything newer ships
    # libssl3, and our static archive sidesteps the version split.
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(OPENSSL_ROOT_DIR "${_tenbox_static_prefix}")
endif()

target_include_directories(agentsphered
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)

target_link_libraries(agentsphered
    PRIVATE
        tenbox_ipc
        nlohmann_json::nlohmann_json
        pthread
)

find_package(LibDataChannel QUIET)
if(NOT TARGET LibDataChannel::LibDataChannel)
    if(NOT AGENTSPHERE_FETCH_LIBDATACHANNEL)
        message(FATAL_ERROR "LibDataChannel is required. Enable AGENTSPHERE_FETCH_LIBDATACHANNEL or install libdatachannel.")
    endif()

    set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        v0.24.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libdatachannel)
    set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED} CACHE BOOL "" FORCE)
endif()

if(TARGET LibDataChannel::LibDataChannel)
    target_link_libraries(agentsphered PRIVATE LibDataChannel::LibDataChannel)
else()
    target_link_libraries(agentsphered PRIVATE datachannel)
endif()

find_package(PkgConfig REQUIRED)
if(AGENTSPHERE_STATIC_FFMPEG)
    # IMPORTED_TARGET respects the _STATIC suffix for static link lines.
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswresample)
    target_link_libraries(agentsphered PRIVATE PkgConfig::FFMPEG)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(agentsphered PRIVATE PkgConfig::OPUS)
else()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
    target_link_libraries(agentsphered PRIVATE PkgConfig::FFMPEG)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(agentsphered PRIVATE PkgConfig::OPUS)
endif()

# OpenSSL is required for the cloud tunnel TLS upgrade (wss://). Phase 3
# defaults to wss://my.tenbox.ai/api/device-tunnel; ws:// is kept for local
# dev and falls through the same code path with TLS disabled.
find_package(OpenSSL REQUIRED)
target_link_libraries(agentsphered PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# libcurl is used by the in-process LLM API reverse proxy (llm_proxy.cpp)
# to forward OpenAI-compatible requests from guest VMs to the upstream
# providers configured via host.llm_proxy.set, including SSE streaming
# responses (CURLOPT_WRITEFUNCTION).
#
# In the static release build we pick up the slimmed-down libcurl.a
# shipped in /opt/tenbox-deps. Going through pkg_check_modules (rather
# than find_package(CURL)) lets pkg-config's --static logic pull in
# whatever transitive system deps libcurl needs (here just openssl,
# because we built libcurl --without-* everything else). For dev
# builds we keep the existing CMake CURL find_package so a system
# libcurl4-openssl-dev still works without /opt/tenbox-deps present.
if(AGENTSPHERE_STATIC_FFMPEG)
    pkg_check_modules(STATIC_CURL REQUIRED IMPORTED_TARGET libcurl)
    target_link_libraries(agentsphered PRIVATE PkgConfig::STATIC_CURL)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(agentsphered PRIVATE CURL::libcurl)
endif()

if(AGENTSPHERE_ENABLE_LIBYUV)
    if(AGENTSPHERE_STATIC_FFMPEG)
        # Match the prefix the build-base image installs into. cmake's
        # find_library prefers shared over static when both libyuv.so
        # and libyuv.a sit in the same dir (libyuv's cmake install
        # produces both even with BUILD_SHARED_LIBS=OFF); without
        # pinning to .a, agentsphered ends up DT_NEEDED on libyuv.so which
        # is absent on the target host and the daemon refuses to start.
        find_path(LIBYUV_INCLUDE_DIR NAMES libyuv.h
            HINTS ${_tenbox_static_prefix}/include
            NO_DEFAULT_PATH)
        set(LIBYUV_LIBRARY "${_tenbox_static_prefix}/lib/libyuv.a"
            CACHE FILEPATH "Path to static libyuv.a" FORCE)
        if(NOT EXISTS "${LIBYUV_LIBRARY}")
            message(FATAL_ERROR "AGENTSPHERE_STATIC_FFMPEG=ON but ${LIBYUV_LIBRARY} not present")
        endif()
    else()
        find_path(LIBYUV_INCLUDE_DIR NAMES libyuv.h)
        find_library(LIBYUV_LIBRARY NAMES yuv libyuv)
    endif()
    if(LIBYUV_INCLUDE_DIR AND LIBYUV_LIBRARY)
        target_compile_definitions(agentsphered PRIVATE AGENTSPHERE_ENABLE_LIBYUV=1)
        target_include_directories(agentsphered PRIVATE ${LIBYUV_INCLUDE_DIR})
        target_link_libraries(agentsphered PRIVATE ${LIBYUV_LIBRARY})
    else()
        message(FATAL_ERROR "libyuv is required for the BGRA->YUV slice conversion path; install libyuv-dev or disable AGENTSPHERE_ENABLE_LIBYUV explicitly.")
    endif()
endif()
