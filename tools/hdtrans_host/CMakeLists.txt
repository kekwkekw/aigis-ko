cmake_minimum_required(VERSION 3.20)
project(AigisKR_HdTransHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "AigisKR.HdTransHost must be built 32-bit x86 (-A Win32) to load 32-bit J2KEngine.dll")
    endif()
endif()

add_executable(AigisKR_HdTransHost
    main.cpp
    hdtrans_engine.cpp
    hdtrans_engine.hpp
    host_protocol.hpp
)

set_target_properties(AigisKR_HdTransHost PROPERTIES
    OUTPUT_NAME "AigisKR.HdTransHost"
)

target_compile_definitions(AigisKR_HdTransHost PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    AIGISKR_VERSION_STR="0.1.0-rc.2"
)

target_compile_options(AigisKR_HdTransHost PRIVATE /utf-8)
target_link_libraries(AigisKR_HdTransHost PRIVATE shell32)
