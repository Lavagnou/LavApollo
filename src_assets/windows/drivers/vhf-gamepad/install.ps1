# Installs or removes the VHF virtual gamepad driver bundled with LavApollo.
#
# The driver is Nonary's libvirtualgamepad (MIT), released unsigned. LavApollo's CI signs its
# catalogue and root-device setup tool with LavApollo's own certificate, shipped next to them as
# driver\LavApolloVhfGamepad.cer -- the same trust model as SudoVDA's sudovda.cer. The driver DLL
# itself is never signed: the catalogue hashes it, and signing it would break the catalogue.
#
# Always best effort: a gamepad driver that fails to install must not fail LavApollo's installer.
# ViGEmBus keeps working either way.
[CmdletBinding()]
param(
    [switch] $Uninstall,
    [switch] $RemoveDriverStorePackage
)

$ErrorActionPreference = 'Stop'

$rebootExitCode = 3010
$ownedRootPrefix = 'ROOT\VIBESHINEVIRTUALGAMEPAD\'
$infPath = Join-Path $PSScriptRoot 'driver\VibeshineVhfGamepad.inf'
$catalogPath = Join-Path $PSScriptRoot 'driver\VibeshineVhfGamepad.cat'
$dllPath = Join-Path $PSScriptRoot 'driver\VibeshineVhfGamepad.dll'
$certificatePath = Join-Path $PSScriptRoot 'driver\LavApolloVhfGamepad.cer'
$setupToolPath = Join-Path $PSScriptRoot 'tools\VibeshineVhfGamepadDeviceSetup.exe'
$trustStores = @('Root', 'TrustedPublisher')

function Write-DriverMessage {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "[LavApollo VHF gamepad] $Message"
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script changes PnP devices and must run elevated.'
    }
}

function Get-PnpUtil {
    # Sysnative first: a 32-bit host would otherwise be redirected to SysWOW64.
    foreach ($candidate in @(
        (Join-Path $env:WINDIR 'Sysnative\pnputil.exe'),
        (Join-Path $env:WINDIR 'System32\pnputil.exe'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'pnputil.exe is unavailable.'
}

function Invoke-PnpUtil {
    param([Parameter(Mandatory = $true)][string[]] $Arguments)
    & (Get-PnpUtil) @Arguments | Out-Host
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and $exitCode -ne $rebootExitCode) {
        throw "pnputil $($Arguments[0]) failed with exit code $exitCode."
    }
    return $exitCode
}

function Get-Thumbprint {
    param([Parameter(Mandatory = $true)] $Certificate)
    return $Certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
}

function Set-CertificateTrust {
    param(
        [Parameter(Mandatory = $true)] $Certificate,
        [Parameter(Mandatory = $true)][bool] $Trusted
    )
    foreach ($name in $trustStores) {
        $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($name, 'LocalMachine')
        $store.Open('ReadWrite')
        try {
            if ($Trusted) {
                $store.Add($Certificate)
            } else {
                $found = $store.Certificates.Find('FindByThumbprint', (Get-Thumbprint -Certificate $Certificate), $false)
                foreach ($match in $found) {
                    $store.Remove($match)
                }
            }
        } finally {
            $store.Close()
        }
    }
}

function Install-DriverPackage {
    foreach ($path in @($infPath, $catalogPath, $dllPath, $certificatePath, $setupToolPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing ${path}: this build does not bundle the VHF gamepad driver."
        }
    }

    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
    $thumbprint = Get-Thumbprint -Certificate $certificate

    # Check who signed before trusting anyone: the shipped certificate must be the one that signed
    # both files, otherwise there is nothing to trust it for.
    foreach ($path in @($catalogPath, $setupToolPath)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ($null -eq $signature.SignerCertificate -or (Get-Thumbprint -Certificate $signature.SignerCertificate) -ne $thumbprint) {
            throw "$path is not signed by the bundled LavApollo certificate."
        }
    }

    Set-CertificateTrust -Certificate $certificate -Trusted $true
    Write-DriverMessage "Trusted the LavApollo driver signing certificate $thumbprint."

    foreach ($path in @($catalogPath, $setupToolPath)) {
        $status = (Get-AuthenticodeSignature -LiteralPath $path).Status
        if ($status -ne [System.Management.Automation.SignatureStatus]::Valid) {
            throw "$path signature is $status after trusting the certificate."
        }
    }

    # PnPUtil checks the catalogue against the INF and the DLL while it stages the package.
    $stageExitCode = Invoke-PnpUtil -Arguments @('/add-driver', $infPath)

    # The setup tool creates (or reloads) the root source device and completes a protocol handshake
    # with the driver, so its success is the real "installed" signal.
    & $setupToolPath install --inf $infPath | Out-Host
    $setupExitCode = $LASTEXITCODE
    if ($setupExitCode -eq 0) {
        Write-DriverMessage 'Installed and verified the VHF source device.'
        return 0
    }
    if ($setupExitCode -eq $rebootExitCode -or $stageExitCode -eq $rebootExitCode) {
        Write-DriverMessage 'Installed; Windows needs a restart before the VHF gamepad is available.'
        return $rebootExitCode
    }
    throw "Root-device setup failed with exit code $setupExitCode."
}

function Remove-OwnedRootDevices {
    # Only the exact root-enumerated source device: never match by class or VID/PID, which could
    # reach controllers owned by another application.
    $rebootRequired = $false
    $devices = @(Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop | Where-Object {
        ([string] $_.PNPDeviceID).StartsWith($ownedRootPrefix, [System.StringComparison]::OrdinalIgnoreCase)
    })
    foreach ($device in $devices) {
        Write-DriverMessage "Removing source device $($device.PNPDeviceID)."
        $rebootRequired = ((Invoke-PnpUtil -Arguments @('/remove-device', $device.PNPDeviceID)) -eq $rebootExitCode) -or $rebootRequired
    }
    return $rebootRequired
}

function Uninstall-DriverPackage {
    $rebootRequired = $false
    if (Test-Path -LiteralPath $setupToolPath -PathType Leaf) {
        & $setupToolPath remove | Out-Host
        $setupExitCode = $LASTEXITCODE
        if ($setupExitCode -ne 0 -and $setupExitCode -ne $rebootExitCode) {
            Write-DriverMessage "Setup tool removal ended with exit code $setupExitCode; removing the device directly."
        }
        $rebootRequired = $setupExitCode -eq $rebootExitCode
    }
    $rebootRequired = (Remove-OwnedRootDevices) -or $rebootRequired

    if ($RemoveDriverStorePackage) {
        $packages = @(Get-WindowsDriver -Online -All | Where-Object {
            $_.OriginalFileName -like '*\VibeshineVhfGamepad.inf' -or $_.OriginalFileName -eq 'VibeshineVhfGamepad.inf'
        })
        foreach ($package in $packages) {
            Write-DriverMessage "Removing Driver Store package $($package.Driver)."
            $rebootRequired = ((Invoke-PnpUtil -Arguments @('/delete-driver', $package.Driver, '/uninstall')) -eq $rebootExitCode) -or $rebootRequired
        }
    }

    # Withdraw the trust this script granted. The certificate is LavApollo's own, so nothing else
    # depends on it.
    if (Test-Path -LiteralPath $certificatePath -PathType Leaf) {
        $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
        Set-CertificateTrust -Certificate $certificate -Trusted $false
        Write-DriverMessage 'Removed the LavApollo driver signing certificate.'
    }

    if ($rebootRequired) {
        Write-DriverMessage 'Removed; Windows needs a restart to finish.'
        return $rebootExitCode
    }
    Write-DriverMessage 'Removed the VHF gamepad driver.'
    return 0
}

try {
    Assert-Administrator
    if ($Uninstall) {
        $null = Uninstall-DriverPackage
    } else {
        $null = Install-DriverPackage
    }
} catch {
    Write-DriverMessage "WARNING: $($_.Exception.Message)"
}
exit 0
