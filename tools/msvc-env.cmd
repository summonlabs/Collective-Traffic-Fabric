@echo off
rem Locates Visual Studio and imports its x64 developer environment into the
rem current cmd.exe session.  Used by the local development helper scripts and
rem by the closure validation script; the CMake build does not need it because
rem CMake locates the toolchain itself.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found; install Visual Studio 2022 Build Tools. 1>&2
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo No Visual Studio installation with the C++ toolset was found. 1>&2
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
endlocal & set "VSPATH=%VSPATH%" & set "PATH=%PATH%" & set "INCLUDE=%INCLUDE%" & set "LIB=%LIB%" & set "VCINSTALLDIR=%VCINSTALLDIR%" & set "VCToolsInstallDir=%VCToolsInstallDir%"
exit /b 0
