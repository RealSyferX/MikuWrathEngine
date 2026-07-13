cmake_minimum_required(VERSION 3.16)
project(MikuWrathEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /utf-8 /MP)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

include(FetchContent)

# --- Capstone (for disassembly) ---
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    capstone
    GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
    GIT_TAG 5.0.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(capstone)

add_executable(MikuWrathEngine WIN32
    src/main.cpp
    src/app.cpp
    src/ui.cpp
    src/process_manager.cpp
    src/scanner.cpp
    src/disassembler.cpp
    src/memory_viewer.cpp
    src/address_table.cpp
    src/debugger.cpp
    src/access_hits_window.cpp
)

target_include_directories(MikuWrathEngine PRIVATE
    src
    ${capstone_SOURCE_DIR}/include
)

target_link_libraries(MikuWrathEngine PRIVATE
    gdiplus
    dwmapi
    gdi32
    user32
    psapi
    capstone
    Comdlg32
)

# ============================================================
# Unit tests for pure logic (AOB parsing, value round-trip, address parsing).
# Enabled by default; configure with -DBUILD_TESTS=OFF to skip.
# ============================================================
option(BUILD_TESTS "Build the mwe_tests unit test target" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(mwe_tests
        tests/test_main.cpp
        src/scanner.cpp          # provides Scanner::ParseAOB
        src/process_manager.cpp  # provides ProcessManager symbols pulled in by scanner
    )
    target_include_directories(mwe_tests PRIVATE
        src
        ${capstone_SOURCE_DIR}/include
    )
    # scanner.cpp/process_manager.cpp reference Win32 (psapi) symbols.
    target_link_libraries(mwe_tests PRIVATE psapi user32)
    add_test(NAME mwe_tests COMMAND mwe_tests)
endif()
