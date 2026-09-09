#Requires -Version 7
[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cli = Join-Path $root "src/QueueCache.Cli/bin/$Configuration/net10.0/win-x64/qcache.exe"
if (-not (Test-Path $cli)) { $cli = Join-Path $root "src/QueueCache.Cli/bin/$Configuration/net10.0/qcache.exe" }
foreach ($arguments in @(@('--help'),@('apply','--help'),@('test','--help'),@('benchmark','--help'),@('--version'))) {
    & $cli @arguments
    if ($LASTEXITCODE) { throw "Help/version failed: $arguments" }
}
& $cli test --does-not-exist 2>&1 | Out-Host
if ($LASTEXITCODE -ne 2) { throw 'Invalid arguments must return 2 before device access.' }
& $cli apply 'Q:' 2>&1 | Out-Host
if ($LASTEXITCODE -ne 1) { throw 'Fast preset without acceptance must fail before device access.' }
Write-Host 'CLI contract checks passed. No disk handle opened.'
# GitHub's pwsh wrapper propagates the last native exit code. The negative tests
# intentionally leave it nonzero, so report this script's own successful result.
exit 0
