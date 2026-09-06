cmake_minimum_required(VERSION 3.25)

project(FastRecord VERSION 0.1.0 LANGUAGES CXX)

set(FASTRECORD_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown by the app and installer")
set(FASTRECORD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FASTRECORD_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/BuildVersion.h.in"
    "${FASTRECORD_GENERATED_DIR}/BuildVersion.h"
    @ONLY
)
set(FASTRECORD_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/fast-record-icon.ico")
set(FASTRECORD_RESOURCE_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/resource.h")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/FastRecord.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FastRecord.rc"
    @ONLY
)

include(FetchContent)

set(FASTRECORD_WEBVIEW2_VERSION "1.0.4191.47" CACHE STRING "WebView2 SDK version")
FetchContent_Declare(
    webview2
    URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${FASTRECORD_WEBVIEW2_VERSION}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(webview2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(fast-record WIN32
    src/main.cpp
    src/app/AppController.cpp
    src/app/AppSettings.cpp
    src/platform/CaptureSelector.cpp
    src/platform/TrayIcon.cpp
    src/recording/AmfProbe.cpp
    src/recording/MediaFoundationBackend.cpp
    src/recording/GraphicsCapture.cpp
    src/recording/NvencBackend.cpp
    src/recording/NvencProbe.cpp
    src/recording/RecorderController.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/FastRecord.rc
    src/ui/WebViewHost.cpp
)

target_include_directories(fast-record PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FASTRECORD_GENERATED_DIR}
    ${webview2_SOURCE_DIR}/build/native/include
)

target_compile_definitions(fast-record PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(fast-record PRIVATE /W4 /permissive- /utf-8)
endif()

target_link_libraries(fast-record PRIVATE
    ${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib
    advapi32
    d3d11
    windowsapp
    gdi32
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    oleaut32
    shell32
    shlwapi
    user32
    version
)

add_executable(capture-smoke EXCLUDE_FROM_ALL
    tests/CaptureSmoke.cpp
    src/recording/MediaFoundationBackend.cpp
    src/recording/GraphicsCapture.cpp
)
target_include_directories(capture-smoke PRIVATE src)
target_compile_definitions(capture-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(capture-smoke PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(capture-smoke PRIVATE d3d11 windowsapp mf mfplat mfreadwrite mfuuid ole32 user32)

# NVENC is intentionally loaded at runtime. The SDK headers are only needed
# when compiling the optional capability probe and encoder backend.
set(FASTRECORD_NVENC_SDK_DIR "" CACHE PATH "NVIDIA Video Codec SDK directory")
option(FASTRECORD_FETCH_ENCODER_HEADERS
    "Fetch pinned NVENC headers when the local SDK is not installed"
    ON
)
find_path(FASTRECORD_NVENC_INCLUDE_DIR
    NAMES nvEncodeAPI.h
    HINTS
        "${FASTRECORD_NVENC_SDK_DIR}"
        "$ENV{FASTRECORD_NVENC_SDK_DIR}"
    PATH_SUFFIXES Interface include Include
)

if(NOT FASTRECORD_NVENC_INCLUDE_DIR AND FASTRECORD_FETCH_ENCODER_HEADERS)
    # The header-only package is used only at build time. The driver API is
    # still loaded dynamically, so the released app does not ship NVIDIA DLLs.
    FetchContent_Declare(
        nvcodec_headers
        GIT_REPOSITORY https://github.com/FFmpeg/nv-codec-headers.git
        GIT_TAG 0a6fba9a2820628b8103464f4c8753ee05838baa
        GIT_SHALLOW TRUE
    )
    FetchContent_Populate(nvcodec_headers)
    set(FASTRECORD_NVENC_INCLUDE_DIR
        "${nvcodec_headers_SOURCE_DIR}/include/ffnvcodec"
        CACHE PATH "NVIDIA Video Codec SDK include directory"
        FORCE
    )
endif()

# Older configurations may have cached the repository root instead of the
# directory that directly contains nvEncodeAPI.h.
if(FASTRECORD_NVENC_INCLUDE_DIR AND
   NOT EXISTS "${FASTRECORD_NVENC_INCLUDE_DIR}/nvEncodeAPI.h" AND
   EXISTS "${FASTRECORD_NVENC_INCLUDE_DIR}/ffnvcodec/nvEncodeAPI.h")
    set(FASTRECORD_NVENC_INCLUDE_DIR
        "${FASTRECORD_NVENC_INCLUDE_DIR}/ffnvcodec"
        CACHE PATH "NVIDIA Video Codec SDK include directory"
        FORCE
    )
endif()

if(FASTRECORD_NVENC_INCLUDE_DIR)
    set(FASTRECORD_HAS_NVENC 1)
    target_include_directories(fast-record PRIVATE ${FASTRECORD_NVENC_INCLUDE_DIR})
    target_compile_definitions(fast-record PRIVATE FASTRECORD_HAS_NVENC=1)
else()
    set(FASTRECORD_HAS_NVENC 0)
    target_compile_definitions(fast-record PRIVATE FASTRECORD_HAS_NVENC=0)
endif()

add_executable(nvenc-smoke EXCLUDE_FROM_ALL
    tests/NvencSmoke.cpp
    src/recording/NvencBackend.cpp
    src/recording/GraphicsCapture.cpp
)
target_include_directories(nvenc-smoke PRIVATE src)
if(FASTRECORD_NVENC_INCLUDE_DIR)
    target_include_directories(nvenc-smoke PRIVATE ${FASTRECORD_NVENC_INCLUDE_DIR})
endif()
target_compile_definitions(nvenc-smoke PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    FASTRECORD_HAS_NVENC=${FASTRECORD_HAS_NVENC}
)
if(MSVC)
    target_compile_options(nvenc-smoke PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(nvenc-smoke PRIVATE d3d11 windowsapp mf mfplat mfreadwrite mfuuid ole32 user32)

# AMF is also optional. The headers are used at build time; the AMD runtime is
# loaded from the installed display driver at runtime.
set(FASTRECORD_AMF_SDK_DIR "" CACHE PATH "AMD Advanced Media Framework SDK directory")
find_path(FASTRECORD_AMF_INCLUDE_DIR
    NAMES core/Factory.h
    HINTS
        "${FASTRECORD_AMF_SDK_DIR}"
        "$ENV{FASTRECORD_AMF_SDK_DIR}"
    PATH_SUFFIXES amf/public/include public/include
)

if(FASTRECORD_AMF_INCLUDE_DIR)
    target_include_directories(fast-record PRIVATE ${FASTRECORD_AMF_INCLUDE_DIR})
    target_compile_definitions(fast-record PRIVATE FASTRECORD_HAS_AMF=1)
else()
    target_compile_definitions(fast-record PRIVATE FASTRECORD_HAS_AMF=0)
endif()
