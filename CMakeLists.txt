cmake_minimum_required(VERSION 3.16)
project(MikuWrathEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /utf-8 /MP)
endif()

include(FetchContent)

# --- Dear ImGui (source-only, no CMake target) ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# --- Capstone disassembly engine ---
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

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(MikuWrathEngine WIN32
    src/main.cpp
    src/app.cpp
    src/process_manager.cpp
    src/scanner.cpp
    src/disassembler.cpp
    src/memory_viewer.cpp
    src/address_table.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(MikuWrathEngine PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${capstone_SOURCE_DIR}/include
)

target_link_libraries(MikuWrathEngine PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    gdi32
    user32
    capstone
)

if(MSVC)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
