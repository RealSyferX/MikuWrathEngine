@echo off
title MikuWrathEngine Build
echo ============================================
echo   MikuWrathEngine - Build Script
echo ============================================
echo.

if not exist build mkdir build
cd build

echo [1/2] Configuring with CMake...
cmake .. -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo [ERROR] CMake configuration failed!
    echo Make sure you have CMake and Visual Studio Build Tools installed.
    pause
    exit /b 1
)

echo.
echo [2/2] Building...
cmake --build . --config Release --parallel
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo ============================================
echo   BUILD SUCCESSFUL!
echo   Output: build\Release\MikuWrathEngine.exe
echo ============================================
pause
