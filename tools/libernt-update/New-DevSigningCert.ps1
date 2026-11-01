<#
.SYNOPSIS
    Creates the internal DEVELOPMENT signing certificate for LiberNT Update (stage 1).

.DESCRIPTION
    - The private key is written ONLY to a PFX inside KeyDir (default %USERPROFILE%\.leeway-dev-signing),
      outside the repository and outside every test VM. The script refuses a KeyDir inside the repository.
    - Only the public certificate (.cer, DER) is exported for the bundle (PublicOut).
    - The certificate is removed from the Windows certificate store after export, so the key lives in one place.
    - Idempotent: if the PFX already exists nothing is regenerated; the public certificate is re-exported.
    - RSA-2048: manifest.sig is verified by the portable core of the boot applier, which is RSA-2048 only.
    - This is NOT a distribution key. Do not generate or publish a production key without owner authorization.
#>
[CmdletBinding()]
param(
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [string]$PublicOut,
    [string]$Subject = 'CN=Leeway LiberNT Update DEV'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PublicOut) { $PublicOut = Join-Path $here 'certs\Leeway-LiberNT-Update-DEV.cer' }
$repoRoot = (Resolve-Path (Join-Path $here '..\..')).Path
$keyFull = [IO.Path]::GetFullPath($KeyDir)
if ($keyFull.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "KeyDir '$keyFull' is inside the repository '$repoRoot'. The private key must stay outside."
}

$pfx = Join-Path $keyFull 'Leeway-LiberNT-Update-DEV.pfx'
$pwFile = Join-Path $keyFull 'pfx-password.txt'
New-Item -ItemType Directory -Force -Path $keyFull | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $PublicOut) | Out-Null

if (-not (Test-Path $pfx)) {
    # random password, kept next to the key (the directory is private to this user)
    $bytes = New-Object byte[] 24
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $password = [Convert]::ToBase64String($bytes)
    [IO.File]::WriteAllText($pwFile, $password)

    $cert = New-SelfSignedCertificate -Subject $Subject -Type CodeSigningCert -KeyAlgorithm RSA -KeyLength 2048 `
        -HashAlgorithm SHA256 -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(5) `
        -CertStoreLocation 'Cert:\CurrentUser\My'
    $sec = ConvertTo-SecureString -String $password -AsPlainText -Force
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $sec | Out-Null
    Export-Certificate -Cert $cert -FilePath $PublicOut -Type CERT | Out-Null
    Remove-Item -Path ("Cert:\CurrentUser\My\" + $cert.Thumbprint) -DeleteKey -ErrorAction SilentlyContinue

    # restrict the directory to the current user only
    & icacls $keyFull /inheritance:r /grant:r "$($env:USERNAME):(OI)(CI)F" | Out-Null
    Write-Host "Created development certificate: $($cert.Subject)"
}
else {
    $password = [IO.File]::ReadAllText($pwFile)
    $c = New-Object Security.Cryptography.X509Certificates.X509Certificate2($pfx, $password)
    New-Item -ItemType Directory -Force -Path (Split-Path $PublicOut) | Out-Null
    [IO.File]::WriteAllBytes($PublicOut, $c.RawData)
    Write-Host "PFX already exists; re-exported the public certificate."
}

$pub = New-Object Security.Cryptography.X509Certificates.X509Certificate2($PublicOut)
$sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($pub.RawData)).Replace('-', '').ToLower()
Write-Host "Public certificate : $PublicOut"
Write-Host "SHA-256 (DER)      : $sha"
Write-Host "Private key        : $pfx (outside the repository; never copy it into a VM or a bundle)"
