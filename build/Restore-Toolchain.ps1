#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root '.tools'
New-Item -ItemType Directory -Path $tools -Force | Out-Null
$nuget = Join-Path $tools 'nuget-6.14.0.exe'
if (-not (Test-Path $nuget)) {
    Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/v6.14.0/nuget.exe' -OutFile $nuget
}
& $nuget restore (Join-Path $PSScriptRoot 'packages.config') -PackagesDirectory (Join-Path $root '.packages') -ConfigFile (Join-Path $root 'NuGet.Config') -NonInteractive
if ($LASTEXITCODE) { throw "Toolchain restore failed: $LASTEXITCODE" }
