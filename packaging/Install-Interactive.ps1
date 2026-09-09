#Requires -RunAsAdministrator
[CmdletBinding()]
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$package = Split-Path $PSScriptRoot -Parent
$controller = Join-Path $package 'controller\qcache.exe'
$manager = Join-Path $package 'lab\Manage-Lab.ps1'
$statePath = Join-Path $env:ProgramData 'QueueCacheLab\qcachelab-install.json'
function Manage([hashtable]$Options) {
    # A child process isolates Add-Type declarations across repeated installer invocations.
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$manager,'-PackageDirectory',$package)
    foreach ($key in $Options.Keys) { $arguments += "-$key"; if ($Options[$key] -isnot [switch] -and $Options[$key] -isnot [bool]) { $arguments += [string]$Options[$key] } }
    & powershell.exe @arguments
    if ($LASTEXITCODE) { throw "Driver operation failed: $LASTEXITCODE" }
}
function UpdatePath([bool]$Remove) {
    $entry = Join-Path $package 'controller'
    $current = [Environment]::GetEnvironmentVariable('Path','Machine')
    $parts = @($current -split ';' | Where-Object { $_ -and $_.TrimEnd('\') -ine $entry.TrimEnd('\') })
    if (-not $Remove) { $parts += $entry }
    [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'Machine')
}
try {
    if ($Uninstall) {
        if (Test-Path -LiteralPath $statePath) {
            $recorded = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
            $disk = @(Get-CimInstance Win32_DiskDrive | Where-Object PNPDeviceID -eq $recorded.InstanceId)
            if ($disk.Count -ne 1) { throw 'Recorded disk unavailable; do not guess a removal target.' }
            if ((Get-Service qcachelab -ErrorAction SilentlyContinue).Status -eq 'Running') {
                & $controller disable "PhysicalDrive$($disk[0].Index)"
                if ($LASTEXITCODE) { throw 'Drain failed. Driver and recovery tools retained.' }
            }
            Manage @{Action='Uninstall'}
        }
        UpdatePath $true
        Unregister-ScheduledTask -TaskName 'QueueCache-Restore' -Confirm:$false -ErrorAction SilentlyContinue
        Write-Host 'Reboot to unload the driver. The retained kernel service/binary preserve the existing recovery path.'
        exit 3010
    }
    Write-Host 'QueueCache EXPERIMENTAL LAB installer. Secondary disposable disks only.' -ForegroundColor Yellow
    Write-Host 'Requires a snapshot, Secure Boot disabled and test-signing active. No automatic security changes or cache enablement.'
    UpdatePath $false
    $restoreAction = New-ScheduledTaskAction -Execute $controller -Argument 'restore'
    $restoreTrigger = New-ScheduledTaskTrigger -AtStartup
    $restoreTrigger.Delay = 'PT30S'
    $restorePrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $restoreSettings = New-ScheduledTaskSettingsSet -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
    Register-ScheduledTask -TaskName 'QueueCache-Restore' -Action $restoreAction -Trigger $restoreTrigger -Principal $restorePrincipal -Settings $restoreSettings -Force | Out-Null
    Write-Host 'Startup task registered. Only explicitly saved HKLM profiles are restored; no saved profile means no automatic caching.'
    $disks = @(Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem -and $_.Number -gt 0 })
    $disks | Select-Object Number,FriendlyName,Size,PartitionStyle | Format-Table
    $answer = Read-Host 'Type the secondary disk number (or blank to install applications only)'
    if ([string]::IsNullOrWhiteSpace($answer)) { exit 0 }
    $number = -1
    if (-not [int]::TryParse($answer, [ref]$number)) { throw 'Invalid disk number.' }
    $selected = @($disks | Where-Object Number -eq $number)
    if ($selected.Count -ne 1) { throw 'Not an eligible secondary disk.' }
    if ((Read-Host 'Type SNAPSHOT to confirm a current whole-VM snapshot and disposable target') -cne 'SNAPSHOT') { throw 'Snapshot confirmation required.' }
    $options = @{Action='Install';DiskNumber=$number;ExpectedBytes=$selected[0].Size;SnapshotConfirmed=$true;AllowWriteCache=$true}
    if ($selected[0].PartitionStyle -ne 'RAW') {
        $identity = Get-CimInstance Win32_DiskDrive -Filter "Index=$number"
        Write-Host "Existing data/partitions will NOT be formatted. Selected PnP identity: $($identity.PNPDeviceID)"
        if ((Read-Host 'Type ATTACH to explicitly allow filtering this formatted secondary disk') -cne 'ATTACH') { throw 'Formatted target not confirmed.' }
        $options.AllowFormattedDisk = $true; $options.ExpectedInstanceId = $identity.PNPDeviceID
    }
    if (Test-Path -LiteralPath $statePath) {
        $recorded = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        $identity = Get-CimInstance Win32_DiskDrive -Filter "Index=$number"
        if ($identity.PNPDeviceID -ne $recorded.InstanceId) { throw 'Selection differs from the recorded installation.' }
        if ((Get-Service qcachelab).Status -eq 'Running') {
            & $controller disable "PhysicalDrive$number"
            if ($LASTEXITCODE) { throw 'Drain failed; refusing upgrade.' }
            Manage @{Action='Uninstall'}
            Write-Host 'Upgrade stage 1 complete. Reboot, then run the Driver setup shortcut again to install the new binary.'
            exit 3010
        }
        $options.Action = 'Upgrade'; $options.AllowFormattedDisk = $true
    }
    Manage $options
    Write-Host 'Driver registered. Reboot, then select the volume and Apply in QueueCache. Fast preset is the default; risk acknowledgement is explicit.'
    exit 3010
} catch {
    Write-Host "SETUP FAILED: $_" -ForegroundColor Red
    Read-Host 'Press Enter to close and retain the installed recovery tools'
    exit 1
}
