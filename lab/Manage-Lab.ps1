#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidateSet('Preflight','EnableTestSigning','Install','Reattach','Upgrade','Uninstall','Status')]
    [string]$Action = 'Preflight',
    [int]$DiskNumber = -1,
    [long]$ExpectedBytes = 0,
    [switch]$SnapshotConfirmed,
    [string]$PackageDirectory,
    [switch]$AllowWriteCache,
    [switch]$AllowFormattedDisk,
    [string]$ExpectedInstanceId
)
$ErrorActionPreference = 'Stop'
$package = Split-Path $PSScriptRoot -Parent
if ($PackageDirectory) { $package = (Resolve-Path -LiteralPath $PackageDirectory).Path }
$stateRoot = Join-Path $env:ProgramData 'QueueCacheLab'
New-Item -ItemType Directory -Path "$stateRoot/Logs" -Force | Out-Null
$log = "$stateRoot/Logs/Lab-$Action-$(Get-Date -Format yyyyMMdd-HHmmss)-$PID.log"
Start-Transcript -Path $log | Out-Null
function Log([string]$message) { Write-Host "[$(Get-Date -Format HH:mm:ss)] $message" }
function Native([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE) { throw "$exe failed: $LASTEXITCODE" }
}
function InspectFilter([string]$instanceId) {
    $result = & "$package/controller/qcache.exe" lab-filter inspect $instanceId
    if ($LASTEXITCODE) { throw 'Unable to inspect target devnode filters.' }
    return $result | ConvertFrom-Json
}
function ChangeFilter([string]$instanceId, [string]$driverKey, [string]$operation) {
    Native "$package/controller/qcache.exe" @('lab-filter', $operation, $instanceId, $driverKey, '--lab-installer')
}
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    if ($Action -eq 'Preflight') {
        Log 'Read-only machine checks (apart from this transcript).'
        Get-Disk | Select-Object Number,FriendlyName,Size,PartitionStyle,IsBoot,IsSystem | Format-Table
        Get-CimInstance Win32_DiskDrive | Select-Object Index,PNPDeviceID | Format-List
        Log "Secure Boot enabled: $(Confirm-SecureBootUEFI)"
        Native bcdedit.exe @('/enum','{current}')
        Get-BitLockerVolume | Select-Object MountPoint,ProtectionStatus,VolumeStatus | Format-Table
        Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard -ClassName Win32_DeviceGuard |
            Select-Object SecurityServicesRunning | Format-List
        return
    }
    if ($Action -eq 'EnableTestSigning') {
        if (-not $SnapshotConfirmed) { throw 'Confirm a current VM snapshot with -SnapshotConfirmed.' }
        if (Confirm-SecureBootUEFI) { throw 'Disable Secure Boot in the VM firmware first. Do not delete EFI disk/keys.' }
        $protected = @(Get-BitLockerVolume | Where-Object { $_.ProtectionStatus -ne 'Off' })
        if ($protected.Count) { throw 'BitLocker protection is active. Secure recovery keys and review protection before proceeding.' }
        Native bcdedit.exe @('/set','{current}','testsigning','on')
        Log 'Test-signing configured. Reboot the VM before installation. No driver installed; no automatic reboot.'
        return
    }
    $stateFile = "$stateRoot/qcachelab-install.json"
    if ($Action -eq 'Status') {
        if (Test-Path $stateFile) { Get-Content $stateFile }
        Get-Service qcachelab -ErrorAction SilentlyContinue | Format-List
        if ($DiskNumber -ge 0) { Native "$package/controller/qcache.exe" @('status', "PhysicalDrive$DiskNumber", '--json') }
        return
    }
    if ($Action -eq 'Uninstall') {
        if (-not (Test-Path $stateFile)) { throw 'No recorded installation. Do not guess an instance ID.' }
        $state = Get-Content $stateFile -Raw | ConvertFrom-Json
        $current = InspectFilter $state.InstanceId
        Log "Removing only qcachelab from $($state.InstanceId). Other filters are preserved."
        ChangeFilter $state.InstanceId $state.DriverKey 'remove'
        # Keep the service and binary until the filter has been unloaded by reboot.
        # Deleting/disableing a still-referenced filter service can make the disk fail to start.
        Log 'Filter registration removed. Reboot; do not stop/delete the service while attached.'
        Log 'After reboot verify the disk is healthy. Service/binary are retained for recovery and later cleanup.'
        return
    }
    if (-not $SnapshotConfirmed) { throw 'Confirm a current whole-VM snapshot with -SnapshotConfirmed.' }
    if ($DiskNumber -le 0 -or $ExpectedBytes -le 0) { throw 'Specify a nonzero secondary disk number and exact expected size.' }
    $upgrade = $Action -eq 'Upgrade'
    $reattach = $Action -in @('Reattach','Upgrade')
    if ($AllowFormattedDisk -and -not $reattach -and -not $ExpectedInstanceId) { throw 'Initial formatted-disk attachment requires an explicit expected PnP identity.' }
    if ((Test-Path $stateFile) -and -not $reattach) { throw 'Installation state already exists. Inspect it; do not overwrite recovery information.' }
    if ($reattach -and -not (Test-Path $stateFile)) { throw 'Reattachment requires the recorded original installation.' }
    if (Confirm-SecureBootUEFI) { throw 'Secure Boot remains enabled. Complete the firmware/test-signing prerequisites first.' }
    # Check the active kernel setting, not merely a pending BCD edit.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LabCodeIntegrity {
    [StructLayout(LayoutKind.Sequential)] public struct Info { public uint Length; public uint Options; }
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, ref Info info, uint size, out uint required);
    public static bool TestSigningActive() { var i = new Info { Length = 8 }; uint n;
        int s = NtQuerySystemInformation(103, ref i, 8, out n);
        if (s < 0) throw new InvalidOperationException("Cannot query active Code Integrity: " + s);
        return (i.Options & 2) != 0;
    }
}
'@
    if (-not [LabCodeIntegrity]::TestSigningActive()) { throw 'Test-signing is not active in this boot. Enable it and reboot first.' }
    $disk = Get-Disk -Number $DiskNumber
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.Size -ne $ExpectedBytes) {
        throw 'Requires a secondary disk matching the exact expected byte size, never boot/system storage.'
    }
    $originalStyle = $disk.PartitionStyle
    $originalPartitionCount = $disk.NumberOfPartitions
    if (-not $AllowFormattedDisk -and ($originalStyle -ne 'RAW' -or $originalPartitionCount -ne 0)) {
        throw 'Requires an empty RAW disk, or recorded Reattach/Upgrade with explicit -AllowFormattedDisk. Nothing is formatted.'
    }
    $identities = @(Get-CimInstance Win32_DiskDrive | Where-Object Index -eq $DiskNumber)
    if ($identities.Count -ne 1) { throw 'Cannot uniquely resolve disk number to PnP identity.' }
    $instanceId = $identities[0].PNPDeviceID
    if ($ExpectedInstanceId -and $ExpectedInstanceId -ne $instanceId) { throw 'Disk PnP identity differs from the explicitly selected target.' }
    $letters = @(Get-Partition -DiskNumber $DiskNumber | Where-Object DriveLetter | ForEach-Object { "$($_.DriveLetter):" })
    if (@(Get-CimInstance Win32_PageFileUsage | Where-Object { $_.Name.Substring(0,2) -in $letters }).Count) { throw 'Paging disk attachment is unsupported.' }
    $before = InspectFilter $instanceId
    if (@($before.UpperFilters).Count) { throw 'Initial lab install requires no existing per-device upper filters; review manually instead.' }
    $servicePath = 'HKLM:\SYSTEM\CurrentControlSet\Services\qcachelab'
    $binary = Join-Path $env:windir 'System32/drivers/qcachelab.sys'
    if ($reattach) {
        $saved = Get-Content $stateFile -Raw | ConvertFrom-Json
        if ($saved.InstanceId -ne $instanceId -or $saved.DriverKey -ne $before.DriverKey) { throw 'Reattachment identity differs from the recorded disk.' }
        if ((Get-Service qcachelab).Status -ne 'Stopped') { throw 'Reboot after removal before reattaching.' }
        $service = Get-ItemProperty $servicePath
        if ($service.LabAllowedDriverKey -ne $before.DriverKey -or $service.Start -ne 3 -or
            $service.ImagePath -ne 'System32\drivers\qcachelab.sys') { throw 'Unexpected retained service configuration.' }
        if (-not $upgrade -and (Get-FileHash $binary).Hash -ne (Get-FileHash "$package/driver/qcachelab.sys").Hash) { throw 'Retained driver differs from the signed package. Use explicit Upgrade only after removal/reboot.' }
    } elseif ((Test-Path $servicePath) -or (Test-Path $binary)) { throw 'Existing qcachelab service/binary: review before installation.' }
    $metadata = Get-Content "$package/build-info.json" -Raw | ConvertFrom-Json
    if ((-not $metadata.labPassThrough -and -not ($metadata.labWriteCache -and $AllowWriteCache)) -or -not $metadata.driverSigned) { throw 'Expected signed lab package; write-cache builds require explicit -AllowWriteCache.' }
    foreach ($line in (Get-Content "$package/SHA256SUMS.txt")) {
        $parts = $line -split '  ', 2
        $path = [IO.Path]::GetFullPath((Join-Path $package $parts[1]))
        if (-not $path.StartsWith([IO.Path]::GetFullPath($package).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid checksum path.' }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $parts[0]) { throw "Hash mismatch: $($parts[1])" }
    }
    $signature = Get-AuthenticodeSignature "$package/driver/qcachelab.sys"
    if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $metadata.testCertificateThumbprint) {
        throw 'Driver signing certificate does not match package metadata.'
    }
    if ($signature.Status -notin @('Valid','UnknownError','NotTrusted')) { throw "Invalid signature: $($signature.Status)" }
    # Re-read immediately before mutation; never select by size alone.
    $disk = Get-Disk -Number $DiskNumber
    $again = @(Get-CimInstance Win32_DiskDrive | Where-Object Index -eq $DiskNumber)
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.Size -ne $ExpectedBytes -or $disk.PartitionStyle -ne $originalStyle -or
        $disk.NumberOfPartitions -ne $originalPartitionCount -or
        $again.Count -ne 1 -or $again[0].PNPDeviceID -ne $instanceId) { throw 'Disk identity changed during preflight.' }
    if ($upgrade) {
        # Only after removal + reboot, signature/hash validation and exact-target checks.
        # Preserve the previous binary and original install identity for recovery.
        if ((Get-Service qcachelab).Status -ne 'Stopped') { throw 'Service changed state before upgrade.' }
        $backupRoot = Join-Path $stateRoot 'BinaryBackups'
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        $oldHash = (Get-FileHash $binary).Hash
        $backup = Join-Path $backupRoot "qcachelab-$oldHash.sys"
        if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $binary -Destination $backup }
        if ((Get-FileHash -LiteralPath $backup).Hash -ne $oldHash) { throw 'Previous binary backup verification failed.' }
        Log "Backing up stopped driver to $backup; installing validated version $($metadata.version)."
        Copy-Item -LiteralPath "$package/driver/qcachelab.sys" -Destination $binary -Force
        if ((Get-FileHash $binary).Hash -ne (Get-FileHash "$package/driver/qcachelab.sys").Hash) { throw 'Installed binary hash verification failed.' }
    }
    if (-not $reattach) {
    $before | ConvertTo-Json -Depth 4 | Set-Content $stateFile -Encoding UTF8
    Log "Recorded recovery identity: $instanceId ($ExpectedBytes bytes)."
    Log 'Copying signed qcachelab.sys; creating demand-start service. Not starting or restarting any device.'
    Copy-Item "$package/driver/qcachelab.sys" $binary
    Native sc.exe @('create','qcachelab','type=','kernel','start=','demand','error=','normal','binPath=','System32\drivers\qcachelab.sys')
    New-ItemProperty -Path $servicePath -Name LabAllowedDriverKey -PropertyType String -Value $before.DriverKey | Out-Null
    } else { Log 'Reusing verified stopped service and original recovery state.' }
    ChangeFilter $instanceId $before.DriverKey 'add'
    Log 'Registered on the ONE selected devnode. Reboot to load. Cache starts disabled; explicit configure/enable required.'
    Log "Recovery: run this script -Action Uninstall, then reboot; or restore the whole-VM snapshot. State: $stateFile"
} catch {
    Log "FAILED: $($_.Exception.Message)"
    Log 'Stop and inspect the transcript/state; do not repeatedly install over a partial result.'
    throw
} finally { Stop-Transcript | Out-Null; Write-Host "Log: $log" }
