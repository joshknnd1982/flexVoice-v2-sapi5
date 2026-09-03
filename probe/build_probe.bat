@echo off
rem Build fv2_probe as 32-bit: FlexVoice_2_00_010.dll is x86-only.
rem C++14 because EngineFactory::createEngine returns std::auto_ptr by value and
rem the mangled name only matches if the type really is std::auto_ptr.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1

set "ROOT=%~dp0.."
if not exist "%ROOT%\build\probe" mkdir "%ROOT%\build\probe"

cl /nologo /std:c++14 /EHsc /GR /MD /O2 /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%ROOT%\build\probe\\" /Fe"%ROOT%\build\probe\fv2_probe.exe" ^
   "%ROOT%\probe\fv2_probe.cpp" ^
   "%ROOT%\sdk\fv2lib\FlexVoice_2_00_010.lib"
if errorlevel 1 exit /b 1

echo Built %ROOT%\build\probe\fv2_probe.exe
endlocal
