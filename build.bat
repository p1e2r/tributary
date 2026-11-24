@echo off
rem Build Tributary on Windows (Visual Studio 2022 with "Desktop development with C++").
cd /d "%~dp0"
cmake -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
echo Built build\Release\Tributary.exe
