#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$UnsignedPackageDirectory, [switch]$AllowWriteCache)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = (Resolve-Path -LiteralPath $UnsignedPackageDirectory).Path
$metadata = Get-Content "$source/build-info.json" -Raw | ConvertFrom-Json
if ((-not $metadata.labPassThrough -and -not ($metadata.labWriteCache -and $AllowWriteCache)) -or $metadata.driverSigned -or $metadata.configuration -ne 'Release') {
    throw 'Only unsigned Release lab packages may be signed; write-cache builds require explicit -AllowWriteCache.'
}
if ((Test-Path "$source/driver/qcache.sys") -or -not (Test-Path "$source/driver/qcachelab.sys")) { throw 'Unexpected driver contents.' }
$stage = Join-Path $root "artifacts/lab/QueueCache-$($metadata.version)-test-signed-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item "$source/*" $stage -Recurse
Copy-Item "$root/lab" "$stage/lab" -Recurse
# Private key remains non-exportable in this user's Windows certificate store.
# Never generate production trust or place a PFX in a package/repository.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=QueueCache disposable lab $([guid]::NewGuid().ToString('N'))" `
    -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddMonths(6)
Export-Certificate -Cert $cert -FilePath "$stage/QueueCacheLab.cer" | Out-Null
$signtool = "$root/.packages/Microsoft.Windows.SDK.CPP.10.0.28000.2526/c/bin/10.0.28000.0/x64/signtool.exe"
& $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint "$stage/driver/qcachelab.sys"
if ($LASTEXITCODE) { throw "SignTool failed: $LASTEXITCODE" }
$sig = Get-AuthenticodeSignature "$stage/driver/qcachelab.sys"
if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint -or $sig.Status -eq 'HashMismatch') {
    throw 'Signed binary verification failed.'
}
$metadata.driverSigned = $true
$metadata | Add-Member -NotePropertyName testCertificateThumbprint -NotePropertyValue $cert.Thumbprint
$metadata | Add-Member -NotePropertyName signingPurpose -NotePropertyValue 'Disposable lab only; not production-trusted'
$metadata | ConvertTo-Json | Set-Content "$stage/build-info.json" -Encoding utf8
Get-ChildItem $stage -File -Recurse | Where-Object Name -ne SHA256SUMS.txt | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, [IO.Path]::GetRelativePath($stage, $_.FullName)
} | Set-Content "$stage/SHA256SUMS.txt" -Encoding utf8
Compress-Archive -Path "$stage/*" -DestinationPath "$stage.zip"
Write-Host "Signed lab package: $stage.zip"
Write-Host "Public certificate thumbprint: $($cert.Thumbprint). No trust stores changed; private key stays in CurrentUser/My."
