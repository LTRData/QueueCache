#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$Version = '0.1.0.0',
    [switch]$ManagedOnly,
    [switch]$LabPassThrough,
    [switch]$LabSerialized,
    [switch]$LabWriteCache
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or
    @($Version.Split('.') | Where-Object { [long]$_ -gt 65535 }).Count) {
    throw 'Version must contain four numeric fields, each in 0..65535.'
}
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    function Invoke-Checked([string]$Command, [string[]]$Arguments) {
        & $Command @Arguments
        if ($LASTEXITCODE) { throw "$Command failed with exit code $LASTEXITCODE" }
    }
    if ($LabWriteCache) { $LabPassThrough = [switch]::new($true); $LabSerialized = [switch]::new($true) }
    if ($ManagedOnly -and $LabPassThrough) { throw 'Choose controller-only or lab, not both.' }
    if ($LabSerialized -and -not $LabPassThrough) { throw 'Serialized worker is an explicit lab-only option; also specify -LabPassThrough.' }
    $flavor = if ($LabPassThrough) { 'lab' } else { 'legacy' }
    if ($LabSerialized) { $flavor = 'lab-serialized' }
    if ($LabWriteCache) { $flavor = 'lab-writecache' }
    $driverName = if ($LabPassThrough) { 'qcachelab' } else { 'qcache' }
    $kind = if ($ManagedOnly) { 'controller' } elseif ($LabPassThrough) { 'lab-passthrough-unsigned' } else { 'unsigned' }
    if ($LabSerialized) { $kind = 'lab-serialized-unsigned' }
    if ($LabWriteCache) { $kind = 'lab-writecache-unsigned' }
    # Each invocation uses a fresh staging directory; no stale binaries or private files.
    $name = "QueueCache-$Version-x64-$Configuration-$kind"
    $stage = Join-Path $root "artifacts/packages/$name-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Invoke-Checked dotnet @('run', '--project', 'tests/QueueCache.Management.Tests', '-c', $Configuration)
    Invoke-Checked dotnet @('publish', 'src/QueueCache.Cli', '-c', $Configuration,
        '-r', 'win-x64', '--self-contained', 'true', "-p:Version=$Version", '-o', "$stage/controller")
    Invoke-Checked dotnet @('publish', 'src/QueueCache.Desktop', '-c', $Configuration,
        '-r', 'win-x64', '--self-contained', 'true', "-p:Version=$Version", '-o', "$stage/desktop")
    if ($LabPassThrough) {
        Invoke-Checked dotnet @('publish', 'tests/QueueCache.LabTests', '-c', $Configuration,
            '-r', 'win-x64', '--self-contained', 'true', "-p:Version=$Version", '-o', "$stage/tests")
        Invoke-Checked dotnet @('publish', 'tests/QueueCache.WriteTests', '-c', $Configuration,
            '-r', 'win-x64', '--self-contained', 'true', "-p:Version=$Version", '-o', "$stage/write-tests")
        Invoke-Checked dotnet @('publish', 'tests/QueueCache.FileTests', '-c', $Configuration,
            '-r', 'win-x64', '--self-contained', 'true', "-p:Version=$Version", '-o', "$stage/file-tests")
    }
    if (-not $ManagedOnly) {
        & "$PSScriptRoot/Restore-Toolchain.ps1"
        $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
        $vsPath = & $vswhere -latest -products '*' -version '[18.0,19.0)' -property installationPath
        if (-not $vsPath) { throw 'Install Visual Studio 2026 with Desktop development with C++ and Windows Driver Kit.' }
        $msbuild = Join-Path $vsPath 'MSBuild/Current/Bin/MSBuild.exe'
        # Some launchers supply both Path and PATH. Framework MSBuild rejects that
        # environment when spawning CL. Normalize only this process, not Windows settings.
        $buildPath = $env:Path
        Remove-Item Env:Path -ErrorAction SilentlyContinue
        Remove-Item Env:PATH -ErrorAction SilentlyContinue
        $env:Path = $buildPath
        Invoke-Checked $msbuild @('driver/qcache/QueueCache.Driver.vcxproj', '/m', '/t:Rebuild',
            "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:BuildVersion=$Version",
            "/p:LabPassThrough=$($LabPassThrough.IsPresent.ToString().ToLowerInvariant())",
            "/p:LabSerialized=$($LabSerialized.IsPresent.ToString().ToLowerInvariant())",
            "/p:LabWriteCache=$($LabWriteCache.IsPresent.ToString().ToLowerInvariant())",
            "/bl:artifacts/driver-$flavor-$Configuration.binlog")
        $driverVersion = (Get-Item "artifacts/driver/$flavor/$Configuration/$driverName.sys").VersionInfo
        if ($driverVersion.FileVersion -ne $Version -or $driverVersion.ProductVersion -ne $Version) {
            throw 'Driver version resource does not match the requested package version.'
        }
        New-Item -ItemType Directory -Path "$stage/driver" | Out-Null
        Copy-Item "artifacts/driver/$flavor/$Configuration/$driverName.sys", "artifacts/driver/$flavor/$Configuration/$driverName.pdb" "$stage/driver"
    }
    Copy-Item LICENSE, THIRD_PARTY_NOTICES.md, README.md $stage
    Copy-Item LICENSES "$stage/LICENSES" -Recurse
    New-Item -ItemType Directory -Path "$stage/docs" | Out-Null
    Copy-Item docs/KNOWN_ISSUES.md, docs/LICENSING_REVIEW.md "$stage/docs"
    $commit = & git rev-parse HEAD
    if ($LASTEXITCODE) { throw 'Cannot determine source commit.' }
    $dirty = [bool](& git status --porcelain --untracked-files=normal)
    [ordered]@{ version = $Version; configuration = $Configuration; architecture = 'x64';
        commit = $commit; workingTreeDirty = $dirty; driverIncluded = (-not $ManagedOnly);
        driverSigned = $false; labPassThrough = ($LabPassThrough.IsPresent -and -not $LabWriteCache); storageCorrectnessValidated = $false;
        labWriteCache = $LabWriteCache.IsPresent;
        labSerialized = $LabSerialized.IsPresent;
        dotnetSdk = (& dotnet --version); wdkSdkPackage = '10.0.28000.2526';
        createdUtc = [DateTime]::UtcNow.ToString('O') } |
        ConvertTo-Json | Set-Content "$stage/build-info.json" -Encoding utf8
    Get-ChildItem $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash,
            [IO.Path]::GetRelativePath($stage, $_.FullName)
    } | Set-Content "$stage/SHA256SUMS.txt" -Encoding utf8
    $archive = "$stage.zip"
    Compress-Archive -Path "$stage/*" -DestinationPath $archive
    Write-Host "Package: $archive"
    Write-Host 'Experimental build only: no driver is signed, installed or enabled by this script.'
} finally { Pop-Location }
