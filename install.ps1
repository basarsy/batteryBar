# Installs BatteryBar for the current user (no admin needed) and starts it.
# It registers itself to start with Windows on first run (toggle via right-click menu).
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'build\BatteryBar.exe'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\BatteryBar'
if (-not (Test-Path $src)) { throw "Build first: $src not found" }

Get-Process BatteryBar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $src (Join-Path $dir 'BatteryBar.exe') -Force
Start-Process (Join-Path $dir 'BatteryBar.exe')
Write-Host "Installed to $dir and started."
