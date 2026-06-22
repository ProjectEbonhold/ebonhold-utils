@echo off
REM Rebuild ebonhold.dll (with the embedded glue script) + the patcher, stage the
REM DLL next to Wow.exe, and regenerate Wow_patched.exe. Run from this folder.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
call "%VCVARS%" >nul
cd /d "%~dp0"

echo [1/5] Embedding ebonhold_glue.lua -> glue_script.h ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_glue.ps1" || goto :err

echo [2/5] Building ebonhold.dll (x86)...
cl /nologo /LD /MT /Od /EHsc dllmain.cpp /Fe:ebonhold.dll /link /MACHINE:X86 || goto :err

echo [3/5] Building patcher (ebonhold_applymod.exe)...
cl /nologo /O2 /MT /EHsc patcher.cpp /Fe:ebonhold_applymod.exe /link /MANIFESTUAC:level='asInvoker' || goto :err

echo [4/5] Staging ebonhold.dll next to Wow.exe...
copy /Y ebonhold.dll "..\ebonhold.dll" >nul || goto :err

echo [5/5] Patching Wow.exe -> Wow_patched.exe...
"%~dp0ebonhold_applymod.exe" "%~dp0..\Wow.exe" "%~dp0..\Wow_patched.exe" || goto :err

echo.
echo DONE. Run Wow_patched.exe (ebonhold.dll must sit beside it).
goto :eof

:err
echo BUILD FAILED (exit %errorlevel%).
exit /b 1
