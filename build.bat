@echo off
rem Builds build\BatteryBar.exe with MinGW-w64 (g++/gcc/windres must be on PATH).
setlocal
cd /d "%~dp0"
if not exist build\stub mkdir build\stub
windres res\app.rc -O coff -o build\app.res || exit /b 1
rem MinGW links its own default manifest, which clashes with ours; shadow it with an empty object.
echo. | gcc -x c -c - -o build\stub\default-manifest.o || exit /b 1
g++ -Bbuild\stub\ -std=c++17 -Os -s -static -municode -mwindows -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wno-missing-field-initializers ^
    src\main.cpp src\logitech.cpp src\bluetooth.cpp build\app.res ^
    -o build\BatteryBar.exe -lhid -lsetupapi -lgdi32 -luser32 -ladvapi32 || exit /b 1
echo Built build\BatteryBar.exe
