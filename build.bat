@echo off
rem Builds build\MinimalVideoViewer.exe with the Visual Studio C++ build tools.
setlocal
cd /d "%~dp0"

set "MPV_URL=https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261101/mpv-dev-x86_64-20261101-git-48fca90e8c.7z"
if not exist third_party\libmpv\libmpv-2.dll (
    echo Downloading libmpv...
    if not exist third_party\libmpv mkdir third_party\libmpv
    powershell -NoProfile -Command "Invoke-WebRequest '%MPV_URL%' -OutFile 'third_party\libmpv\mpv-dev.7z'" || exit /b 1
    tar -xf third_party\libmpv\mpv-dev.7z -C third_party\libmpv || exit /b 1
    del third_party\libmpv\mpv-dev.7z
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build
lib /nologo /def:src\mpv.def /machine:x64 /out:build\mpv.lib || exit /b 1
rc /nologo /fo build\mvv.res src\mvv.rc || exit /b 1
cl /nologo /O2 /W4 /MT /DUNICODE /D_UNICODE /Ithird_party\libmpv\include src\mvv.c ^
   /Fobuild\ /Fe:build\MinimalVideoViewer.exe ^
   /link /SUBSYSTEM:WINDOWS build\mpv.lib build\mvv.res user32.lib gdi32.lib shell32.lib comdlg32.lib advapi32.lib ole32.lib uuid.lib || exit /b 1
copy /y third_party\libmpv\libmpv-2.dll build\ >nul
echo Built build\MinimalVideoViewer.exe
