# Generates LavApollo's self-signed driver signing certificate for the VHF virtual gamepad, and
# the values of the two GitHub secrets the release workflow reads:
#   VHF_SIGNING_CERT_BASE64    contents of vhf-signing.pfx.b64
#   VHF_SIGNING_CERT_PASSWORD  contents of vhf-signing.password.txt
#
# Run once, on a machine you trust. Every PC that installs LavApollo with the VHF driver trusts
# this key to sign drivers: once the secrets exist, delete the output folder or keep it offline.
# See docs/vhf-gamepad.md.
[CmdletBinding()]
param(
    [string] $OutDir = (Join-Path (Get-Location) 'vhf-signing'),
    [int] $Years = 10
)

$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject 'CN=LavApollo VHF Gamepad Driver' `
    -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -KeyExportPolicy Exportable `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -NotAfter (Get-Date).AddYears($Years)

try {
    $bytes = [byte[]]::new(24)
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $password = [Convert]::ToBase64String($bytes)

    $pfxPath = Join-Path $OutDir 'vhf-signing.pfx'
    Export-PfxCertificate -Cert $certificate -FilePath $pfxPath `
        -Password (ConvertTo-SecureString -String $password -AsPlainText -Force) | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $OutDir 'vhf-signing.pfx.b64'),
        [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($pfxPath)))
    [System.IO.File]::WriteAllText((Join-Path $OutDir 'vhf-signing.password.txt'), $password)
} finally {
    # The key lives in the PFX and the secrets, not in this user's certificate store.
    Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -Force
}

Write-Host "Certificate: $($certificate.Subject), thumbprint $($certificate.Thumbprint), valid until $($certificate.NotAfter)."
Write-Host ''
Write-Host 'Create these repository secrets (Settings > Secrets and variables > Actions):'
Write-Host "  VHF_SIGNING_CERT_BASE64   = contents of $(Join-Path $OutDir 'vhf-signing.pfx.b64')"
Write-Host "  VHF_SIGNING_CERT_PASSWORD = contents of $(Join-Path $OutDir 'vhf-signing.password.txt')"
Write-Host ''
Write-Host "Then delete $OutDir, or move it offline."
