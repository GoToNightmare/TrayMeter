@echo off
rem Builds bin\TrayMeter.exe with the Visual Studio C++ tools (any edition with the x64 compiler).
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio with the C++ tools is required.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo The Visual Studio C++ x64 tools were not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%obj" mkdir "%ROOT%obj"
rc /nologo /fo "%ROOT%obj\TrayMeter.res" "%ROOT%src\TrayMeter.rc" || exit /b 1
cl /nologo /O1 /MT /W4 /DUNICODE /D_UNICODE /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\TrayMeter.exe" "%ROOT%src\TrayMeter.c" "%ROOT%obj\TrayMeter.res" ^
    /link /SUBSYSTEM:WINDOWS /DELAYLOAD:pdh.dll delayimp.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib pdh.lib wtsapi32.lib || exit /b 1
echo Built %ROOT%bin\TrayMeter.exe
