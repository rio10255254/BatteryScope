# Builds dist\BatteryScope-<version>-setup.exe from an existing Release build.
# Installs Inno Setup with Chocolatey when it is missing (GitHub runners).
param(
    [string]$SourceExe = "build\Release\BatteryScope.exe",
    [string]$OutputDir = "dist"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$match = Select-String -Path (Join-Path $root 'src\version.h') -Pattern 'BS_VERSION_STRING "([0-9.]+)"'
$version = $match.Matches[0].Groups[1].Value
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    choco install innosetup --no-progress -y | Out-Host
    $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
}
$source = (Resolve-Path (Join-Path $root $SourceExe)).Path
New-Item -ItemType Directory -Force -Path (Join-Path $root $OutputDir) | Out-Null
$output = (Resolve-Path (Join-Path $root $OutputDir)).Path
& $iscc "/DAppVersion=$version" "/DSourceExe=$source" "/DOutputDir=$output" (Join-Path $root 'installer\BatteryScope.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with $LASTEXITCODE" }
$setup = Join-Path $output "BatteryScope-$version-setup.exe"
if (-not (Test-Path $setup)) { throw "Missing $setup" }
Write-Output $setup
