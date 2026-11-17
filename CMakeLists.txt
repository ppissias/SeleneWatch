cmake_minimum_required(VERSION 3.16)

# The release: year, month, and the release's number in that month
# (2026.11-01 is the first release of November 2026). Change it here only;
# the code, the executable's properties and the packages take it from here.
set(SELENEWATCH_RELEASE "2026.11-01")
string(REGEX REPLACE "^([0-9]+)\\.0?([0-9]+)-0?([0-9]+)$" "\\1.\\2.\\3" selenewatch_numeric_version "${SELENEWATCH_RELEASE}")
project(SeleneWatch VERSION ${selenewatch_numeric_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED TRUE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

add_custom_target(selenewatch_version
    COMMAND ${CMAKE_COMMAND} "-DSELENEWATCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DSELENEWATCH_RELEASE=${SELENEWATCH_RELEASE}"
        "-DSELENEWATCH_VERSION_HEADER=${CMAKE_CURRENT_BINARY_DIR}/selenewatch_version.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/write_version.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/selenewatch_version.h"
    VERBATIM)

function(selenewatch_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

# One processing library, shared by the application and tests. It has no
# Widgets dependency; the UI library and its tests use Widgets separately.
add_library(selenewatch_core STATIC
    blip_injector.cpp frame_pool.cpp memory_budget.cpp preview_image.cpp session.cpp
    frame_ring_buffer.cpp flash_detector.cpp event_capture.cpp
    sources/fine_sleep.cpp sources/synthetic_scene.cpp
    sources/synthetic_frame_source.cpp sources/recording_source.cpp
    cameras/zwo_source.cpp cameras/zwo_sdk.cpp cameras/qhy_source.cpp cameras/qhy_sdk.cpp cameras/vendor_runtime.cpp
    cameras/playerone_source.cpp cameras/playerone_sdk.cpp cameras/touptek_source.cpp cameras/touptek_sdk.cpp
    recording/firecapture_log.cpp recording/recording_reader.cpp recording/ser_reader.cpp recording/ser_writer.cpp
    recording/avi_reader.cpp recording/event_writer.cpp recording/clip_overview.cpp
    recording/metadata_text.cpp recording/frame_timing.cpp recording/recording_writer.cpp clock_check.cpp
    detection_mask.cpp sky_schedule.cpp sky_exposure.cpp
    guiding/feature_lock.cpp guiding/lock_tracker.cpp guiding/guider.cpp
    guiding/simulated_mount.cpp guiding/calibrator.cpp guiding/guide_controller.cpp guiding/ascom_mount.cpp
)
target_include_directories(selenewatch_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(selenewatch_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(selenewatch_core PUBLIC Qt6::Core Qt6::Gui)
add_dependencies(selenewatch_core selenewatch_version)
if(WIN32)
    target_link_libraries(selenewatch_core PRIVATE winmm ws2_32 version)
endif()
selenewatch_compile_options(selenewatch_core)

# Use the ASI import library and place the matching DLL and notice beside the build outputs.
set(SELENEWATCH_ZWO_SDK_DIR "" CACHE PATH "Optional ZWO ASI SDK root, containing include and lib/x64")
if(SELENEWATCH_ZWO_SDK_DIR)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The configured ASI SDK integration requires Windows x64")
    endif()
    foreach(part include/ASICamera2.h lib/x64/ASICamera2.lib lib/x64/ASICamera2.dll license.txt)
        if(NOT EXISTS "${SELENEWATCH_ZWO_SDK_DIR}/${part}")
            message(FATAL_ERROR "ASI SDK component missing: ${SELENEWATCH_ZWO_SDK_DIR}/${part}")
        endif()
    endforeach()
    target_compile_definitions(selenewatch_core PRIVATE SELENEWATCH_WITH_ZWO=1)
    target_include_directories(selenewatch_core SYSTEM PRIVATE "${SELENEWATCH_ZWO_SDK_DIR}/include")
    target_link_libraries(selenewatch_core PRIVATE "${SELENEWATCH_ZWO_SDK_DIR}/lib/x64/ASICamera2.lib")
    add_custom_target(selenewatch_zwo_runtime
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SELENEWATCH_ZWO_SDK_DIR}/lib/x64/ASICamera2.dll" "${CMAKE_CURRENT_BINARY_DIR}/ASICamera2.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SELENEWATCH_ZWO_SDK_DIR}/license.txt" "${CMAKE_CURRENT_BINARY_DIR}/licenses/ZWO-ASI-SDK.txt"
        VERBATIM)
    add_dependencies(selenewatch_core selenewatch_zwo_runtime)
endif()

# The ASCOM mount adapter uses late-bound COM against the installed ASCOM
# Platform, so it needs no SDK at build time. Without the Platform the
# application offers the simulated mount only.
option(SELENEWATCH_WITH_ASCOM "Build the ASCOM telescope adapter (Windows)" ${WIN32})
if(SELENEWATCH_WITH_ASCOM)
    if(NOT WIN32)
        message(FATAL_ERROR "The ASCOM adapter requires Windows")
    endif()
    target_compile_definitions(selenewatch_core PRIVATE SELENEWATCH_WITH_ASCOM=1)
    target_link_libraries(selenewatch_core PRIVATE ole32 oleaut32)
endif()

# Build against the QHY declarations. The adapter discovers the user's DLL at runtime.
set(SELENEWATCH_QHY_SDK_DIR "" CACHE PATH "Optional QHY SDK root containing include/qhyccd.h")
if(SELENEWATCH_QHY_SDK_DIR)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The QHY adapter requires Windows x64")
    endif()
    set(qhy_header "${SELENEWATCH_QHY_SDK_DIR}/include/qhyccd.h")
    if(NOT EXISTS "${qhy_header}")
        message(FATAL_ERROR "QHY SDK header missing: ${qhy_header}")
    endif()
    target_compile_definitions(selenewatch_core PRIVATE SELENEWATCH_WITH_QHY=1
        SELENEWATCH_QHY_HEADER="${qhy_header}")
    message(STATUS "QHY runtime will be discovered on demand from the user's Windows installation")
endif()

# Feature-16: Player One and ToupTek, built against the vendors' headers and
# using the DLL the observer installs, found at run time as for QHY. Neither
# SDK's files are in the repository or the packages (THIRD_PARTY_LICENSES.md).
set(SELENEWATCH_PLAYERONE_SDK_DIR "" CACHE PATH "Optional Player One camera SDK root containing include/PlayerOneCamera.h")
if(SELENEWATCH_PLAYERONE_SDK_DIR)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The Player One adapter requires Windows x64")
    endif()
    set(playerone_header "${SELENEWATCH_PLAYERONE_SDK_DIR}/include/PlayerOneCamera.h")
    if(NOT EXISTS "${playerone_header}")
        message(FATAL_ERROR "Player One SDK header missing: ${playerone_header}")
    endif()
    target_compile_definitions(selenewatch_core PRIVATE SELENEWATCH_WITH_PLAYERONE=1
        SELENEWATCH_PLAYERONE_HEADER="${playerone_header}")
endif()
set(SELENEWATCH_TOUPTEK_SDK_DIR "" CACHE PATH "Optional ToupTek camera SDK root containing inc/toupcam.h")
if(SELENEWATCH_TOUPTEK_SDK_DIR)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The ToupTek adapter requires Windows x64")
    endif()
    set(touptek_header "${SELENEWATCH_TOUPTEK_SDK_DIR}/inc/toupcam.h")
    if(NOT EXISTS "${touptek_header}")
        message(FATAL_ERROR "ToupTek SDK header missing: ${touptek_header}")
    endif()
    target_compile_definitions(selenewatch_core PRIVATE SELENEWATCH_WITH_TOUPTEK=1
        SELENEWATCH_TOUPTEK_HEADER="${touptek_header}")
endif()

add_library(selenewatch_ui STATIC mainwindow.ui mainwindow.h mainwindow.cpp
    preview_widget.h preview_widget.cpp detection_panel.h detection_panel.cpp display_panel.h display_panel.cpp camera_panel.h camera_panel.cpp
    histogram_panel.h histogram_panel.cpp guiding_panel.h guiding_panel.cpp elided_label.h elided_label.cpp readout_label.h readout_label.cpp site_dialog.h site_dialog.cpp
    camera_settings_editor.h camera_settings_editor.cpp qhy_camera_panel.h qhy_camera_panel.cpp
    camera_family_panel.h runtime_chooser.h runtime_chooser.cpp camera_support_dialog.h camera_support_dialog.cpp
    playerone_camera_panel.h playerone_camera_panel.cpp touptek_camera_panel.h touptek_camera_panel.cpp)
target_include_directories(selenewatch_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(selenewatch_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(selenewatch_ui PUBLIC selenewatch_core Qt6::Widgets)
add_dependencies(selenewatch_ui selenewatch_version)
selenewatch_compile_options(selenewatch_ui)
qt_add_executable(SeleneWatch main.cpp)
if(WIN32)
    # The resource script includes selenewatch_version.h for the file's version.
    target_sources(SeleneWatch PRIVATE selenewatch.rc)
    target_include_directories(SeleneWatch PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    add_dependencies(SeleneWatch selenewatch_version)
endif()
qt_add_resources(SeleneWatch "icons" PREFIX "/" FILES
    icons/selenewatch-16.png icons/selenewatch-32.png
    icons/selenewatch-48.png icons/selenewatch-256.png)
set_target_properties(SeleneWatch PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(SeleneWatch PRIVATE selenewatch_ui)
selenewatch_compile_options(SeleneWatch)
# The .zip and the installer, from the Release build only (a Debug build
# would need Qt's debug DLLs and the debug C++ runtime, which may not ship).
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(packaging/package.cmake)
endif()

enable_testing()
add_executable(SeleneWatchTests tests/core_tests.cpp)
target_link_libraries(SeleneWatchTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchTests)
# TAP reaches stdout on Windows even when CTest starts without a console.
add_test(NAME core COMMAND SeleneWatchTests -o -,tap)

qt_add_executable(SeleneWatchPreviewTests tests/preview_tests.cpp preview_widget.cpp preview_widget.h)
target_include_directories(SeleneWatchPreviewTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(SeleneWatchPreviewTests PRIVATE Qt6::Widgets Qt6::Test)
selenewatch_compile_options(SeleneWatchPreviewTests)
add_test(NAME preview COMMAND SeleneWatchPreviewTests -o -,tap)
set_tests_properties(preview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchSerTests tests/ser_writer_tests.cpp)
target_link_libraries(SeleneWatchSerTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchSerTests)
add_test(NAME ser COMMAND SeleneWatchSerTests -o -,tap)

add_executable(SeleneWatchDetectionTests tests/detection_tests.cpp)
target_link_libraries(SeleneWatchDetectionTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchDetectionTests)
add_test(NAME detection COMMAND SeleneWatchDetectionTests -o -,tap)

add_executable(SeleneWatchUiTests tests/ui_tests.cpp)
target_link_libraries(SeleneWatchUiTests PRIVATE selenewatch_ui Qt6::Test)
target_compile_definitions(SeleneWatchUiTests PRIVATE SELENEWATCH_UI_QA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/out/qa-feature9")
target_compile_definitions(SeleneWatchUiTests PRIVATE SELENEWATCH_DISPLAY_QA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/out/qa-feature5")
target_compile_definitions(SeleneWatchUiTests PRIVATE SELENEWATCH_MANUAL_QA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/out/qa-manual")
target_compile_definitions(SeleneWatchUiTests PRIVATE SELENEWATCH_CAMERA_QA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/out/qa-zwo")
selenewatch_compile_options(SeleneWatchUiTests)
add_test(NAME ui COMMAND SeleneWatchUiTests -o -,tap)
set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchDisplayTests tests/display_capture_tests.cpp)
target_link_libraries(SeleneWatchDisplayTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchDisplayTests)
add_test(NAME display_capture COMMAND SeleneWatchDisplayTests -o -,tap)

add_executable(SeleneWatchOverviewTests tests/overview_tests.cpp)
target_link_libraries(SeleneWatchOverviewTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchOverviewTests)
add_test(NAME overview COMMAND SeleneWatchOverviewTests -o -,tap)
set_tests_properties(overview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchSkyTests tests/sky_tests.cpp)
target_link_libraries(SeleneWatchSkyTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchSkyTests)
add_test(NAME sky COMMAND SeleneWatchSkyTests -o -,tap)
set_tests_properties(sky PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchDarkeningTests tests/darkening_tests.cpp)
target_link_libraries(SeleneWatchDarkeningTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchDarkeningTests)
add_test(NAME darkening COMMAND SeleneWatchDarkeningTests -o -,tap)
set_tests_properties(darkening PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchSkyExtrasTests tests/sky_extras_tests.cpp)
target_link_libraries(SeleneWatchSkyExtrasTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchSkyExtrasTests)
add_test(NAME sky_extras COMMAND SeleneWatchSkyExtrasTests -o -,tap)
set_tests_properties(sky_extras PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchSessionRecordTests tests/session_record_tests.cpp)
target_link_libraries(SeleneWatchSessionRecordTests PRIVATE selenewatch_core Qt6::Test)
if(WIN32)
    target_link_libraries(SeleneWatchSessionRecordTests PRIVATE ws2_32)
endif()
selenewatch_compile_options(SeleneWatchSessionRecordTests)
add_test(NAME session_record COMMAND SeleneWatchSessionRecordTests -o -,tap)
set_tests_properties(session_record PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchRegistrationTests tests/registration_tests.cpp tools/translation_estimator.cpp)
target_link_libraries(SeleneWatchRegistrationTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchRegistrationTests)
add_test(NAME registration COMMAND SeleneWatchRegistrationTests -o -,tap)

add_executable(SeleneWatchManualTests tests/manual_capture_tests.cpp)
target_link_libraries(SeleneWatchManualTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchManualTests)
add_test(NAME manual_capture COMMAND SeleneWatchManualTests -o -,tap)

add_executable(SeleneWatchZwoTests tests/zwo_tests.cpp)
target_link_libraries(SeleneWatchZwoTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchZwoTests)
add_test(NAME zwo COMMAND SeleneWatchZwoTests -o -,tap)

add_executable(SeleneWatchGuidingTests tests/guiding_tests.cpp)
target_link_libraries(SeleneWatchGuidingTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchGuidingTests)
add_test(NAME guiding COMMAND SeleneWatchGuidingTests -o -,tap)
set_tests_properties(guiding PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchQhyTests tests/qhy_tests.cpp)
target_link_libraries(SeleneWatchQhyTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchQhyTests)
add_test(NAME qhy COMMAND SeleneWatchQhyTests -o -,tap)

add_executable(SeleneWatchVendorRuntimeTests tests/vendor_runtime_tests.cpp)
target_link_libraries(SeleneWatchVendorRuntimeTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchVendorRuntimeTests)
add_test(NAME vendor_runtime COMMAND SeleneWatchVendorRuntimeTests -o -,tap)
# It loads files that are not DLLs. Should Windows ever show an error box for
# one, the test fails within a minute instead of waiting for a click.
set_tests_properties(vendor_runtime PROPERTIES TIMEOUT 60)

add_executable(SeleneWatchPlayerOneTests tests/playerone_tests.cpp)
target_link_libraries(SeleneWatchPlayerOneTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchPlayerOneTests)
add_test(NAME playerone COMMAND SeleneWatchPlayerOneTests -o -,tap)

add_executable(SeleneWatchToupTekTests tests/touptek_tests.cpp)
target_link_libraries(SeleneWatchToupTekTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchToupTekTests)
add_test(NAME touptek COMMAND SeleneWatchToupTekTests -o -,tap)

# Run by hand, with an explicit input and output path; never part of CTest.
add_executable(SeleneWatchProbe tools/recording_probe.cpp tools/detection_probe.cpp tools/rearming_probe.cpp tools/translation_estimator.cpp
    tools/lock_probe.cpp tools/guide_probe.cpp tools/pulse_probe.cpp tools/overview_probe.cpp tools/darkening_probe.cpp)
target_link_libraries(SeleneWatchProbe PRIVATE selenewatch_core)
selenewatch_compile_options(SeleneWatchProbe)

add_library(selenewatch_camera_probe STATIC tools/camera_probe.cpp tools/qhy_probe.cpp)
target_link_libraries(selenewatch_camera_probe PUBLIC selenewatch_core)
if(WIN32)
    target_link_libraries(selenewatch_camera_probe PRIVATE psapi)
endif()
selenewatch_compile_options(selenewatch_camera_probe)
add_executable(SeleneWatchCameraProbe tools/camera_probe_main.cpp)
target_link_libraries(SeleneWatchCameraProbe PRIVATE selenewatch_camera_probe)
selenewatch_compile_options(SeleneWatchCameraProbe)
add_executable(SeleneWatchQhyProbe tools/qhy_probe_main.cpp)
target_link_libraries(SeleneWatchQhyProbe PRIVATE selenewatch_camera_probe)
selenewatch_compile_options(SeleneWatchQhyProbe)
add_executable(SeleneWatchRuntimeProbe tools/runtime_probe_main.cpp)
target_link_libraries(SeleneWatchRuntimeProbe PRIVATE selenewatch_core)
selenewatch_compile_options(SeleneWatchRuntimeProbe)

add_executable(SeleneWatchCameraProbeTests tests/camera_probe_tests.cpp)
target_link_libraries(SeleneWatchCameraProbeTests PRIVATE selenewatch_camera_probe Qt6::Test)
selenewatch_compile_options(SeleneWatchCameraProbeTests)
add_test(NAME camera_probe COMMAND SeleneWatchCameraProbeTests -o -,tap)
set_tests_properties(camera_probe PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
