@echo off
REM deploy.bat - rebuild + stage + re-patch, WITHOUT the gen_glue.ps1 step.
REM
REM Use this instead of build.bat when ebonhold_glue.lua has NOT changed (the
REM embedded glue_script.h is already current). It avoids the PowerShell
REM -ExecutionPolicy Bypass call that some AV/endpoint policies block.
REM
REM If you DID edit ebonhold_glue.lua, run build.bat instead so glue_script.h is
REM regenerated first.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
call "%VCVARS%" >nul || (echo ERROR: vcvars32 not found & exit /b 1)
cd /d "%~dp0"

echo [1/5] Compiling version resources...
rc /nologo /fo ebonhold_dll.res ebonhold_dll.rc || goto :err
rc /nologo /fo ebonhold_exe.res ebonhold_exe.rc || goto :err

echo [2/5] Building ebonhold.dll (x86, W-xor-X detour, ASLR/DEP, version info)...
cl /nologo /LD /MT /Od /EHsc dllmain.cpp ebonhold_dll.res /Fe:ebonhold.dll /link /MACHINE:X86 /DYNAMICBASE /NXCOMPAT || goto :err

echo [3/5] Building patcher (ebonhold_applymod.exe, CFG, asInvoker manifest)...
cl /nologo /O2 /MT /EHsc /guard:cf patcher.cpp ebonhold_exe.res /Fe:ebonhold_applymod.exe /link /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /DYNAMICBASE /NXCOMPAT /guard:cf || goto :err

echo [4/5] Staging ebonhold.dll next to Wow.exe...
copy /Y ebonhold.dll "..\ebonhold.dll" >nul || goto :err

echo [5/5] Patching Wow.exe -> Wow_patched.exe...
if not exist "%~dp0..\Wow.exe" (
    echo   SKIPPED: ..\Wow.exe not found. DLL is built and staged; copy Wow.exe
    echo   into the parent folder and re-run, or run ebonhold_applymod.exe manually.
    goto :done
)
"%~dp0ebonhold_applymod.exe" "%~dp0..\Wow.exe" "%~dp0..\Wow_patched.exe" || goto :err

:done
echo.
echo DONE. ebonhold.dll rebuilt and staged. Run Wow_patched.exe (the DLL must sit beside it).
goto :eof

:err
echo BUILD FAILED (exit %errorlevel%).
exit /b 1
