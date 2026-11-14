cmake_minimum_required(VERSION 3.16)
project(SeleneWatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED TRUE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

add_custom_target(selenewatch_version
    COMMAND ${CMAKE_COMMAND} "-DSELENEWATCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
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
    recording/firecapture_log.cpp recording/recording_reader.cpp recording/ser_reader.cpp recording/ser_writer.cpp
    recording/avi_reader.cpp recording/event_writer.cpp
)
target_include_directories(selenewatch_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(selenewatch_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(selenewatch_core PUBLIC Qt6::Core Qt6::Gui)
add_dependencies(selenewatch_core selenewatch_version)
if(WIN32)
    target_link_libraries(selenewatch_core PRIVATE winmm)
endif()
selenewatch_compile_options(selenewatch_core)

add_library(selenewatch_ui STATIC mainwindow.ui mainwindow.h mainwindow.cpp
    preview_widget.h preview_widget.cpp detection_panel.h detection_panel.cpp display_panel.h display_panel.cpp)
target_include_directories(selenewatch_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(selenewatch_ui PUBLIC selenewatch_core Qt6::Widgets)
selenewatch_compile_options(selenewatch_ui)
qt_add_executable(SeleneWatch main.cpp)
if(WIN32)
    target_sources(SeleneWatch PRIVATE selenewatch.rc)
endif()
qt_add_resources(SeleneWatch "icons" PREFIX "/" FILES
    icons/selenewatch-16.png icons/selenewatch-32.png
    icons/selenewatch-48.png icons/selenewatch-256.png)
set_target_properties(SeleneWatch PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(SeleneWatch PRIVATE selenewatch_ui)
selenewatch_compile_options(SeleneWatch)

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
selenewatch_compile_options(SeleneWatchUiTests)
add_test(NAME ui COMMAND SeleneWatchUiTests -o -,tap)
set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(SeleneWatchDisplayTests tests/display_capture_tests.cpp)
target_link_libraries(SeleneWatchDisplayTests PRIVATE selenewatch_core Qt6::Test)
selenewatch_compile_options(SeleneWatchDisplayTests)
add_test(NAME display_capture COMMAND SeleneWatchDisplayTests -o -,tap)

# Run by hand, with an explicit input and output path; never part of CTest.
add_executable(SeleneWatchProbe tools/recording_probe.cpp tools/detection_probe.cpp)
target_link_libraries(SeleneWatchProbe PRIVATE selenewatch_core)
selenewatch_compile_options(SeleneWatchProbe)
