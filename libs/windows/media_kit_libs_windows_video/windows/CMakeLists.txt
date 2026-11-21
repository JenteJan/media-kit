# This file is a part of media_kit (https://github.com/media-kit/media-kit).
#
# Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
# All rights reserved.
# Use of this source code is governed by MIT license that can be found in the LICENSE file.

cmake_minimum_required(VERSION 3.14)

# This option is read by the other packages which are part of package:media_kit.
option(MEDIA_KIT_LIBS_AVAILABLE "package:media_kit libraries are available." ON)

set(PROJECT_NAME "media_kit_libs_windows_video")
project(${PROJECT_NAME} LANGUAGES CXX)

# Opt in to policies supported by Flutter's minimum CMake version without
# requiring policies introduced by newer CMake releases.
cmake_policy(VERSION 3.14...3.25)

# ------------------------------------------------------------------------------
function(download_and_verify_sha256 url sha256 locationForArchive)
  if(EXISTS "${locationForArchive}")
    file(SHA256 "${locationForArchive}" ARCHIVE_SHA256)

    if(NOT "${sha256}" STREQUAL "${ARCHIVE_SHA256}")
      file(REMOVE "${locationForArchive}")
      message(STATUS "SHA-256 mismatch. File deleted.")
    endif()
  endif()

  if(NOT EXISTS "${locationForArchive}")
    message(STATUS "Downloading archive from ${url}...")
    file(
      DOWNLOAD
      "${url}"
      "${locationForArchive}"
      EXPECTED_HASH "SHA256=${sha256}"
      STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_STATUS_CODE)
    list(GET DOWNLOAD_STATUS 1 DOWNLOAD_STATUS_MESSAGE)

    if(NOT DOWNLOAD_STATUS_CODE EQUAL 0)
      file(REMOVE "${locationForArchive}")
      message(
        FATAL_ERROR
        "Failed to download ${url}: ${DOWNLOAD_STATUS_MESSAGE}"
      )
    endif()
  endif()

  file(SHA256 "${locationForArchive}" ARCHIVE_SHA256)

  if(NOT "${sha256}" STREQUAL "${ARCHIVE_SHA256}")
    message(
      FATAL_ERROR
      "${locationForArchive} SHA-256 verification failed."
    )
  endif()

  message(STATUS "${locationForArchive} SHA-256 verification successful.")
endfunction()

# Find 7zip for extracting the libmpv archive.
find_program(SEVENZ_EXECUTABLE NAMES 7z 7za)

# libmpv 0.42.0 release artifacts.
set(LIBMPV_RELEASE "20261123")
set(LIBMPV_BUILD_DATE "20261123")
set(LIBMPV_REVISION_SHORT "b0841e8c4d")

if("${FLUTTER_TARGET_PLATFORM}" STREQUAL "windows-x64")
  set(LIBMPV_ARCH "x86_64")
  set(
    LIBMPV_SHA256
    "558c47ce08d39bd27d060363c57907604fde7c6d5f7b09e50dc7b04da2a46300"
  )
elseif("${FLUTTER_TARGET_PLATFORM}" STREQUAL "windows-arm64")
  set(LIBMPV_ARCH "aarch64")
  set(
    LIBMPV_SHA256
    "2af0781b6e90dbcdea912437136d7541cffa45b29af535475ed1ad6176b0d786"
  )
else()
  message(
    FATAL_ERROR
    "Unsupported FLUTTER_TARGET_PLATFORM for libmpv: "
    "${FLUTTER_TARGET_PLATFORM}. Expected windows-x64 or windows-arm64."
  )
endif()

set(
  LIBMPV
  "mpv-dev-${LIBMPV_ARCH}-${LIBMPV_BUILD_DATE}-git-${LIBMPV_REVISION_SHORT}.7z"
)
set(
  LIBMPV_URL
  "https://github.com/Carapacik/libmpv-win32-video-cmake/releases/download/${LIBMPV_RELEASE}/${LIBMPV}"
)

set(LIBMPV_ARCHIVE "${CMAKE_BINARY_DIR}/${LIBMPV}")
set(LIBMPV_SRC "${CMAKE_BINARY_DIR}/libmpv")
set(LIBMPV_INCLUDE_DIR "${LIBMPV_SRC}/include/mpv")
set(LIBMPV_IMPORT_LIBRARY "${LIBMPV_SRC}/libmpv.dll.a")
set(LIBMPV_RUNTIME_LIBRARY "${LIBMPV_SRC}/libmpv-2.dll")

download_and_verify_sha256(
  "${LIBMPV_URL}"
  "${LIBMPV_SHA256}"
  "${LIBMPV_ARCHIVE}"
)

set(
  LIBMPV_REQUIRED_FILES
  "${LIBMPV_INCLUDE_DIR}/client.h"
  "${LIBMPV_INCLUDE_DIR}/render.h"
  "${LIBMPV_INCLUDE_DIR}/render_d3d11.h"
  "${LIBMPV_INCLUDE_DIR}/stream_cb.h"
  "${LIBMPV_IMPORT_LIBRARY}"
  "${LIBMPV_RUNTIME_LIBRARY}"
)

# Always recreate staging from the verified archive to prevent stale files.
message(STATUS "Extracting ${LIBMPV}...")
file(REMOVE_RECURSE "${LIBMPV_SRC}")
file(MAKE_DIRECTORY "${LIBMPV_SRC}")

if(SEVENZ_EXECUTABLE)
  message(STATUS "Using 7zip for extraction: ${SEVENZ_EXECUTABLE}")
  execute_process(
    COMMAND "${SEVENZ_EXECUTABLE}" x -y "${LIBMPV_ARCHIVE}"
    WORKING_DIRECTORY "${LIBMPV_SRC}"
    RESULT_VARIABLE LIBMPV_EXTRACT_RESULT
    OUTPUT_VARIABLE LIBMPV_EXTRACT_OUTPUT
    ERROR_VARIABLE LIBMPV_EXTRACT_ERROR
  )
else()
  message(STATUS "7zip not found, falling back to CMake tar")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xvf "${LIBMPV_ARCHIVE}"
    WORKING_DIRECTORY "${LIBMPV_SRC}"
    RESULT_VARIABLE LIBMPV_EXTRACT_RESULT
    OUTPUT_VARIABLE LIBMPV_EXTRACT_OUTPUT
    ERROR_VARIABLE LIBMPV_EXTRACT_ERROR
  )
endif()

if(NOT LIBMPV_EXTRACT_RESULT EQUAL 0)
  file(REMOVE_RECURSE "${LIBMPV_SRC}")
  message(
    FATAL_ERROR
    "Failed to extract ${LIBMPV_ARCHIVE}:\n"
    "${LIBMPV_EXTRACT_OUTPUT}\n${LIBMPV_EXTRACT_ERROR}"
  )
endif()

foreach(LIBMPV_REQUIRED_FILE IN LISTS LIBMPV_REQUIRED_FILES)
  if(NOT EXISTS "${LIBMPV_REQUIRED_FILE}")
    file(REMOVE_RECURSE "${LIBMPV_SRC}")
    message(
      FATAL_ERROR
      "libmpv archive is missing required file: ${LIBMPV_REQUIRED_FILE}"
    )
  endif()
endforeach()

# ------------------------------------------------------------------------------

# Flutter's generated plugin build depends on this target name.
set(PLUGIN_NAME "media_kit_libs_windows_video_plugin")

add_library(${PLUGIN_NAME} SHARED
  "include/media_kit_libs_windows_video/media_kit_libs_windows_video_plugin_c_api.h"
  "media_kit_libs_windows_video_plugin_c_api.cpp"
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  FLUTTER_PLUGIN_IMPL
  _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin)

set(
  media_kit_libs_windows_video_bundled_libraries
  "${LIBMPV_RUNTIME_LIBRARY}"
  PARENT_SCOPE
)
