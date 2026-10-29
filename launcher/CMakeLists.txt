# PrimedGun.exe: PrimedGun's launcher window (from its Dolphin build's
# DolphinQt/MainWindow.cpp) beside metroid_prime_port.exe. It edits the game's
# port_settings.ini and starts the game. Built with -DMP_BUILD_LAUNCHER=ON and
# Qt 6.5 or later on CMAKE_PREFIX_PATH.
set(PRIMEDGUN_LAUNCHER_VERSION "2.0.0-beta.6")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)

set(PRIMEDGUN_LAUNCHER_UI_SOURCES
    ui/about_tab.cpp
    ui/update_release.cpp
    ui/update_dialog.cpp
    ui/aurora_host.cpp
    ui/calibration_tab.cpp
    ui/cannon_tab.cpp
    ui/controller_tab.cpp
    ui/controller_layout_dialog.cpp
    ui/launcher_window.cpp
    ui/layout_tab.cpp
    ui/port_config_tab.cpp
    ui/tab_context.cpp
    ui/widgets.cpp
    # The memory-card transfer uses the game's own Dolphin card import.
    ${PROJECT_SOURCE_DIR}/platform/port_gci.cpp
    ${PROJECT_SOURCE_DIR}/platform/port_save_convert.cpp)
qt_add_executable(PrimedGun WIN32 main.cpp ${PRIMEDGUN_LAUNCHER_UI_SOURCES})
qt_add_resources(PrimedGun "primedgun_assets"
    PREFIX "/"
    BASE assets
    FILES
        assets/PrimedGun.png
        assets/controller_layout.png
        assets/default_arm.png
        assets/samus.png
        assets/samus_arm.png)
qt_add_resources(PrimedGun "primedgun_updater" PREFIX "/" FILES update-windows.ps1)
if(WIN32)
    target_sources(PrimedGun PRIVATE PrimedGun.rc PrimedGun.manifest)
endif()

# Next to the game: the launcher finds the game, and the game's user folder
# (platform/include/port_paths.h), from its own folder.
set_target_properties(PrimedGun PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
if(MSVC)
    # Qt's static entry-point library is built with /GL, which lld-link cannot
    # read; the C runtime's own entry point calls main() just as well.
    set_target_properties(PrimedGun PROPERTIES qt_no_entrypoint ON)
    target_link_options(PrimedGun PRIVATE /ENTRY:mainCRTStartup)
endif()
target_compile_definitions(PrimedGun PRIVATE
    PRIMEDGUN_LAUNCHER_VERSION="${PRIMEDGUN_LAUNCHER_VERSION}")
target_include_directories(PrimedGun PRIVATE
    ui
    ${PROJECT_SOURCE_DIR}/platform/include)
target_link_libraries(PrimedGun PRIVATE
    primedgun_launcher_core Qt6::Widgets aurora::card ${AURORA_SDL3_TARGET})
if(WIN32)
    target_link_libraries(PrimedGun PRIVATE shell32 ole32 advapi32)
endif()
# The build revision (MP_BUILD_REVISION in platform/include/port_build_info.h) is
# defined in the root's generated source since upstream 074e531a, so it is built
# into this target too. The generated folder is not on the include path: in an
# older build folder it still holds a stale port_build_info.h.
add_dependencies(PrimedGun port_build_info)
target_sources(PrimedGun PRIVATE "${PROJECT_BINARY_DIR}/generated/port_build_info.cpp")

# Qt's DLLs and platform plugin, and PrimedGun's cannon texture slots (seeded
# into the user folder on first start).
if(WIN32 AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET PrimedGun POST_BUILD
        COMMAND "$<TARGET_FILE:Qt6::windeployqt>" --release --no-translations
            --no-compiler-runtime --no-opengl-sw --no-system-d3d-compiler
            "$<TARGET_FILE:PrimedGun>"
        COMMENT "Copying the Qt runtime next to PrimedGun.exe"
        VERBATIM)
endif()
add_custom_command(TARGET PrimedGun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/data/cannon_textures"
        "$<TARGET_FILE_DIR:PrimedGun>/primedgun/cannon_textures"
    COMMENT "Copying PrimedGun's cannon textures next to PrimedGun.exe")

install(TARGETS PrimedGun RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/cannon_textures/
    DESTINATION ${CMAKE_INSTALL_BINDIR}/primedgun/cannon_textures)
qt_generate_deploy_app_script(
    TARGET PrimedGun
    OUTPUT_SCRIPT primedgun_deploy_script
    NO_TRANSLATIONS
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${primedgun_deploy_script})

if(MP_BUILD_TESTS)
    if(WIN32 AND Python3_EXECUTABLE)
        add_test(NAME port_windows_update_tests COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/port_windows_update.py)
        set_tests_properties(port_windows_update_tests PROPERTIES LABELS port TIMEOUT 120)
    endif()
    add_executable(port_launcher_updates_tests ${PROJECT_SOURCE_DIR}/tests/port_launcher_updates.cpp ui/update_release.cpp)
    target_include_directories(port_launcher_updates_tests PRIVATE ui)
    target_link_libraries(port_launcher_updates_tests PRIVATE Qt6::Core)
    set_target_properties(port_launcher_updates_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    add_test(NAME port_launcher_updates_tests COMMAND port_launcher_updates_tests)
    set_tests_properties(port_launcher_updates_tests PROPERTIES LABELS port)
    add_executable(port_launcher_settings_sync_tests
        ${PROJECT_SOURCE_DIR}/tests/port_launcher_settings_sync.cpp
        ${PRIMEDGUN_LAUNCHER_UI_SOURCES}
        "${PROJECT_BINARY_DIR}/generated/port_build_info.cpp")
    qt_add_resources(port_launcher_settings_sync_tests "primedgun_update_test_resource"
        PREFIX "/" FILES update-windows.ps1)
    add_dependencies(port_launcher_settings_sync_tests port_build_info)
    target_include_directories(port_launcher_settings_sync_tests PRIVATE ui ${PROJECT_SOURCE_DIR}/platform/include)
    target_compile_definitions(port_launcher_settings_sync_tests PRIVATE
        PRIMEDGUN_LAUNCHER_VERSION="${PRIMEDGUN_LAUNCHER_VERSION}")
    target_link_libraries(port_launcher_settings_sync_tests PRIVATE
        primedgun_launcher_core Qt6::Widgets aurora::card ${AURORA_SDL3_TARGET})
    if(WIN32)
        target_link_libraries(port_launcher_settings_sync_tests PRIVATE shell32 ole32 advapi32)
    endif()
    set_target_properties(port_launcher_settings_sync_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    add_test(NAME port_launcher_settings_sync_tests COMMAND port_launcher_settings_sync_tests)
    if(WIN32)
        set_tests_properties(port_launcher_settings_sync_tests PROPERTIES LABELS port
            ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 20)
    else()
        set_tests_properties(port_launcher_settings_sync_tests PROPERTIES LABELS port
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
    endif()
    add_executable(port_controller_layout_ui_tests
        ${PROJECT_SOURCE_DIR}/tests/port_controller_layout_ui.cpp
        ui/controller_layout_dialog.cpp ui/tab_context.cpp ui/widgets.cpp)
    target_include_directories(port_controller_layout_ui_tests PRIVATE ui ${PROJECT_SOURCE_DIR}/platform/include)
    target_link_libraries(port_controller_layout_ui_tests PRIVATE primedgun_launcher_core Qt6::Widgets ${AURORA_SDL3_TARGET})
    set_target_properties(port_controller_layout_ui_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    add_test(NAME port_controller_layout_ui_tests COMMAND port_controller_layout_ui_tests)
    if(WIN32)
        set_tests_properties(port_controller_layout_ui_tests PROPERTIES LABELS port ENVIRONMENT "QT_QPA_PLATFORM=windows;PRIMEDGUN_LAYOUT_TEST=1" TIMEOUT 20)
    else()
        set_tests_properties(port_controller_layout_ui_tests PROPERTIES LABELS port ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PRIMEDGUN_LAYOUT_TEST=1" TIMEOUT 20)
    endif()
endif()
