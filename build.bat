@echo off
rem Build french_input.exe with MSVC.
rem   build.bat          -> native build for this machine (x64 or arm64)
rem   build.bat x64      -> force x64
rem   build.bat arm64    -> force arm64 (cross-compiles on an x64 host if ARM64 tools are installed)
setlocal
cd /d "%~dp0"

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if "%~1"=="" if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=arm64"

set "VCARCH=%ARCH%"
set "REQ=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
if /i "%ARCH%"=="arm64" set "REQ=Microsoft.VisualStudio.Component.VC.Tools.ARM64"
if /i "%ARCH%"=="arm64" if /i not "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "VCARCH=x64_arm64"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [error] vswhere.exe not found. Install Visual Studio or Build Tools with "Desktop development with C++".
    exit /b 1
)

set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires %REQ% -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [error] No Visual Studio install has the C++ tools for %ARCH% ^(%REQ%^).
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>nul || exit /b 1

if not exist build mkdir build
cl /nologo /O1 /W4 /MT /utf-8 /DUNICODE /D_UNICODE french_input.c ^
   /Fo"build\\" /Fe"french_input_%ARCH%.exe" /link /SUBSYSTEM:WINDOWS || exit /b 1

echo.
echo Built french_input_%ARCH%.exe
