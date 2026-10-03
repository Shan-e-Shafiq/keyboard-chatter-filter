#Requires -Version 5.1
<#
.SYNOPSIS
    Removes keyboard-chatter-filter from Windows. Safe to run more than once.

.DESCRIPTION
    Run from an elevated PowerShell:
        .\uninstall.ps1           # keep the configuration file and logs
        .\uninstall.ps1 -Purge    # also remove the configuration file and logs

    Only files and settings this project creates are removed.
#>
param([switch]$Purge)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$program = 'keyboard-chatter-filter'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Uninstalling needs administrator rights. Open PowerShell with 'Run as administrator'."
}

$installDir = Join-Path $env:ProgramFiles $program
$exe = Join-Path $installDir "$program.exe"

if (Get-Service -Name $program -ErrorAction SilentlyContinue) {
    Stop-Service -Name $program -Force -ErrorAction SilentlyContinue
    try { (Get-Service -Name $program).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) } catch { }
    & sc.exe delete $program | Out-Null
    Write-Host "Removed the '$program' service"
}

# Agents exit when the service stops; wait for them so the executable can be deleted.
for ($i = 0; $i -lt 20 -and (Get-Process -Name $program -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }

if (Test-Path -LiteralPath $exe) {
    Remove-Item -LiteralPath $exe -Force
    Write-Host "Removed $exe"
}
if ((Test-Path -LiteralPath $installDir) -and -not (Get-ChildItem -LiteralPath $installDir -Force)) {
    Remove-Item -LiteralPath $installDir -Force
}

$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$entries = $machinePath -split ';' | Where-Object { $_ -and ($_ -ne $installDir) }
if (($machinePath -split ';') -contains $installDir) {
    [Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'Machine')
    Write-Host "Removed $installDir from the system PATH"
}

if ($Purge) {
    $dataDir = Join-Path $env:ProgramData $program
    foreach ($file in @((Join-Path $dataDir 'config.toml'), (Join-Path $dataDir 'logs\service.log'), (Join-Path $dataDir 'logs\service.log.1'))) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force; Write-Host "Removed $file" }
    }
    foreach ($dir in @((Join-Path $dataDir 'logs'), $dataDir)) {
        if ((Test-Path -LiteralPath $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir -Force }
    }
    $userLogDir = Join-Path $env:LOCALAPPDATA $program
    foreach ($file in @((Join-Path $userLogDir 'agent.log'), (Join-Path $userLogDir 'agent.log.1'))) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force; Write-Host "Removed $file" }
    }
    if ((Test-Path -LiteralPath $userLogDir) -and -not (Get-ChildItem -LiteralPath $userLogDir -Force)) { Remove-Item -LiteralPath $userLogDir -Force }
}
Write-Host "Uninstalled."
