@echo off
rem Fast syntax and warning gate used during development.  It compiles every
rem first-party translation unit with the same strict flags the Release build
rem uses, without linking and without CMake.  Usage:
rem   tools\compile-check.cmd <source file> [<source file> ...]
rem A source file not under src\ or tests\ must be given as an absolute path
rem (only the first two are searched by the loop below).
setlocal enabledelayedexpansion
call "%~dp0msvc-env.cmd"
if errorlevel 1 exit /b 1
set "ROOT=%~dp0.."
if not exist "%ROOT%\build-scratch\" mkdir "%ROOT%\build-scratch\"
pushd "%ROOT%\build-scratch"
set FAILED=0
for %%F in (%*) do (
  call :one "%ROOT%\%%F"
  if errorlevel 1 set FAILED=1
)
popd
if "%FAILED%"=="1" (
  echo [compile-check] FAILED
  exit /b 1
)
echo [compile-check] OK
exit /b 0

:one
echo [compile-check] %~1
cl /nologo /std:c++20 /EHsc /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /I"%ROOT%\include" /I"%ROOT%\tests" /c "%~1" /Fo: /Fd:vc.pdb
exit /b %errorlevel%
