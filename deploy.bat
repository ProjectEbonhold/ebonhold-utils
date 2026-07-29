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
REM The live 3.3.5 client. Everything is staged here, next to Wow.exe.
set GAME=%~dp0..\WoW-Ebonhold
call "%VCVARS%" >nul || (echo ERROR: vcvars32 not found & exit /b 1)
cd /d "%~dp0"

echo [1/5] Compiling version resources...
rc /nologo /fo ebonhold_dll.res ebonhold_dll.rc || goto :err
rc /nologo /fo ebonhold_exe.res ebonhold_exe.rc || goto :err

echo [2/5] Building ebonhold.dll (x86, W-xor-X detour, ASLR/DEP, version info)...
cl /nologo /LD /MT /Od /EHsc dllmain.cpp ebonhold_dll.res /Fe:ebonhold.dll /link /MACHINE:X86 /DYNAMICBASE /NXCOMPAT || goto :err

echo [3/5] Building patcher (ebonhold_applymod.exe, CFG, asInvoker manifest)...
cl /nologo /O2 /MT /EHsc /guard:cf patcher.cpp ebonhold_exe.res /Fe:ebonhold_applymod.exe /link /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /DYNAMICBASE /NXCOMPAT /guard:cf || goto :err

echo [4/5] Staging ebonhold.dll + ClientExt.dll into "%GAME%"...
copy /Y ebonhold.dll "%GAME%\ebonhold.dll" >nul || goto :err
REM ClientExt.dll is loaded at runtime by ebonhold.dll (LoadLibrary by name once the
REM client is up - see dllmain.cpp LoadClientExtOnce); it is NOT statically imported
REM and needs no export stamp. It only has to sit next to the exe.
if exist "%~dp0ClientExt.dll" copy /Y ClientExt.dll "%GAME%\ClientExt.dll" >nul || goto :err

REM Output name must NOT contain "patch"/"setup"/"install"/"update": Wow.exe's
REM manifest has no requestedExecutionLevel, so UAC installer detection kicks in on
REM such names and CreateProcess fails with ERROR_ELEVATION_REQUIRED (740).
echo [5/5] Patching Wow.exe -^> Wow_ebonhold.exe (imports ebonhold.dll only)...
if not exist "%GAME%\Wow.exe" (
    echo   SKIPPED: no Wow.exe in "%GAME%". DLLs are built and staged; fix the GAME
    echo   variable at the top of this script, or run ebonhold_applymod.exe manually.
    goto :done
)
"%~dp0ebonhold_applymod.exe" "%GAME%\Wow.exe" "%GAME%\Wow_ebonhold.exe" || goto :err

:done
echo.
echo DONE. DLLs rebuilt and staged. Run "%GAME%\Wow_ebonhold.exe".
goto :eof

:err
echo BUILD FAILED (exit %errorlevel%).
exit /b 1
