cmake_minimum_required(VERSION 3.16)
project(SeleneWatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED TRUE)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE SELENEWATCH_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT SELENEWATCH_VERSION)
    set(SELENEWATCH_VERSION "unknown")
endif()

function(selenewatch_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

# One processing library, shared by the application and tests. It has no
# Widgets dependency; only the application and preview tests may use them.
add_library(selenewatch_core STATIC
    blip_injector.cpp frame_pool.cpp preview_image.cpp session.cpp
    frame_ring_buffer.cpp flash_detector.cpp event_capture.cpp
    sources/fine_sleep.cpp sources/synthetic_scene.cpp
    sources/synthetic_frame_source.cpp sources/recording_source.cpp
    recording/firecapture_log.cpp recording/ser_reader.cpp recording/ser_writer.cpp
    recording/avi_reader.cpp recording/event_writer.cpp
)
target_include_directories(selenewatch_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(selenewatch_core PUBLIC Qt6::Core Qt6::Gui)
target_compile_definitions(selenewatch_core PRIVATE SELENEWATCH_VERSION="${SELENEWATCH_VERSION}")
if(WIN32)
    target_link_libraries(selenewatch_core PRIVATE winmm)
endif()
selenewatch_compile_options(selenewatch_core)

qt_add_executable(SeleneWatch main.cpp mainwindow.ui mainwindow.h mainwindow.cpp
    preview_widget.h preview_widget.cpp)
if(WIN32)
    target_sources(SeleneWatch PRIVATE selenewatch.rc)
endif()
qt_add_resources(SeleneWatch "icons" PREFIX "/" FILES
    icons/selenewatch-16.png icons/selenewatch-32.png
    icons/selenewatch-48.png icons/selenewatch-256.png)
set_target_properties(SeleneWatch PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(SeleneWatch PRIVATE selenewatch_core Qt6::Widgets)
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

# Run by hand, with an explicit input and output path; never part of CTest.
add_executable(SeleneWatchProbe tools/recording_probe.cpp)
target_link_libraries(SeleneWatchProbe PRIVATE selenewatch_core)
selenewatch_compile_options(SeleneWatchProbe)
