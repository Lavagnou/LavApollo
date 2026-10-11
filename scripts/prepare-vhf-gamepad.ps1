# Downloads the pinned libvirtualgamepad driver release, checks it, and signs it with LavApollo's
# driver certificate, producing the package that CMake bundles (LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR).
#
# Signed: the catalogue (.cat) and the root-device setup tool. Not signed: the driver DLL, which
# the catalogue hashes. Output layout:
#   driver\VibeshineVhfGamepad.{inf,dll,cat}  driver\LavApolloVhfGamepad.cer
#   tools\VibeshineVhfGamepadDeviceSetup.exe
#
# See docs/vhf-gamepad.md. The certificate comes from scripts/generate-vhf-signing-cert.ps1.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $OutDir,
    [Parameter(Mandatory = $true)][string] $PfxPath,
    [string] $PfxPassword = $env:VHF_SIGNING_CERT_PASSWORD,
    [string] $TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'

# Pinned release. The client protocol compiled into LavApollo (third-party/libvirtualgamepad) is
# the source commit of this release: move both together.
$releaseUrl = 'https://github.com/Nonary/libvirtualgamepad/releases/download/v0.1.0-beta.6/libvirtualgamepad-0.1.0-beta.6-windows-x64.zip'
$releaseSha256 = 'a45a8ae27d2764ad26a4b89d43d1e2dc43510d84bddd5899aac7c80499754782'
$expectedFiles = @(
    'manifest.json',
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)

function Find-SignTool {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } |
        Sort-Object { $_.Directory.Parent.Name } |
        Select-Object -Last 1
    if ($null -eq $found) {
        throw 'signtool.exe was not found (Windows SDK).'
    }
    return $found.FullName
}

function Invoke-Sign {
    param([string] $SignTool, [string] $Path)
    $common = @('sign', '/fd', 'SHA256', '/f', $PfxPath, '/p', $PfxPassword)
    & $SignTool @common '/tr' $TimestampUrl '/td' 'SHA256' $Path
    if ($LASTEXITCODE -eq 0) {
        return
    }
    # A timestamp server outage must not block a release: the signature stays valid until the
    # certificate expires instead of forever.
    Write-Warning "Timestamped signing of $Path failed; signing without a timestamp."
    & $SignTool @common $Path
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed on $Path."
    }
}

if ([string]::IsNullOrEmpty($PfxPassword)) {
    throw 'No PFX password: pass -PfxPassword or set VHF_SIGNING_CERT_PASSWORD.'
}

$archive = Join-Path ([System.IO.Path]::GetTempPath()) "libvirtualgamepad-$([guid]::NewGuid()).zip"
try {
    Write-Host "Downloading $releaseUrl"
    Invoke-WebRequest -Uri $releaseUrl -OutFile $archive -UseBasicParsing
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $releaseSha256) {
        throw "SHA-256 mismatch for the libvirtualgamepad release: expected $releaseSha256, got $actual."
    }

    if (Test-Path -LiteralPath $OutDir) {
        Remove-Item -LiteralPath $OutDir -Recurse -Force
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $OutDir
} finally {
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
}

$root = (Resolve-Path -LiteralPath $OutDir).Path
$actualFiles = @(Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
    $_.FullName.Substring($root.Length + 1).Replace('\', '/')
} | Sort-Object)
if (Compare-Object -ReferenceObject ($expectedFiles | Sort-Object) -DifferenceObject $actualFiles) {
    throw "Unexpected libvirtualgamepad archive layout: $($actualFiles -join ', ')"
}

$catalog = Join-Path $root 'driver\VibeshineVhfGamepad.cat'
$setupTool = Join-Path $root 'tools\VibeshineVhfGamepadDeviceSetup.exe'
$signTool = Find-SignTool
Invoke-Sign -SignTool $signTool -Path $catalog
Invoke-Sign -SignTool $signTool -Path $setupTool

$pfx = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($PfxPath, $PfxPassword)
$certificatePath = Join-Path $root 'driver\LavApolloVhfGamepad.cer'
[System.IO.File]::WriteAllBytes($certificatePath, $pfx.Export('Cert'))

# install.ps1 refuses a certificate that did not sign both files: catch that here instead.
foreach ($path in @($catalog, $setupTool)) {
    $signer = (Get-AuthenticodeSignature -LiteralPath $path).SignerCertificate
    if ($null -eq $signer -or $signer.Thumbprint -ne $pfx.Thumbprint) {
        throw "$path is not signed by $($pfx.Subject)."
    }
}
Write-Host "Prepared the VHF gamepad driver in $root, signed by $($pfx.Subject) ($($pfx.Thumbprint))."
