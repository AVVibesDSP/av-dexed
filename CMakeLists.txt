cmake_minimum_required(VERSION 3.22)
# The oldest macOS AV Dexed runs on (Apple silicon starts at 11). It must be set
# before project(); without it the build only runs on the builder's own macOS.
# FORCE fills an empty value left in an older build folder; a value the builder
# passes (-DCMAKE_OSX_DEPLOYMENT_TARGET=...) is kept.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(AVDexed VERSION 0.3.2)

option(DEXED_BUILD_TESTS "Build AV Dexed regression tests" OFF)
if(DEXED_BUILD_TESTS)
    enable_testing()
endif()

#Compile commands, useful for some IDEs like VS-Code
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# AV Dexed's own version (0.x = public preview); BUILD_ID overrides it.
if(NOT BUILD_ID)
    add_definitions("-DDEXED_ID=${PROJECT_VERSION}")
else()
    add_definitions("-DDEXED_ID=${BUILD_ID}")
endif()

# Release is universal unless the builder picks architectures (-DCMAKE_OSX_ARCHITECTURES=arm64).
# project() already left an empty CMAKE_OSX_ARCHITECTURES in the cache, so FORCE.
if (CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures" FORCE)
endif()

#static linking in Windows
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# include JUCE *AFTER* the MSVC runtime and so on is set up
set(DEXED_JUCE_PATH "${CMAKE_SOURCE_DIR}/libs/JUCE" CACHE STRING "Path to JUCE library source tree")

# Fixes from JUCE's develop branch not yet in the release used here, as patches
# (libs/juce-patches, see its README). Applied in order, once, to the JUCE
# files; configuring stops if they neither apply nor are applied already.
option(AVDEXED_JUCE_PATCHES "Apply libs/juce-patches to the JUCE sources" ON)
file(GLOB AVDEXED_JUCE_PATCH_FILES "${CMAKE_SOURCE_DIR}/libs/juce-patches/*.patch")
list(SORT AVDEXED_JUCE_PATCH_FILES)
if(AVDEXED_JUCE_PATCHES AND AVDEXED_JUCE_PATCH_FILES)
    find_package(Git REQUIRED)
    set(AVDEXED_JUCE_PATCHES_REVERSED ${AVDEXED_JUCE_PATCH_FILES})
    list(REVERSE AVDEXED_JUCE_PATCHES_REVERSED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check ${AVDEXED_JUCE_PATCHES_REVERSED}
                    WORKING_DIRECTORY "${DEXED_JUCE_PATH}" RESULT_VARIABLE AVDEXED_JUCE_PATCHED OUTPUT_QUIET ERROR_QUIET)
    if(NOT AVDEXED_JUCE_PATCHED EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace ${AVDEXED_JUCE_PATCH_FILES}
                        WORKING_DIRECTORY "${DEXED_JUCE_PATH}" RESULT_VARIABLE AVDEXED_JUCE_APPLIED ERROR_VARIABLE AVDEXED_JUCE_WHY)
        if(NOT AVDEXED_JUCE_APPLIED EQUAL 0)
            message(FATAL_ERROR "libs/juce-patches do not apply to ${DEXED_JUCE_PATH}: ${AVDEXED_JUCE_WHY}")
        endif()
        message(STATUS "Applied libs/juce-patches to JUCE")
    endif()
endif()
add_subdirectory(${DEXED_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)

#Adds all the module sources so they appear correctly in the IDE
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Enable Module Source Groups" ON)

#set any of these to "ON" if you want to build one of the juce examples
#or extras (Projucer/AudioPluginHost, etc):
option(JUCE_BUILD_EXTRAS "Build JUCE Extras" OFF)
option(JUCE_BUILD_EXAMPLES "Build JUCE Examples" OFF)

add_subdirectory(libs)
add_subdirectory(assets)
add_subdirectory(source)

# uncomment to enable warnings are errors,
# suppress warnings (or remove when fixed)
# or add -Wall, -Wextra or specific warnings as you see fit
if (APPLE)
target_compile_options(${PROJECT_NAME} PUBLIC
        #-Werror
        #-Wno-deprecated-declarations
        #-Wno-unused-value
        )
elseif(UNIX AND NOT APPLE)
target_compile_options(${PROJECT_NAME} PUBLIC
        #-Werror
        -Wno-deprecated-declarations
        -Wno-unused-value
        )
else()
target_compile_options(${PROJECT_NAME} PUBLIC
        #/WX # Warnings are errors
        #/wd4996 # deprecated-declarations
        # (I don't know all the windows warning numbers
        #  replace cxxx with /wdxxx to supress them)
        )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/assets/installers")
include(installer)
