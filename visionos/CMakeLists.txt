# SPDX-License-Identifier: GPL-3.0-or-later
#
# PrimedGun for Apple Vision Pro: the SwiftUI app around the game (docs/VISIONOS.md).
#
# The game itself is built first by the main CMakeLists.txt with Ninja, cross-compiled for visionOS
# as a framework (MetroidPrimeGame.framework; nod's Rust build and the decompiled game stay out of
# Xcode that way). This project, which only the Xcode generator can make (Swift, signing, the
# bundle), holds the launcher app, links that framework and embeds it, and drives it through
# platform/include/visionos/*.h.
#
#   cmake -S visionos -B build/visionos-app -G Xcode \
#       -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
#       -DMP_VISIONOS_GAME_FRAMEWORK=<build dir>/MetroidPrimeGame.framework \
#       -DMP_VISIONOS_TEAM=<Apple developer team id>
#   cmake --build build/visionos-app --config Release --target PrimedGunVision
#
# visionos/Build-VisionOS.sh runs every step (Dawn, the game, this app) and installs the result.
cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=visionOS (and -DCMAKE_OSX_SYSROOT=xros or xrsimulator)")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "The visionOS app needs the Xcode generator (-G Xcode): Swift, signing and the bundle come from it")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    # visionOS 26: the PlayStation VR2 Sense controllers and ARKit's accessory tracking start there.
    set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "" FORCE)
endif()
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)

project(PrimedGunVision LANGUAGES C CXX OBJC Swift)

set(MP_VISIONOS_GAME_FRAMEWORK "" CACHE PATH "The game framework the Ninja build made (MetroidPrimeGame.framework)")
set(MP_VISIONOS_TEAM "" CACHE STRING "Apple developer team id for automatic signing (empty: sign in Xcode, or ad hoc for the simulator)")
set(MP_VISIONOS_BUNDLE_ID "org.primedgun.vision" CACHE STRING "Bundle identifier of the app")
set(MP_VISIONOS_APP_NAME "PrimedGun" CACHE STRING "Name shown on the Home View")
set(MP_VISIONOS_VERSION "1.0" CACHE STRING "Version shown for the app")

if(NOT MP_VISIONOS_GAME_FRAMEWORK OR NOT IS_DIRECTORY "${MP_VISIONOS_GAME_FRAMEWORK}")
    message(FATAL_ERROR "MP_VISIONOS_GAME_FRAMEWORK must name the game framework built for this SDK "
                        "(visionos/Build-VisionOS.sh builds it first): '${MP_VISIONOS_GAME_FRAMEWORK}'")
endif()
get_filename_component(MP_VISIONOS_GAME_FRAMEWORK_DIR "${MP_VISIONOS_GAME_FRAMEWORK}" DIRECTORY)
get_filename_component(MP_VISIONOS_GAME_FRAMEWORK_NAME "${MP_VISIONOS_GAME_FRAMEWORK}" NAME_WE)

# Xcode signs on the iOS family with an identity; without a team the simulator build is signed ad
# hoc, which only the simulator runs.
if(MP_VISIONOS_TEAM)
    set(MP_VISIONOS_SIGN_IDENTITY "Apple Development")
    set(MP_VISIONOS_SIGN_STYLE Automatic)
else()
    set(MP_VISIONOS_SIGN_IDENTITY "-")
    set(MP_VISIONOS_SIGN_STYLE Manual)
endif()

set(MP_VISIONOS_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/App")
set(MP_VISIONOS_APP_SOURCES
    "${MP_VISIONOS_APP_DIR}/PrimedGunVisionApp.swift"
    "${MP_VISIONOS_APP_DIR}/GameInfo.swift"
    "${MP_VISIONOS_APP_DIR}/GameModel.swift"
    "${MP_VISIONOS_APP_DIR}/GameStorage.swift"
    "${MP_VISIONOS_APP_DIR}/ImmersiveGameView.swift"
    "${MP_VISIONOS_APP_DIR}/LauncherView.swift"
    "${MP_VISIONOS_APP_DIR}/SettingsView.swift")
set(MP_VISIONOS_RESOURCES "${MP_VISIONOS_APP_DIR}/Assets.xcassets")

add_executable(PrimedGunVision MACOSX_BUNDLE ${MP_VISIONOS_APP_SOURCES} ${MP_VISIONOS_RESOURCES})
set_source_files_properties(${MP_VISIONOS_RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(PrimedGunVision PROPERTIES
    OUTPUT_NAME "PrimedGunVision"
    MACOSX_BUNDLE_INFO_PLIST "${MP_VISIONOS_APP_DIR}/Info.plist"
    XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MP_VISIONOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_PRODUCT_NAME "PrimedGunVision"
    XCODE_ATTRIBUTE_MP_APP_DISPLAY_NAME "${MP_VISIONOS_APP_NAME}"
    XCODE_ATTRIBUTE_MARKETING_VERSION "${MP_VISIONOS_VERSION}"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
    # 7 is the Vision device family.
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "xros xrsimulator"
    XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_SWIFT_VERSION "6.0"
    XCODE_ATTRIBUTE_SWIFT_STRICT_CONCURRENCY "minimal"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${MP_VISIONOS_APP_DIR}/PrimedGunVision-Bridging-Header.h"
    XCODE_ATTRIBUTE_HEADER_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../platform/include"
    XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS "${MP_VISIONOS_GAME_FRAMEWORK_DIR}"
    XCODE_ATTRIBUTE_OTHER_LDFLAGS "-framework ${MP_VISIONOS_GAME_FRAMEWORK_NAME}"
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks"
    XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
    XCODE_ATTRIBUTE_ENABLE_PREVIEWS NO
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED YES
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${MP_VISIONOS_SIGN_IDENTITY}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE ${MP_VISIONOS_SIGN_STYLE}
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MP_VISIONOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${MP_VISIONOS_APP_DIR}/PrimedGunVision.entitlements"
    XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES
    XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL "-O"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_GENERATE_ASSET_SYMBOLS NO
    # The layered Home View icon in App/Assets.xcassets (a Back/Front image stack).
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    # The game framework, embedded and signed with the app's identity.
    XCODE_EMBED_FRAMEWORKS "${MP_VISIONOS_GAME_FRAMEWORK}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
    XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY ON)
# Relinks when the Ninja build replaces the framework.
set_property(TARGET PrimedGunVision APPEND PROPERTY LINK_DEPENDS
    "${MP_VISIONOS_GAME_FRAMEWORK}/${MP_VISIONOS_GAME_FRAMEWORK_NAME}")

message(STATUS "visionOS app: ${MP_VISIONOS_APP_NAME} (${MP_VISIONOS_BUNDLE_ID}) embedding ${MP_VISIONOS_GAME_FRAMEWORK}")
