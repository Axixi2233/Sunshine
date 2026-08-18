# HIDMaestro virtual DualSense helper

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    message(STATUS "HIDMaestro DualSense support is unavailable for ${CMAKE_SYSTEM_PROCESSOR}")
    return()
endif()

set(HIDMAESTRO_VERSION "1.6.1")
set(HIDMAESTRO_ARCHIVE_SHA256 "00145c23d9838be6089389ce58b3fd2b6766fa9bc0f1f3c60a3c885361b53c34")
set(HIDMAESTRO_RELEASE_URL
        "https://github.com/hifihedgehog/HIDMaestro/releases/download/v${HIDMAESTRO_VERSION}/HIDMaestro-v${HIDMAESTRO_VERSION}.zip")
set(HIDMAESTRO_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps/hidmaestro-v${HIDMAESTRO_VERSION}")
set(HIDMAESTRO_ARCHIVE "${HIDMAESTRO_DEPS_DIR}/HIDMaestro-v${HIDMAESTRO_VERSION}.zip")
set(HIDMAESTRO_EXTRACT_DIR "${HIDMAESTRO_DEPS_DIR}/release")
set(HIDMAESTRO_SDK_DIR "${HIDMAESTRO_DEPS_DIR}/sdk")

file(MAKE_DIRECTORY "${HIDMAESTRO_DEPS_DIR}")
set(_hidmaestro_download TRUE)
if(EXISTS "${HIDMAESTRO_ARCHIVE}")
    file(SHA256 "${HIDMAESTRO_ARCHIVE}" _hidmaestro_existing_sha256)
    if(_hidmaestro_existing_sha256 STREQUAL HIDMAESTRO_ARCHIVE_SHA256)
        set(_hidmaestro_download FALSE)
    endif()
endif()

if(_hidmaestro_download)
    message(STATUS "Downloading HIDMaestro v${HIDMAESTRO_VERSION}")
    file(DOWNLOAD
            "${HIDMAESTRO_RELEASE_URL}"
            "${HIDMAESTRO_ARCHIVE}"
            SHOW_PROGRESS
            EXPECTED_HASH "SHA256=${HIDMAESTRO_ARCHIVE_SHA256}"
            TIMEOUT 600)
endif()

set(_hidmaestro_core "${HIDMAESTRO_EXTRACT_DIR}/HIDMaestro.Core.dll")
set(_hidmaestro_windows_sdk "${HIDMAESTRO_EXTRACT_DIR}/HIDMaestroTest/Microsoft.Windows.SDK.NET.dll")
set(_hidmaestro_winrt "${HIDMAESTRO_EXTRACT_DIR}/HIDMaestroTest/WinRT.Runtime.dll")
if(NOT EXISTS "${_hidmaestro_core}" OR
        NOT EXISTS "${_hidmaestro_windows_sdk}" OR
        NOT EXISTS "${_hidmaestro_winrt}")
    file(REMOVE_RECURSE "${HIDMAESTRO_EXTRACT_DIR}")
    file(MAKE_DIRECTORY "${HIDMAESTRO_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${HIDMAESTRO_ARCHIVE}" DESTINATION "${HIDMAESTRO_EXTRACT_DIR}")
endif()

file(MAKE_DIRECTORY "${HIDMAESTRO_SDK_DIR}")
configure_file("${_hidmaestro_core}" "${HIDMAESTRO_SDK_DIR}/HIDMaestro.Core.dll" COPYONLY)
configure_file("${_hidmaestro_windows_sdk}" "${HIDMAESTRO_SDK_DIR}/Microsoft.Windows.SDK.NET.dll" COPYONLY)
configure_file("${_hidmaestro_winrt}" "${HIDMAESTRO_SDK_DIR}/WinRT.Runtime.dll" COPYONLY)

set(DOTNET_EXECUTABLE "" CACHE FILEPATH "Path to the .NET 10 dotnet executable")
if(NOT DOTNET_EXECUTABLE)
    find_program(DOTNET_EXECUTABLE dotnet)
endif()
if(NOT DOTNET_EXECUTABLE)
    message(FATAL_ERROR ".NET 10 SDK is required to build the HIDMaestro helper. Set DOTNET_EXECUTABLE.")
endif()

execute_process(
        COMMAND "${DOTNET_EXECUTABLE}" --list-sdks
        OUTPUT_VARIABLE _hidmaestro_dotnet_sdks
        RESULT_VARIABLE _hidmaestro_dotnet_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _hidmaestro_dotnet_result EQUAL 0 OR NOT _hidmaestro_dotnet_sdks MATCHES "(^|\n)10\\.")
    message(FATAL_ERROR ".NET 10 SDK is required to build the HIDMaestro helper. Found:\n${_hidmaestro_dotnet_sdks}")
endif()

set(HIDMAESTRO_HOST_PROJECT "${CMAKE_SOURCE_DIR}/tools/hidmaestro-host/sunshine-hidmaestro-host.csproj")
set(HIDMAESTRO_HOST_SOURCE "${CMAKE_SOURCE_DIR}/tools/hidmaestro-host/Program.cs")
set(HIDMAESTRO_HOST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tools")
set(HIDMAESTRO_HOST_EXECUTABLE "${HIDMAESTRO_HOST_OUTPUT_DIR}/sunshine-hidmaestro-host.exe")
set(HIDMAESTRO_HOST_INTERMEDIATE_DIR "${CMAKE_BINARY_DIR}/hidmaestro-host")

add_custom_command(
        OUTPUT "${HIDMAESTRO_HOST_EXECUTABLE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HIDMAESTRO_HOST_OUTPUT_DIR}"
        COMMAND "${DOTNET_EXECUTABLE}" publish "${HIDMAESTRO_HOST_PROJECT}"
                --configuration Release
                --runtime win-x64
                --output "${HIDMAESTRO_HOST_OUTPUT_DIR}"
                --nologo
                "-p:HIDMaestroSdkDir=${HIDMAESTRO_SDK_DIR}"
                "-p:BaseIntermediateOutputPath=${HIDMAESTRO_HOST_INTERMEDIATE_DIR}/obj/"
                "-p:BaseOutputPath=${HIDMAESTRO_HOST_INTERMEDIATE_DIR}/bin/"
        DEPENDS
                "${HIDMAESTRO_HOST_PROJECT}"
                "${HIDMAESTRO_HOST_SOURCE}"
                "${HIDMAESTRO_SDK_DIR}/HIDMaestro.Core.dll"
                "${HIDMAESTRO_SDK_DIR}/Microsoft.Windows.SDK.NET.dll"
                "${HIDMAESTRO_SDK_DIR}/WinRT.Runtime.dll"
        COMMENT "Publishing Sunshine HIDMaestro DualSense helper"
        VERBATIM)

add_custom_target(hidmaestro-host DEPENDS "${HIDMAESTRO_HOST_EXECUTABLE}")
list(APPEND SUNSHINE_TARGET_DEPENDENCIES hidmaestro-host)

set(HIDMAESTRO_HOST_EXECUTABLE "${HIDMAESTRO_HOST_EXECUTABLE}" CACHE INTERNAL "HIDMaestro helper executable")
set(HIDMAESTRO_LICENSE_FILE "${HIDMAESTRO_EXTRACT_DIR}/LICENSE" CACHE INTERNAL "HIDMaestro license")
set(HIDMAESTRO_NOTICES_FILE "${HIDMAESTRO_EXTRACT_DIR}/THIRD-PARTY-NOTICES.txt" CACHE INTERNAL "HIDMaestro notices")
