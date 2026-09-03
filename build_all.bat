@echo off
setlocal enabledelayedexpansion
rem Build both architectures, stage, and package the installer.

set ROOT=%~dp0
pushd "%ROOT%"

echo === stopping any running engine host ===
if exist "build_x86\bin\Release\flexvoice_host.exe" (
    "build_x86\bin\Release\flexvoice_host.exe" --shutdown >nul 2>&1
)
taskkill /F /IM flexvoice_host.exe >nul 2>&1

echo === configuring x86 ===
cmake -S . -B build_x86 -G "Visual Studio 17 2022" -A Win32 || goto :fail
echo === building x86 ===
cmake --build build_x86 --config Release || goto :fail

echo === configuring x64 ===
cmake -S . -B build_x64 -G "Visual Studio 17 2022" -A x64 || goto :fail
echo === building x64 ===
cmake --build build_x64 --config Release || goto :fail

echo === self test ===
"build_x86\bin\Release\flexvoice_host.exe" --selftest "%ROOT%bin\fv" || goto :fail

echo === staging ===
powershell -NoProfile -ExecutionPolicy Bypass -File "installer\stage.ps1" || goto :fail

echo === installer ===
set ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" (
    echo Inno Setup 6 not found; skipping the installer.
    echo Everything else built successfully.
    goto :done
)
"%ISCC%" /O"%ROOT%output" "installer\flexvoice.iss" || goto :fail
echo.
for %%f in ("%ROOT%output\FlexVoiceSAPI_Setup_*.exe") do echo Installer: %%f

:done
popd
exit /b 0

:fail
echo.
echo BUILD FAILED
popd
exit /b 1
