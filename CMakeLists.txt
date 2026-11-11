cmake_minimum_required(VERSION 3.21)
project(AudioPluginHost VERSION 1.0.0 LANGUAGES CXX)

set(PLUGIN_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
if(PLUGIN_VERSION_PATCH STREQUAL "")
    set(PLUGIN_VERSION_PATCH 0)
endif()

set(PLUGIN_AUTHOR "Draugrath")
set(PLUGIN_COPYRIGHT "Copyright © 2026 ${PLUGIN_AUTHOR}.")
set(PLUGIN_DESCRIPTION "Audio Plugin Host for Skyrim SE and AE: VST3 effects on the game's sound, for SKSE Menu Framework.")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Keep local paths out of the binary.
set(EXTERN_DIR "${CMAKE_SOURCE_DIR}/extern")
set(LIB_DIR "${EXTERN_DIR}")
if(NOT EXISTS "${LIB_DIR}/CommonLibSSE-NG/CMakeLists.txt" AND EXISTS "${CMAKE_SOURCE_DIR}/../WaterOnSteroids/extern/CommonLibSSE-NG/CMakeLists.txt")
    set(LIB_DIR "${CMAKE_SOURCE_DIR}/../WaterOnSteroids/extern")
endif()
get_filename_component(LIB_DIR "${LIB_DIR}" REALPATH)

set(WOS_PATH_MAPS)
foreach(dir IN ITEMS "${LIB_DIR}" "${EXTERN_DIR}")
    file(TO_NATIVE_PATH "${dir}" native)
    list(APPEND WOS_PATH_MAPS "${native}=extern" "${dir}=extern")
endforeach()
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" WOS_NATIVE_SOURCE_DIR)
list(APPEND WOS_PATH_MAPS "${WOS_NATIVE_SOURCE_DIR}=${PROJECT_NAME}" "${CMAKE_SOURCE_DIR}=${PROJECT_NAME}")
if(DEFINED ENV{USERPROFILE})
    file(TO_NATIVE_PATH "$ENV{USERPROFILE}" WOS_NATIVE_USERPROFILE)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" WOS_CMAKE_USERPROFILE)
    list(APPEND WOS_PATH_MAPS "${WOS_NATIVE_USERPROFILE}=user" "${WOS_CMAKE_USERPROFILE}=user")
endif()
list(REMOVE_DUPLICATES WOS_PATH_MAPS)

if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/experimental:deterministic)
    foreach(map IN LISTS WOS_PATH_MAPS)
        add_compile_options("/pathmap:${map}")
    endforeach()
endif()
if(MSVC)
    add_link_options("$<$<CONFIG:Release>:/DEBUG:NONE>" /PDBALTPATH:%_PDB%)
endif()

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "Enable Skyrim SE" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Enable Skyrim AE" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Disable Skyrim VR" FORCE)
set(COMMONLIB_PREBUILT OFF CACHE BOOL "Disable all-runtime CommonLib prebuilt" FORCE)
set(COMMONLIB_PREBUILT_DIR "" CACHE PATH "Disable all-runtime CommonLib prebuilt" FORCE)
set(COMMONLIB_PREBUILT_MULTICONFIG OFF CACHE BOOL "Disable all-runtime CommonLib prebuilt" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

include(FetchContent)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

function(declare_dependency name folder repository tag)
    set(source_dir "${LIB_DIR}/${folder}")
    set(binary_dir "${CMAKE_BINARY_DIR}/${folder}")
    if(EXISTS "${source_dir}/CMakeLists.txt")
        message(STATUS "AudioPluginHost: using local ${name} from ${source_dir}")
        FetchContent_Declare(${name} SOURCE_DIR "${source_dir}" BINARY_DIR "${binary_dir}" ${ARGN})
    else()
        message(STATUS "AudioPluginHost: downloading ${name} ${tag}")
        FetchContent_Declare(${name}
            GIT_REPOSITORY "${repository}" GIT_TAG "${tag}" GIT_SHALLOW TRUE GIT_PROGRESS TRUE
            SOURCE_DIR "${source_dir}" BINARY_DIR "${binary_dir}" ${ARGN})
    endif()
endfunction()

declare_dependency(spdlog spdlog https://github.com/gabime/spdlog.git v1.16.0 OVERRIDE_FIND_PACKAGE)
declare_dependency(CommonLibSSE CommonLibSSE-NG https://github.com/alandtse/CommonLibSSE-NG.git v13.1.0)

find_package(fmt CONFIG REQUIRED)
FetchContent_MakeAvailable(spdlog)
set(_saved_skip "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES TRUE)
FetchContent_MakeAvailable(CommonLibSSE)
set(CMAKE_SKIP_INSTALL_RULES "${_saved_skip}")
unset(_saved_skip)
if(NOT COMMAND add_commonlibsse_plugin)
    include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
endif()

# VST3 SDK (MIT): only the hosting parts, as a static library. It is C++17 code.
set(VST3_SDK "${EXTERN_DIR}/vst3sdk")
if(NOT EXISTS "${VST3_SDK}/pluginterfaces/base/funknown.h")
    message(FATAL_ERROR "The VST3 SDK is missing: git clone --depth 1 https://github.com/steinbergmedia/vst3sdk.git extern/vst3sdk, then git submodule update --init --depth 1 base pluginterfaces public.sdk")
endif()
add_library(vst3hosting STATIC
    "${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/module_win32.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/processdata.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/plugprovider.cpp"
    "${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp"
    "${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp"
    "${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp"
    "${VST3_SDK}/public.sdk/source/common/memorystream.cpp"
    "${VST3_SDK}/public.sdk/source/common/commoniids.cpp"
    "${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp"
    "${VST3_SDK}/public.sdk/source/common/threadchecker_win32.cpp"
    "${VST3_SDK}/pluginterfaces/base/coreiids.cpp"
    "${VST3_SDK}/pluginterfaces/base/funknown.cpp"
    "${VST3_SDK}/pluginterfaces/base/ustring.cpp"
    "${VST3_SDK}/base/source/fobject.cpp"
    "${VST3_SDK}/base/source/fdebug.cpp"
    "${VST3_SDK}/base/source/baseiids.cpp"
    "${VST3_SDK}/base/source/updatehandler.cpp"
    "${VST3_SDK}/base/source/fstring.cpp"
    "${VST3_SDK}/base/thread/source/flock.cpp")
set_target_properties(vst3hosting PROPERTIES CXX_STANDARD 17)
target_include_directories(vst3hosting PUBLIC "${VST3_SDK}")
target_compile_definitions(vst3hosting PUBLIC RELEASE=1 NOMINMAX UNICODE _UNICODE)
target_compile_options(vst3hosting PRIVATE /W0)

set(SOURCES
    "${CMAKE_SOURCE_DIR}/src/Plugin.cpp"
    "${CMAKE_SOURCE_DIR}/src/AudioChain.h"
    "${CMAKE_SOURCE_DIR}/src/Host.cpp"
    "${CMAKE_SOURCE_DIR}/src/Host.h"
    "${CMAKE_SOURCE_DIR}/src/CursorGuard.cpp"
    "${CMAKE_SOURCE_DIR}/src/CursorGuard.h"
    "${CMAKE_SOURCE_DIR}/src/Rack.cpp"
    "${CMAKE_SOURCE_DIR}/src/Rack.h"
    "${CMAKE_SOURCE_DIR}/src/Vst3Host.cpp"
    "${CMAKE_SOURCE_DIR}/src/Vst3Host.h"
    "${CMAKE_SOURCE_DIR}/src/Settings.h"
    "${CMAKE_SOURCE_DIR}/src/FrameworkMenu.cpp"
    "${CMAKE_SOURCE_DIR}/src/FrameworkMenu.h"
    "${CMAKE_SOURCE_DIR}/include/PCH.h"
    "${CMAKE_SOURCE_DIR}/include/SKSEMenuFramework.h")

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "AudioPluginHost"
    AUTHOR "${PLUGIN_AUTHOR}"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
    SOURCES ${SOURCES})

configure_file("${CMAKE_SOURCE_DIR}/cmake/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h" @ONLY)
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc" "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include" "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src")
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/Vst3Host.cpp" "${CMAKE_SOURCE_DIR}/src/Rack.cpp" "${CMAKE_SOURCE_DIR}/src/CursorGuard.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog fmt::fmt vst3hosting ole32 user32 gdi32 shell32)

set(APH_DIST_PLUGIN_DIR "${CMAKE_SOURCE_DIR}/dist/AudioPluginHost/SKSE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APH_DIST_PLUGIN_DIR}/AudioPluginHost/VST3"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${APH_DIST_PLUGIN_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/packed/SKSE/Plugins/AudioPluginHost.ini" "${APH_DIST_PLUGIN_DIR}/AudioPluginHost.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/packed/SKSE/Plugins/AudioPluginHost/VST3/Put VST3 plugins here.txt" "${APH_DIST_PLUGIN_DIR}/AudioPluginHost/VST3/Put VST3 plugins here.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE" "${APH_DIST_PLUGIN_DIR}/AudioPluginHost-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/EXCEPTIONS.md" "${APH_DIST_PLUGIN_DIR}/AudioPluginHost-EXCEPTIONS.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" "${APH_DIST_PLUGIN_DIR}/AudioPluginHost-THIRD_PARTY_NOTICES.txt"
    VERBATIM)
