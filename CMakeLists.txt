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
