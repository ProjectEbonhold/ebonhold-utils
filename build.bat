@echo off
REM Rebuild ebonhold.dll (with the embedded glue script) + the patcher, stage the
REM DLL next to Wow.exe, and regenerate Wow_patched.exe. Run from this folder.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
call "%VCVARS%" >nul
cd /d "%~dp0"

echo [1/6] Embedding ebonhold_glue.lua -> glue_script.h ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_glue.ps1" || goto :err

echo [2/6] Compiling version resources...
rc /nologo /fo ebonhold_dll.res ebonhold_dll.rc || goto :err
rc /nologo /fo ebonhold_exe.res ebonhold_exe.rc || goto :err

REM /DYNAMICBASE (ASLR) + /NXCOMPAT (DEP) are standard hardening; their absence is
REM itself a mild heuristic flag. The version .res gives the binary real metadata.
echo [3/6] Building ebonhold.dll (x86)...
cl /nologo /LD /MT /Od /EHsc dllmain.cpp ebonhold_dll.res /Fe:ebonhold.dll /link /MACHINE:X86 /DYNAMICBASE /NXCOMPAT || goto :err

REM /guard:cf (Control Flow Guard) is safe for the patcher (normal code) but NOT
REM for the DLL, which calls into the game by raw offset -- CFG would reject those
REM targets and crash. So CFG is patcher-only.
echo [4/6] Building patcher (ebonhold_applymod.exe)...
cl /nologo /O2 /MT /EHsc /guard:cf patcher.cpp ebonhold_exe.res /Fe:ebonhold_applymod.exe /link /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /DYNAMICBASE /NXCOMPAT /guard:cf || goto :err

echo [5/6] Staging ebonhold.dll next to Wow.exe...
copy /Y ebonhold.dll "..\ebonhold.dll" >nul || goto :err

echo [6/6] Patching Wow.exe -> Wow_patched.exe...
"%~dp0ebonhold_applymod.exe" "%~dp0..\Wow.exe" "%~dp0..\Wow_patched.exe" || goto :err

echo.
echo DONE. Run Wow_patched.exe (ebonhold.dll must sit beside it).
goto :eof

:err
echo BUILD FAILED (exit %errorlevel%).
exit /b 1
