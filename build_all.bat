@echo off
rem Build FlexVoice 2 SAPI5 end to end: both architectures, then the installer.
setlocal

echo FlexVoice 2 SAPI5 build
echo.

set "ROOT=%~dp0"
cd /d "%ROOT%"

rem --- the engine import library ------------------------------------------
rem FlexVoice 2.0 shipped no .lib, so one is generated from the DLL's own
rem export table. 381 C++ symbols, x86.
if not exist "%ROOT%sdk\fv2lib\FlexVoice_2_00_010.lib" (
    echo Generating the engine import library...
    if not exist "%ROOT%engine\FlexVoice_2_00_010.dll" (
        echo ERROR: engine\FlexVoice_2_00_010.dll is missing.
        echo        The engine is not in this repository; see the README.
        exit /b 1
    )
    call :findvs
    if errorlevel 1 exit /b 1
    call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
    lib /nologo /def:"%ROOT%sdk\fv2lib\FlexVoice_2_00_010.def" /machine:x86 ^
        /out:"%ROOT%sdk\fv2lib\FlexVoice_2_00_010.lib"
    if errorlevel 1 exit /b 1
)

echo Building x86 targets...
cmake -A Win32 -S . -B build_x86
if errorlevel 1 exit /b 1
cmake --build build_x86 --config Release
if errorlevel 1 exit /b 1

echo Building x64 targets...
cmake -A x64 -S . -B build_x64
if errorlevel 1 exit /b 1
cmake --build build_x64 --config Release
if errorlevel 1 exit /b 1

echo Staging output layout...
powershell -NoProfile -ExecutionPolicy Bypass -File installer\stage.ps1
if errorlevel 1 exit /b 1

echo Building installer...
set "ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo ERROR: Inno Setup 6 compiler not found.
    exit /b 1
)
"%ISCC%" /O"output" installer\flexvoice2.iss
if errorlevel 1 exit /b 1

echo.
echo Build completed. Installer is in output\
goto :eof

:findvs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022 or the Build Tools.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: no Visual Studio installation found.
    exit /b 1
)
exit /b 0
