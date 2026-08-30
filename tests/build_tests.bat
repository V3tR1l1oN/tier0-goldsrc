@echo off
rem ============================================================
rem  Regenerates the test harness .exes (x86).
rem  Usage: build_tests.bat   (run from repo root; auto-detects vcvars32)
rem  Must run AFTER build.bat so build\tier0.dll exists -- tests run
rem  against the built DLL next to their .exe in tests\.
rem ============================================================
setlocal

if defined VCToolsInstallDir goto :env_ok

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if exist "%VCVARS%" goto :found_vc
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if exist "%VCVARS%" goto :found_vc
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

:found_vc
if not defined VCVARS goto :env_ok
call "%VCVARS%" >nul

:env_ok
if not defined VCToolsInstallDir (
    echo ERROR: MSVC toolchain not found.
    exit /b 1
)

set "T=%~dp0"

cl /O1 /GS- /nologo "%T%\test_exports.cpp"   /Fe"%T%\test_exports.exe"   /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo "%T%\test_mem.cpp"       /Fe"%T%\test_mem.exe"       /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo "%T%\test_vprof.cpp"     /Fe"%T%\test_vprof.exe"     /link /SUBSYSTEM:CONSOLE user32.lib || goto :fail
cl /O1 /GS- /nologo "%T%\test_getsize2048.cpp" /Fe"%T%\test_getsize2048.exe" /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo "%T%\test_sba_mt.cpp"    /Fe"%T%\test_sba_mt.exe"    /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O2 /GS- /nologo "%T%\test_sba_stress.cpp" /Fe"%T%\test_sba_stress.exe" /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O2 /GS- /nologo "%T%\bench.cpp"          /Fe"%T%\bench.exe"          /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo /I "%T%..\public\tier0" "%T%\test_cpu.cpp" /Fe"%T%\test_cpu.exe" /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo "%T%\sba_diag.cpp"      /Fe"%T%\sba_diag.exe"      /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo "%T%\test_regress.cpp"  /Fe"%T%\test_regress.exe"  /link /SUBSYSTEM:CONSOLE psapi.lib || goto :fail
cl /O1 /GS- /nologo "%T%\test_crash.cpp"     /Fe"%T%\test_crash.exe"     /link /SUBSYSTEM:CONSOLE dbghelp.lib || goto :fail
cl /O1 /GS- /nologo "%T%\test_threadtools.cpp" /Fe"%T%\test_threadtools.exe" /link /SUBSYSTEM:CONSOLE || goto :fail
cl /O1 /GS- /nologo "%T%\test_sba_shutdown.cpp" /Fe"%T%\test_sba_shutdown.exe" /link /SUBSYSTEM:CONSOLE || goto :fail

echo.
echo BUILD OK: tests\.exe
exit /b 0

:fail
echo.
echo TEST BUILD FAILED
exit /b 1