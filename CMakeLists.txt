cmake_minimum_required(VERSION 3.16)
project(SeleneWatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_NO_SYSTEM_FROM_IMPORTED TRUE)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

function(selenewatch_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

set(PROJECT_SOURCES
    main.cpp
    mainwindow.ui
    mainwindow.h
    mainwindow.cpp
    frame.h
    frame_pool.h
    frame_pool.cpp
    latest_frame_slot.h
    stream_info.h
    frame_source.h
    fine_sleep.h
    fine_sleep.cpp
    preview_image.h
    preview_image.cpp
    synthetic_scene.h
    synthetic_scene.cpp
    synthetic_frame_source.h
    synthetic_frame_source.cpp
    ser_reader.h
    ser_reader.cpp
    recording_source.h
    recording_source.cpp
    session.h
    session.cpp
    frame_ring_buffer.h
    frame_ring_buffer.cpp
    flash_detector.h
    flash_detector.cpp
    event_clip.h
    event_capture.h
    event_capture.cpp
    event_writer.h
    event_writer.cpp
)

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        WIN32_EXECUTABLE TRUE
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

selenewatch_compile_options(${PROJECT_NAME})

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE winmm)
endif()

enable_testing()

add_executable(SeleneWatchTests
    tests/core_tests.cpp
    frame_pool.cpp
    fine_sleep.cpp
    preview_image.cpp
    synthetic_scene.cpp
    ser_reader.cpp
    recording_source.cpp
    session.cpp
    frame_ring_buffer.cpp
    flash_detector.cpp
    event_capture.cpp
    event_writer.cpp
)

target_include_directories(SeleneWatchTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(SeleneWatchTests
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Test
)

if(WIN32)
    target_link_libraries(SeleneWatchTests PRIVATE winmm)
endif()

selenewatch_compile_options(SeleneWatchTests)

# On Windows, Qt Test's plain text goes to the debugger, not to stdout, when
# the process has no console of its own, which is how CTest may start it.
# TAP always goes to stdout, so failures show in CTest's log. Run by hand
# from a console, the tests print plain text as usual.
add_test(NAME core COMMAND SeleneWatchTests -o -,tap)
