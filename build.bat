@echo off
rem ============================================================
rem  tier0.dll rebuild script (Win32 / x86)
rem  Reconstructs the GoldSrc tier0.dll (image base 0x10000000)
rem  - If run already inside a "x86 dev prompt", uses that env.
rem  - Otherwise auto-detects vcvars32.bat from VS Build Tools.
rem ============================================================
setlocal

rem --- Toolchain detection ---
if defined VCToolsInstallDir goto :env_ok

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if exist "%VCVARS%" goto :found_vc
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if exist "%VCVARS%" goto :found_vc
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if exist "%VCVARS%" goto :found_vc
set "VCVARS="

:found_vc
if not defined VCVARS goto :env_ok
echo Using: %VCVARS%
call "%VCVARS%" >nul

:env_ok
if not defined VCToolsInstallDir (
    echo.
    echo ERROR: MSVC toolchain not found.
    echo Run this script from a "x86 Native Tools Command Prompt",
    echo or install Visual Studio Build Tools with "Desktop development with C++".
    exit /b 1
)

set OUTDIR=%~dp0build
set SRCDIR=%~dp0tier0
set PUBDIR=%~dp0public

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
cd /d "%OUTDIR%"

set CFLAGS=/nologo /c /Z7 /W3 /O2 /Oi /Gy /Gw /FD /MD ^
 /I"%PUBDIR%\tier0" /I"%PUBDIR%" /I"%SRCDIR%" ^
 /DWIN32 /DNDEBUG /D_WINDOWS /D_USRDLL /DTIER0_DLL_EXPORT /D_CRT_SECURE_NO_WARNINGS ^
 /GF /Gm- /EHsc /guard:cf- /fp:precise /Qpar-

rem --- assemble all sources ---
del *.obj 2>nul

for %%f in ("%SRCDIR%\*.asm") do (
    echo Assembling %%~nxf ...
    ml /c /coff /nologo /Fo"%OUTDIR%\%%~nf.obj" "%%f" || goto :fail
)

for %%f in ("%SRCDIR%\*.cpp") do (
    echo %%~nxf | findstr /i "posix" >nul && (
        echo Skipping %%~nxf
    ) || (
        echo Compiling %%~nxf ...
        cl %CFLAGS% "%%f" || goto :fail
    )
)

echo Linking tier0.dll ...
link /nologo /DLL /DEF:"%~dp0tier0.def" ^
     /BASE:0x10000000 /DYNAMICBASE ^
     /MACHINE:X86 /SUBSYSTEM:WINDOWS ^
     /OPT:NOREF /OPT:NOICF ^
     /OUT:tier0.dll ^
     kernel32.lib user32.lib advapi32.lib dbghelp.lib odbc32.lib winmm.lib ^
     *.obj || goto :fail

echo.
echo BUILD OK: %OUTDIR%\tier0.dll
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1