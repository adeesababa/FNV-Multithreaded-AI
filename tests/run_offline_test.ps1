# Builds tests\offline_test.cpp (32-bit, same flags as the plugin) and runs it.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;" + $env:PATH
$vs = & vswhere.exe -version "[16,17)" -property installationPath | Select -First 1
$out = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul && cl /nologo /O2 /EHa /MT /std:c++17 /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX `"$here\offline_test.cpp`" /Fe:`"$out\offline_test.exe`" /Fo:`"$out\\`" /link /INCREMENTAL:NO user32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { Write-Output "build failed"; exit 1 }
& "$out\offline_test.exe"
exit $LASTEXITCODE
