@echo off
rem ============================================================
rem PeripheralBattery - build script
rem MCHOSE G87 keyboard + A5 Ultra mouse + G7 Pro + DualSense battery display plugin for TrafficMonitor
rem (one DLL, one display item: peripheralBattery - keyboard/mouse/gamepad combined)
rem
rem Requirements:
rem   Visual Studio with "Desktop development with C++"
rem   Windows 10/11 SDK (installed together with VS by default)
rem
rem Usage:
rem   Run `build.bat` from any shell. Outputs go to .\out\
rem ============================================================
setlocal

rem ---- locate Visual Studio via vswhere (fallback: common paths) ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    if exist "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo [ERROR] vcvars64.bat not found.
    echo         Install Visual Studio with the "Desktop development with C++" workload,
    echo         then run this script again.
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

rem ---- newest MSVC toolset under VSROOT ----
set "MSVCVER="
for /f "delims=" %%i in ('dir /b /ad /o-n "%VSROOT%\VC\Tools\MSVC"') do if not defined MSVCVER set "MSVCVER=%%i"
if not defined MSVCVER (
    echo [ERROR] MSVC toolset not found under "%VSROOT%\VC\Tools\MSVC"
    exit /b 1
)

rem ---- Windows SDK: try several known locations ----
set "KITROOT="
if exist "%VSROOT%\Windows Kits\10\Include" set "KITROOT=%VSROOT%\Windows Kits\10"
if not defined KITROOT if exist "%ProgramFiles%\Microsoft Visual Studio\Windows Kits\10\Include" set "KITROOT=%ProgramFiles%\Microsoft Visual Studio\Windows Kits\10"
if not defined KITROOT if exist "C:\Program Files (x86)\Windows Kits\10\Include" set "KITROOT=C:\Program Files (x86)\Windows Kits\10"
if not defined KITROOT (
    echo [ERROR] Windows SDK not found. Install Visual Studio's "Desktop development with C++" workload.
    exit /b 1
)
set "KITVER="
for /f "delims=" %%i in ('dir /b /ad /o-n "%KITROOT%\Include"') do if not defined KITVER set "KITVER=%%i"
if not defined KITVER (
    echo [ERROR] No SDK include version found under "%KITROOT%\Include"
    exit /b 1
)

set "MSVCROOT=%VSROOT%\VC\Tools\MSVC\%MSVCVER%"
set "INCLUDE=%MSVCROOT%\include;%KITROOT%\Include\%KITVER%\ucrt;%KITROOT%\Include\%KITVER%\shared;%KITROOT%\Include\%KITVER%\um"
set "LIB=%MSVCROOT%\lib\x64;%KITROOT%\Lib\%KITVER%\ucrt\x64;%KITROOT%\Lib\%KITVER%\um\x64"
set "PATH=%MSVCROOT%\bin\Hostx64\x64;%PATH%"

set ROOT=%~dp0
if not exist "%ROOT%out" mkdir "%ROOT%out"
cd /d "%ROOT%out"

echo.
echo == [1/3] Build plugin DLL ==
cl /nologo /c /EHsc /utf-8 /DUNICODE /D_UNICODE /O2 /MD /W3 ^
    /I"%ROOT%src\common" /I"%ROOT%src\keyboard" /I"%ROOT%src\mouse" ^
    /I"%ROOT%src\dualsense" /I"%ROOT%src\g7" /I"%ROOT%src\plugin" ^
    "%ROOT%src\keyboard\KeyboardDevice.cpp" ^
    "%ROOT%src\keyboard\KeyboardReader.cpp" ^
    "%ROOT%src\mouse\MouseDevice.cpp" ^
    "%ROOT%src\mouse\MouseReader.cpp" ^
    "%ROOT%src\dualsense\DualSenseDevice.cpp" ^
    "%ROOT%src\dualsense\DualSenseReader.cpp" ^
    "%ROOT%src\g7\G7Device.cpp" ^
    "%ROOT%src\g7\G7Reader.cpp" ^
    "%ROOT%src\plugin\PluginConfig.cpp" ^
    "%ROOT%src\plugin\PeripheralItem.cpp" ^
    "%ROOT%src\plugin\Plugin.cpp" ^
    "%ROOT%src\plugin\dllmain.cpp"
if errorlevel 1 exit /b 1

echo.
echo == [2/3] Compile resources (plugin icon) ==
rc /nologo /I "%ROOT%src\plugin" /fo "%ROOT%out\Plugin.res" "%ROOT%src\plugin\Plugin.rc"
if errorlevel 1 exit /b 1

link /nologo /DLL /OUT:PeripheralBattery.dll /SUBSYSTEM:WINDOWS ^
    KeyboardDevice.obj KeyboardReader.obj ^
    MouseDevice.obj MouseReader.obj ^
    DualSenseDevice.obj DualSenseReader.obj ^
    G7Device.obj G7Reader.obj ^
    PluginConfig.obj ^
    PeripheralItem.obj Plugin.obj dllmain.obj Plugin.res ^
    kernel32.lib user32.lib gdi32.lib gdiplus.lib setupapi.lib hid.lib ucrt.lib uuid.lib
if errorlevel 1 exit /b 1

echo.
echo == [3/3] Check exports ==
dumpbin /exports PeripheralBattery.dll | findstr /C:"TMPluginGetInstance"
if errorlevel 1 (
    echo [ERROR] Export TMPluginGetInstance not found
    exit /b 1
)

echo.
echo [OK] Build finished:
echo      %ROOT%out\PeripheralBattery.dll
echo      (copy to TrafficMonitor's plugins folder, then enable the "peripheralBattery" item)
endlocal
