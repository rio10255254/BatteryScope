param([switch]$Test, [string]$OutputDirectory = 'dist')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$compiler = (Get-Command g++.exe -ErrorAction SilentlyContinue).Source
if (-not $compiler -and (Test-Path -LiteralPath 'C:\mingw64\bin\g++.exe')) { $compiler = 'C:\mingw64\bin\g++.exe' }
if (-not $compiler) { throw 'Install a MinGW-w64 C++17 toolchain and add its bin folder to PATH.' }
$resourceCompiler = Join-Path (Split-Path $compiler) 'windres.exe'
$buildOutput = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $projectRoot $OutputDirectory }
New-Item -ItemType Directory -Path $buildOutput -Force | Out-Null
$resourceOutput = Join-Path $buildOutput 'resources.o'
$applicationOutput = Join-Path $buildOutput 'BatteryScope.exe'
$testOutput = Join-Path $buildOutput 'BatteryScopeTests.exe'
Push-Location (Join-Path $projectRoot 'src')
try {
    & $resourceCompiler app.rc -O coff -o $resourceOutput
    if ($LASTEXITCODE) { throw 'Resource compilation failed.' }
    $common = @('-std=c++17','-O2','-Wall','-Wextra','-Wpedantic','-DUNICODE','-D_UNICODE','-DNOMINMAX','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00','-static','-static-libgcc','-static-libstdc++','-finput-charset=UTF-8','-fexec-charset=UTF-8')
    & $compiler @common -municode -mwindows app.cpp ui.cpp model.cpp platform.cpp energy_model.cpp energy_reader.cpp update.cpp lab.cpp lab_system.cpp lab_workload.cpp governor.cpp activity.cpp asus.cpp $resourceOutput -o $applicationOutput -lcomctl32 -lshell32 -lwinhttp -lbcrypt -luuid -ldxgi -lcfgmgr32 -lws2_32 -ldwmapi -lsetupapi -ladvapi32 -lpsapi -luxtheme -ldwmapi -lgdiplus -lcomdlg32 -lole32 -lpdh -lgdi32 -luser32 '-Wl,--dynamicbase' '-Wl,--nxcompat' -s
    if ($LASTEXITCODE) { throw 'Application compilation failed.' }
    if ($Test) {
        & $compiler @common -municode ..\tests\tests.cpp model.cpp platform.cpp energy_model.cpp energy_reader.cpp update.cpp lab.cpp lab_system.cpp lab_workload.cpp governor.cpp activity.cpp asus.cpp -o $testOutput -lsetupapi -ladvapi32 -lpsapi -lpdh -luser32 -lshell32 -lwinhttp -lbcrypt -lole32 -luuid -ldxgi -ldwmapi -lcfgmgr32 -lws2_32 -lgdi32 -lgdi32
        if ($LASTEXITCODE) { throw 'Test compilation failed.' }
        & $testOutput
        if ($LASTEXITCODE) { throw 'Tests failed.' }
    }
    Get-Item -LiteralPath $applicationOutput | Select-Object FullName,Length,LastWriteTime
} finally { Pop-Location }
