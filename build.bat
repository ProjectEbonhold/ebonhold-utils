@echo off
REM Rebuild ebonhold.dll (with the embedded glue script) + the tools, stage the
REM DLLs into the game folder, and regenerate Wow_ebonhold.exe. Run from this folder.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
REM The live 3.3.5 client. Everything is staged here, next to Wow.exe.
set GAME=%~dp0..\WoW-Ebonhold
call "%VCVARS%" >nul
cd /d "%~dp0"

if not exist "%GAME%\Wow.exe" (
    echo ERROR: no Wow.exe in "%GAME%" - fix the GAME variable at the top of this script.
    exit /b 1
)

echo [1/8] Embedding ebonhold_glue.lua -> glue_script.h ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_glue.ps1" || goto :err

echo [2/8] Compiling version resources...
rc /nologo /fo ebonhold_dll.res ebonhold_dll.rc || goto :err
rc /nologo /fo ebonhold_exe.res ebonhold_exe.rc || goto :err

REM /DYNAMICBASE (ASLR) + /NXCOMPAT (DEP) are standard hardening; their absence is
REM itself a mild heuristic flag. The version .res gives the binary real metadata.
echo [3/8] Building ebonhold.dll (x86)...
cl /nologo /LD /MT /Od /EHsc dllmain.cpp ebonhold_dll.res /Fe:ebonhold.dll /link /MACHINE:X86 /DYNAMICBASE /NXCOMPAT || goto :err

REM /guard:cf (Control Flow Guard) is safe for the patcher (normal code) but NOT
REM for the DLL, which calls into the game by raw offset -- CFG would reject those
REM targets and crash. So CFG is patcher-only.
echo [4/8] Building patcher (ebonhold_applymod.exe)...
cl /nologo /O2 /MT /EHsc /guard:cf patcher.cpp ebonhold_exe.res /Fe:ebonhold_applymod.exe /link /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /DYNAMICBASE /NXCOMPAT /guard:cf || goto :err

echo [5/7] Staging ebonhold.dll + ClientExt.dll into "%GAME%"...
copy /Y ebonhold.dll "%GAME%\ebonhold.dll" >nul || goto :err
REM ClientExt.dll is NOT statically imported and needs no export stamp: ebonhold.dll
REM LoadLibrary()s it by name once the client is up (see dllmain.cpp). It just has
REM to sit next to the exe so LoadLibrary finds it.
if exist "%~dp0ClientExt.dll" copy /Y ClientExt.dll "%GAME%\ClientExt.dll" >nul || goto :err

REM Output name must NOT contain "patch"/"setup"/"install"/"update": Wow.exe's
REM manifest has no requestedExecutionLevel, so UAC installer detection kicks in on
REM such names and CreateProcess fails with ERROR_ELEVATION_REQUIRED (740).
echo [6/7] Patching Wow.exe -> Wow_ebonhold.exe (imports ebonhold.dll only)...
"%~dp0ebonhold_applymod.exe" "%GAME%\Wow.exe" "%GAME%\Wow_ebonhold.exe" || goto :err

echo [7/7] Done staging.
echo.
echo DONE. Run "%GAME%\Wow_ebonhold.exe".
goto :eof

:err
echo BUILD FAILED (exit %errorlevel%).
exit /b 1
