#Requires -Version 7
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SignedPackageDirectory, [string]$Compiler)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$package = (Resolve-Path -LiteralPath $SignedPackageDirectory).Path
$metadata = Get-Content -LiteralPath "$package/build-info.json" -Raw | ConvertFrom-Json
if (-not $metadata.driverSigned -or -not $metadata.labWriteCache) { throw 'Installer requires an explicitly test-signed write-cache package.' }
if (-not $Compiler) { $Compiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path -LiteralPath $Compiler)) { throw 'Install Inno Setup 6 or specify -Compiler.' }
& $Compiler "/DPackageDir=$package" "/DBuildVersion=$($metadata.version)" "$root/packaging/QueueCache.iss"
if ($LASTEXITCODE) { throw "Installer compiler failed: $LASTEXITCODE" }
