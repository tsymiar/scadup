@echo off
REM Build scadup on Windows. Needs CMake and a generator (Visual Studio by default).
setlocal

if "%1"=="clean" (
    if exist build rmdir /s /q build
    if exist target\lib rmdir /s /q target\lib
    if exist target\include rmdir /s /q target\include
    echo cleaned
    exit /b 0
)

if not exist build mkdir build

cmake -S . -B build
if errorlevel 1 exit /b 1

cmake --build build --config Release
if errorlevel 1 exit /b 1

echo Build output: build\test\Release\scadup.exe

REM Smoke test: one publish() must reach one live subscriber
powershell -ExecutionPolicy Bypass -File ci\smoke-test.ps1
if errorlevel 1 exit /b 1

echo Smoke test passed.
endlocal
