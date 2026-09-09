#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$TestExecutable,
    [ValidateSet('attached','detached')][string]$Mode = 'attached'
)
$ErrorActionPreference = 'Stop'
$state = Get-Content "$env:ProgramData/QueueCacheLab/qcachelab-install.json" -Raw | ConvertFrom-Json
$matches = @(Get-CimInstance Win32_DiskDrive | Where-Object PNPDeviceID -eq $state.InstanceId)
if ($matches.Count -ne 1) { throw 'Recorded disk identity is not unique/present.' }
$disk = Get-Disk -Number $matches[0].Index
if ($disk.Number -le 0 -or $disk.IsBoot -or $disk.IsSystem -or $disk.PartitionStyle -ne 'RAW') { throw 'Expected RAW secondary disk.' }
$cli = Join-Path $PackageDirectory 'controller/qcache.exe'
$json = & $cli lab-filter inspect $state.InstanceId
if ($LASTEXITCODE) { throw 'Device inspection failed.' }
$properties = $json | ConvertFrom-Json
if ($properties.DriverKey -ne $state.DriverKey) { throw 'Driver key changed.' }
$registered = @($properties.UpperFilters) -contains 'qcachelab'
if ($registered -ne ($Mode -eq 'attached')) { throw 'Filter registration does not match requested test phase.' }
$logRoot = Join-Path $PSScriptRoot 'Logs'
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$log = Join-Path $logRoot "ReadTest-$Mode-$(Get-Date -Format yyyyMMdd-HHmmss).log"
Start-Transcript $log | Out-Null
try {
    & $TestExecutable $cli $disk.Number $disk.Size $state.DriverKey $Mode
    if ($LASTEXITCODE) { throw "Read test failed: $LASTEXITCODE" }
} finally { Stop-Transcript | Out-Null; Write-Host "Log: $log" }
