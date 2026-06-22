# Generates glue_script.h by embedding ..\ebonhold_glue.lua as a C++ raw string.
# Run by build.bat before compiling, so the DLL always carries the current script.
$lua = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'ebonhold_glue.lua'))
$h = "#pragma once`r`n" +
     "// AUTO-GENERATED from ebonhold_glue.lua by gen_glue.ps1 - do not edit.`r`n" +
     "static const char kGlueScript[] = R`"EBONHOLD(`r`n" +
     $lua +
     "`r`n)EBONHOLD`";`r`n"
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot 'glue_script.h'), $h, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("gen_glue: embedded {0} bytes into glue_script.h" -f $lua.Length)
