cmake_minimum_required(VERSION 3.28)
project(AigisKR_R0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Aigis_r.exe is 32-bit x86; configure with -A Win32")
endif()

set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
set(SAFETYHOOK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(SAFETYHOOK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SAFETYHOOK_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/safetyhook EXCLUDE_FROM_ALL)

add_library(hooking_patterns STATIC
    third_party/Hooking.Patterns/Hooking.Patterns.cpp
)
target_include_directories(hooking_patterns PUBLIC
    third_party/Hooking.Patterns
)
target_compile_features(hooking_patterns PUBLIC cxx_std_20)

add_library(AigisKR SHARED
    src/core/config.cpp
    src/core/coverage.cpp
    src/core/logger.cpp
    src/core/mem_safe.cpp
    src/core/mini_json.cpp
    src/core/tag_parser.cpp
    src/core/template_matcher.cpp
    src/core/text_encoding.cpp
    src/core/translation_core.cpp
    src/resolver/resolver.cpp
    src/core/auto_translate.cpp
    src/core/hdtrans_host_client.cpp
    src/core/knowledge_layer.cpp
    src/diagnostics/d3d11_capture.cpp
    src/diagnostics/healthcheck.cpp
    src/hooks/hangul_font.cpp
    src/hooks/probe_hooks.cpp
    src/hooks/live_text_refresh.cpp
    src/probe_main.cpp
)
set_target_properties(AigisKR PROPERTIES
    PREFIX ""
    OUTPUT_NAME "AigisKR"
    SUFFIX ".asi"
)
target_include_directories(AigisKR PRIVATE
    src
    tools
)
target_compile_definitions(AigisKR PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    ZYDIS_STATIC_BUILD
    AIGISKR_VERSION_STR="0.1.0-rc.9"
)
target_compile_options(AigisKR PRIVATE /utf-8)
target_link_libraries(AigisKR PRIVATE
    safetyhook
    hooking_patterns
    bcrypt
    gdi32
    ole32
    windowscodecs
    uuid
)

add_executable(translation_core_tests
    tests/translation_core_tests.cpp
    src/core/mem_safe.cpp
    src/core/auto_translate.cpp
    src/core/config.cpp
    src/core/coverage.cpp
    src/core/hdtrans_adapter.cpp
    src/core/hdtrans_host_client.cpp
    src/core/knowledge_layer.cpp
    src/core/logger.cpp
    src/core/mini_json.cpp
    src/core/tag_parser.cpp
    src/core/template_matcher.cpp
    src/core/translation_core.cpp
    src/diagnostics/healthcheck.cpp
)
target_include_directories(translation_core_tests PRIVATE
    src
    tools
)
target_link_libraries(translation_core_tests PRIVATE
    bcrypt
)
target_compile_definitions(translation_core_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    AIGISKR_VERSION_STR="0.1.0-rc.9"
)
target_compile_options(translation_core_tests PRIVATE /utf-8)

add_subdirectory(tools/hdtrans_host)

# Independent production-client fault probes. These are never packaged.
add_executable(hdtrans_fault_stub tests/host_fault_stub.cpp)
add_executable(hdtrans_client_probe tests/host_client_probe.cpp src/core/hdtrans_host_client.cpp src/core/logger.cpp)
foreach(probe_target hdtrans_fault_stub hdtrans_client_probe)
    target_include_directories(${probe_target} PRIVATE src tools)
    target_compile_definitions(${probe_target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${probe_target} PRIVATE /utf-8)
endforeach()
target_link_libraries(hdtrans_client_probe PRIVATE bcrypt)
