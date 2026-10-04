cmake_minimum_required(VERSION 3.21)

# Must precede project(). Ignored on other platforms.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(keyboard_chatter_filter
    VERSION 1.0.0
    DESCRIPTION "Filters keyboard switch chatter (accidental duplicate keystrokes)"
    HOMEPAGE_URL "https://github.com/Shan-e-Shafiq/keyboard-chatter-filter"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(KCF_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(KCF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(KCF_STATIC_LINK "Link the executable fully statically (Linux/musl release builds)" OFF)
set(KCF_SERVICE_ID "keyboard-chatter-filter" CACHE STRING
    "Name used for the launchd label, systemd unit and Windows service")
set(KCF_VERSION_SUFFIX "" CACHE STRING "Optional suffix appended to the version string (e.g. -dev)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # Static CRT: the service binary has no runtime dependencies to install.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------------------------
add_library(kcf_warnings INTERFACE)
if(MSVC)
    target_compile_options(kcf_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus
        $<$<BOOL:${KCF_WARNINGS_AS_ERRORS}>:/WX>)
    target_compile_definitions(kcf_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(kcf_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wno-sign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        $<$<BOOL:${KCF_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# ---------------------------------------------------------------------------------------------
# Hardening for the shipped binary
# ---------------------------------------------------------------------------------------------
add_library(kcf_hardening INTERFACE)
if(NOT MSVC)
    target_compile_options(kcf_hardening INTERFACE -fstack-protector-strong
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(kcf_hardening INTERFACE -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
    endif()
else()
    target_compile_options(kcf_hardening INTERFACE /GS /guard:cf)
    target_link_options(kcf_hardening INTERFACE /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
endif()

# ---------------------------------------------------------------------------------------------
# Core: platform-independent filtering logic. Must never include OS headers.
# ---------------------------------------------------------------------------------------------
add_library(kcf_core STATIC
    src/core/chatter_filter.cpp
    src/core/configuration.cpp
    src/core/filter_engine.cpp)
target_include_directories(kcf_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(kcf_core PRIVATE kcf_warnings)

# ---------------------------------------------------------------------------------------------
# Application support shared by all platforms (logging, CLI, status protocol)
# ---------------------------------------------------------------------------------------------
set(KCF_VERSION_STRING "${PROJECT_VERSION}${KCF_VERSION_SUFFIX}")
include(cmake/EmbedTemplates.cmake)
kcf_embed_templates(${PROJECT_BINARY_DIR}/generated/kcf/embedded_templates.h)
configure_file(src/common/build_info.h.in ${PROJECT_BINARY_DIR}/generated/kcf/build_info.h @ONLY)

add_library(kcf_common STATIC
    src/common/cli.cpp
    src/common/logging.cpp
    src/common/status.cpp
    src/common/text.cpp)
target_include_directories(kcf_common PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(kcf_common PUBLIC kcf_core PRIVATE kcf_warnings)
if(UNIX)
    target_sources(kcf_common PRIVATE
        src/common/posix/instance_lock.cpp
        src/common/posix/process.cpp
        src/common/posix/status_server.cpp
        src/common/posix/files.cpp)
endif()

# ---------------------------------------------------------------------------------------------
# Pure translation rules of every platform adapter (no OS headers), built everywhere so they are
# unit-tested on every CI runner.
# ---------------------------------------------------------------------------------------------
add_library(kcf_adapter_logic STATIC
    src/macos/event_conversion.cpp
    src/windows/key_mapping.cpp
    src/linux/device_classifier.cpp)
target_include_directories(kcf_adapter_logic PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(kcf_adapter_logic PUBLIC kcf_common PRIVATE kcf_warnings)

# ---------------------------------------------------------------------------------------------
# Platform adapters + executable
# ---------------------------------------------------------------------------------------------
add_executable(keyboard-chatter-filter src/app/main.cpp)
target_link_libraries(keyboard-chatter-filter PRIVATE kcf_warnings kcf_hardening)

if(APPLE)
    add_library(kcf_platform STATIC
        src/macos/keyboard_interceptor.cpp
        src/macos/keyboard_output.cpp
        src/macos/mach_clock.cpp
        src/macos/daemon.cpp
        src/macos/launch_agent.cpp
        src/macos/platform.cpp)
    target_link_libraries(kcf_platform
        PUBLIC kcf_common kcf_adapter_logic
        PRIVATE kcf_warnings "-framework ApplicationServices" "-framework CoreFoundation")
elseif(WIN32)
    add_library(kcf_platform STATIC
        src/windows/keyboard_interceptor.cpp
        src/windows/keyboard_output.cpp
        src/windows/agent.cpp
        src/windows/service.cpp
        src/windows/win_util.cpp
        src/windows/platform.cpp)
    target_compile_definitions(kcf_platform PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00)
    target_link_libraries(kcf_platform
        PUBLIC kcf_common kcf_adapter_logic
        PRIVATE kcf_warnings user32 advapi32 wtsapi32 userenv shell32 ole32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(kcf_platform STATIC
        src/linux/input_device.cpp
        src/linux/keyboard_interceptor.cpp
        src/linux/keyboard_output.cpp
        src/linux/daemon.cpp
        src/linux/platform.cpp)
    target_link_libraries(kcf_platform PUBLIC kcf_common kcf_adapter_logic PRIVATE kcf_warnings)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()
target_link_libraries(keyboard-chatter-filter PRIVATE kcf_platform)

if(MINGW)
    # wmain entry point, and no dependency on the MinGW runtime DLLs.
    target_link_options(keyboard-chatter-filter PRIVATE -municode -static)
elseif(KCF_STATIC_LINK)
    target_link_options(keyboard-chatter-filter PRIVATE -static)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(keyboard-chatter-filter PRIVATE -static-libstdc++ -static-libgcc)
endif()

include(GNUInstallDirs)
install(TARGETS keyboard-chatter-filter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
if(KCF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
