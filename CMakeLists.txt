cmake_minimum_required(VERSION 3.20)
project(VoxGuard VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── WebView2 SDK (NuGet package = a zip archive) ──────────────────
# Rename to .zip so CMake's extractor recognises it.
FetchContent_Declare(
  webview2_sdk
  URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2792.45"
  DOWNLOAD_NAME "webview2.zip"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(webview2_sdk)

# ── miniaudio (single-header, no extraction step needed) ──────────
set(MINIAUDIO_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/miniaudio-src")
file(MAKE_DIRECTORY "${MINIAUDIO_INCLUDE_DIR}")
if(NOT EXISTS "${MINIAUDIO_INCLUDE_DIR}/miniaudio.h")
  message(STATUS "Downloading miniaudio.h...")
  file(DOWNLOAD
    "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
    "${MINIAUDIO_INCLUDE_DIR}/miniaudio.h"
    SHOW_PROGRESS
  )
endif()

# ── Generate .rc with absolute paths to UI source files ────────────
# The template uses @CMAKE_SOURCE_DIR@ so rc.exe can find the files
# regardless of the build working directory.
configure_file(voxguard.rc.in "${CMAKE_BINARY_DIR}/voxguard.rc" @ONLY)

# ── Main executable ────────────────────────────────────────────────
# WIN32 suppresses the console window (this is a GUI app)
add_executable(VoxGuard WIN32
  src/main.cpp
  "${CMAKE_BINARY_DIR}/voxguard.rc"
)

target_include_directories(VoxGuard PRIVATE
  "${webview2_sdk_SOURCE_DIR}/build/native/include"
  "${MINIAUDIO_INCLUDE_DIR}"
)

target_link_libraries(VoxGuard PRIVATE
  "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
  Shlwapi
  Ole32
  OleAut32
  winmm      # miniaudio WinMM audio backend
  AvRt       # MMCSS: lets miniaudio raise audio thread to real-time priority
)

# Copy the ui/ folder next to VoxGuard.exe on every build.
# The exe uses a file:/// URL pointing there, so assets must travel with it.
add_custom_command(TARGET VoxGuard POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/ui"
    "$<TARGET_FILE_DIR:VoxGuard>/ui"
  COMMENT "Syncing ui/ assets to output directory"
)
