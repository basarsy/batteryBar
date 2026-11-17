# Removes BatteryBar: stops it, deletes the autostart entry, settings key and installed files.
Get-Process BatteryBar -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name BatteryBar -ErrorAction SilentlyContinue
Remove-Item 'HKCU:\Software\BatteryBar' -Recurse -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
Remove-Item (Join-Path $env:LOCALAPPDATA 'Programs\BatteryBar') -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'BatteryBar removed.'
