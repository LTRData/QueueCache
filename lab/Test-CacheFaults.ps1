#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageDirectory, [Parameter(Mandatory)][string]$TestExecutable)
$ErrorActionPreference = 'Stop'
$saved = Get-Content "$env:ProgramData/QueueCacheLab/qcachelab-install.json" -Raw | ConvertFrom-Json
$devices = @(Get-CimInstance Win32_DiskDrive | Where-Object PNPDeviceID -eq $saved.InstanceId)
if ($devices.Count -ne 1 -or $devices[0].Index -le 0) { throw 'Recorded secondary disk identity is not unique.' }
$disk = Get-Disk -Number $devices[0].Index
if ($disk.IsBoot -or $disk.IsSystem -or $disk.PartitionStyle -ne 'RAW' -or $disk.NumberOfPartitions) { throw 'Expected empty RAW secondary disk.' }
$target = "PhysicalDrive$($disk.Number)"
$cli = Join-Path $PackageDirectory 'controller/qcache.exe'
$root = Join-Path $PSScriptRoot 'Logs'
New-Item -ItemType Directory -Path $root -Force | Out-Null
$log = Join-Path $root "CacheFaults-$(Get-Date -Format yyyyMMdd-HHmmss).log"
function InvokeCacheCommand([string[]]$Arguments, [int]$Expected = 0) {
    Write-Host "> qcache $($Arguments -join ' ') (expected exit $Expected)"
    & $cli @Arguments
    if ($LASTEXITCODE -ne $Expected) { throw "Unexpected qcache exit: $LASTEXITCODE" }
}
function State {
    $json = & $cli cache-status $target --json
    if ($LASTEXITCODE) { throw 'Cannot obtain coherent cache snapshot.' }
    $json | Write-Host
    return $json | ConvertFrom-Json
}
function Workload([string]$Mode, [int]$Expected, [long]$Prefix = 0) {
    $arguments = @([string]$disk.Number, [string]$disk.Size, $saved.InstanceId, $Mode)
    if ($Prefix) { $arguments += [string]$Prefix }
    $arguments += @('--seed', [string]$script:scenarioSeed)
    $nativePreference = $ErrorActionPreference
    try {
        # Explicit capture: SSH PowerShell transcripts can omit native stdout.
        $ErrorActionPreference = 'Continue'
        & $TestExecutable @arguments 2>&1 | Tee-Object -FilePath "$root/CacheFaultWorkload-$script:scenarioSeed-$($Mode.TrimStart('-')).log"
    } finally { $ErrorActionPreference = $nativePreference }
    if ($LASTEXITCODE -ne $Expected) { throw "Unexpected test exit $LASTEXITCODE (expected $Expected)." }
}
Start-Transcript $log | Out-Null
try {
    $initial = State
    if (-not $initial.Enabled -or $initial.Faulted -or $initial.DirtyBytes -or $initial.DeviceBytes -ne $disk.Size) {
        throw 'Fault tests require a healthy, enabled, empty cache on the verified disk.'
    }
    InvokeCacheCommand @('lab-delay', $target, '0')
    foreach ($mode in 1,2,4,5) {
        $script:scenarioSeed = [DateTime]::UtcNow.Ticks
        Write-Host "SYNTHETIC drain fault mode $mode. Expected workload/flush failures follow."
        $before = State
        InvokeCacheCommand @('lab-fault', $target, [string]$mode)
        Workload '--write-disposable-region' 1
        $fault = State
        $prefix = [long]$fault.AcceptedBytes - [long]$before.AcceptedBytes
        $expectedStatus = if ($mode -in 1,4) { -1073741435 } else { -1073741668 }
        if (-not $fault.Faulted -or $fault.LastError -ne $expectedStatus -or $fault.DirtyBytes -le 0 -or $prefix -le 0 -or
            $fault.Errors -ne ($before.Errors + 1) -or $fault.DirtyBytes -ne ($fault.AcceptedBytes - $fault.DrainedBytes - $fault.CoalescedBytes - $fault.DiscardedBytes)) {
            throw 'Failed drain did not retain and account for accepted dirty data.'
        }
        InvokeCacheCommand @('flush', $target) 1
        $stillFaulted = State
        if ($stillFaulted.DirtyBytes -ne $fault.DirtyBytes -or -not $stillFaulted.Faulted) { throw 'Flush silently discarded faulted data.' }
        InvokeCacheCommand @('retry', $target)
        $recovered = State
        if ($recovered.Faulted -or $recovered.DirtyBytes -or $recovered.InFlightBytes -or $recovered.AcceptedBytes -ne ($recovered.DrainedBytes + $recovered.CoalescedBytes + $recovered.DiscardedBytes)) {
            throw 'Retry did not fully drain and recover.'
        }
        # Every accepted write before these injected faults was part of the base-pattern prefix.
        Workload '--verify-base-prefix' 0 $prefix
        Write-Host "PASS: fault $mode retained data, flush failed, retry recovered $prefix verified bytes."
    }
    Write-Host 'SYNTHETIC flush fault. Expected FlushFileBuffers/workload failure follows.'
    $script:scenarioSeed = [DateTime]::UtcNow.Ticks
    InvokeCacheCommand @('lab-fault', $target, '3')
    Workload '--write-disposable-region' 1
    $flushFault = State
    if (-not $flushFault.Faulted -or -not $flushFault.LastError) { throw 'Flush error was not visible.' }
    InvokeCacheCommand @('flush', $target) 1
    InvokeCacheCommand @('retry', $target)
    Workload '--verify-only' 0
    InvokeCacheCommand @('lab-fault', $target, '0')
    InvokeCacheCommand @('disable', $target)
    $final = State
    if ($final.Enabled -or $final.Faulted -or $final.DirtyBytes -or $final.InFlightBytes) { throw 'Final disable did not leave a clean cache.' }
    foreach ($mode in 6,7) {
        foreach ($attempt in 1..8) {
            InvokeCacheCommand @('lab-fault', $target, [string]$mode)
            InvokeCacheCommand @('configure', $target, '4') 1
            $failedAllocation = State
            if ($failedAllocation.Enabled -or $failedAllocation.ReservedBytes -or $failedAllocation.PayloadCapacity -or
                $failedAllocation.BudgetBytes -or $failedAllocation.DirtyBytes -or $failedAllocation.InFlightBytes) {
                throw 'Failed allocation left usable or partially published storage.'
            }
            InvokeCacheCommand @('enable', $target) 1
            InvokeCacheCommand @('configure', $target, '4')
        }
        InvokeCacheCommand @('enable', $target)
        $script:scenarioSeed = [DateTime]::UtcNow.Ticks
        Workload '--write-and-read-disposable-region' 0
        InvokeCacheCommand @('disable', $target)
        Write-Host "PASS: allocation fault $mode rejected enable, repeated cleanup and subsequent cache I/O recovered."
    }
    Write-Host 'PASS: pre-submission and completion-boundary faults, retained-data retry, flush error, allocation-failure recovery and final disable.'
} finally { Stop-Transcript | Out-Null; Write-Host "Log: $log" }
