# End-to-end check of the installer on a CI runner: an older "portable" copy's
# shortcuts and sign-in entry must move to the installed copy, a reinstall must
# keep the same folder, and uninstalling must remove the files and its sign-in entry.
param([Parameter(Mandatory)][string]$Setup)
$ErrorActionPreference = 'Stop'
$shell = New-Object -ComObject WScript.Shell
$desktop = [Environment]::GetFolderPath('Desktop')
$pins = Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'
New-Item -ItemType Directory -Force -Path $pins | Out-Null
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
function New-Link($path, $target) { $link = $shell.CreateShortcut($path); $link.TargetPath = $target; $link.Save() }
function Assert($condition, $message) { if (-not $condition) { throw "FAIL: $message" }; Write-Output "PASS: $message" }

# An older portable copy, running, with a desktop shortcut, a taskbar pin and launch at sign-in.
$portable = Join-Path $env:RUNNER_TEMP 'portable\BatteryScope.exe'
New-Item -ItemType Directory -Force -Path (Split-Path $portable) | Out-Null
Copy-Item 'build\Release\BatteryScope.exe' $portable
New-Link (Join-Path $desktop 'BatteryScope (old).lnk') $portable
New-Link (Join-Path $pins 'BatteryScope.lnk') $portable
New-Link (Join-Path $desktop 'Unrelated.lnk') "$env:WINDIR\notepad.exe"
New-ItemProperty -Path $run -Name BatteryScope -Value "`"$portable`" --tray" -Force | Out-Null
$old = Start-Process $portable -ArgumentList '--tray' -PassThru
Start-Sleep -Seconds 5

Start-Process $Setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LOG=install.log' -Wait
$installed = Join-Path $env:LOCALAPPDATA 'Programs\BatteryScope\BatteryScope.exe'
Assert (Test-Path $installed) 'installs per user into %LOCALAPPDATA%\Programs\BatteryScope'
Assert ($old.HasExited) 'the running portable copy was closed before installing'
Assert ((Get-ItemProperty 'HKCU:\Software\BatteryScope').InstallPath -eq (Split-Path $installed)) 'records the install folder'
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'BatteryScope.lnk'
Assert ((Test-Path $startMenu) -and $shell.CreateShortcut($startMenu).TargetPath -eq $installed) 'creates a Start menu shortcut'
Assert ($shell.CreateShortcut((Join-Path $desktop 'BatteryScope (old).lnk')).TargetPath -eq $installed) 'repoints an old desktop shortcut'
Assert ($shell.CreateShortcut((Join-Path $pins 'BatteryScope.lnk')).TargetPath -eq $installed) 'repoints an old taskbar pin'
Assert ($shell.CreateShortcut((Join-Path $desktop 'Unrelated.lnk')).TargetPath -like '*notepad.exe') 'leaves other shortcuts alone'
Assert ((Get-ItemProperty $run).BatteryScope -eq "`"$installed`" --tray") 'repoints launch at sign-in'
Start-Sleep -Seconds 3
Assert ((Get-Process BatteryScope -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $installed }).Count -ge 1) 'opens the installed copy after installing'

# Updating over a running installed copy keeps the same folder and reopens it.
Start-Process $Setup -ArgumentList '/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
Assert (Test-Path $installed) 'reinstall keeps the same folder'
Start-Sleep -Seconds 3
Assert ((Get-Process BatteryScope -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $installed }).Count -eq 1) 'exactly one copy runs after an update'

$uninstaller = Join-Path (Split-Path $installed) 'unins000.exe'
Start-Process $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
Start-Sleep -Seconds 3
Assert (-not (Test-Path $installed)) 'uninstall removes the program'
Assert ($null -eq (Get-ItemProperty $run -ErrorAction SilentlyContinue).BatteryScope) 'uninstall removes its launch at sign-in'
Assert (-not (Test-Path $startMenu)) 'uninstall removes the Start menu shortcut'
Remove-Item (Join-Path $desktop 'BatteryScope (old).lnk'), (Join-Path $desktop 'Unrelated.lnk'), (Join-Path $pins 'BatteryScope.lnk') -ErrorAction SilentlyContinue
