<#
.SYNOPSIS
    Builds (and signs) a LiberNT Update package: manifest.json, payload\, catalog.cat.

.DESCRIPTION
    The package layout is the S1 contract: manifest.json + catalog.cat + payload\. The catalog covers
    manifest.json and every payload file (CatalogVersion 2, SHA-256) and is signed with the
    DEVELOPMENT certificate from KeyDir (or with -SigningPfx for negative tests).

    Items: array of hashtables
        @{ Target = 'Windows\System32\foo.dll'; Content = 'text' }      replace/create with that content
        @{ Target = 'Windows\System32\old.dll'; Delete = $true }        delete
        @{ Target = 'Windows\System32\drivers\d.sys'; Content = 'x'; DriverStart = 3 }   driver

    Switches for negative fixtures:
        -Unsigned          no catalog.cat at all (valid for a non-driver package: the catalog is optional)
        -UnsignedCatalog   a catalog that is present but not signed (must be rejected)
        -SigningPfx/-SigningPasswordFile   sign with another certificate (the manifest then names that one)
        -TamperPayloadAfterSigning        change a payload byte after signing (payload hash no longer matches)
        -TamperManifestAfterSigning       rewrite manifest.json after signing (catalog no longer covers it)
        -NoManifestSig                    leave out manifest.sig
        -TamperManifestSigAfterSigning    flip a byte of manifest.sig after the catalog was signed

    manifest.sig is the RSA-SHA256 (PKCS#1 v1.5) signature of manifest.json made with the same key; the
    boot applier verifies it without WinVerifyTrust (S1-CONTRACTS.md plan.signature).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string]$PackageId,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][hashtable[]]$Items,
    [string]$Type = 'system-component',
    [hashtable]$Applicability,
    [string]$Kb,
    [string]$Title,
    [string]$Description,
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [string]$SigningPfx,
    [string]$SigningPasswordFile,
    [switch]$Unsigned,
    [switch]$UnsignedCatalog,
    [switch]$TamperPayloadAfterSigning,
    [switch]$TamperManifestAfterSigning,
    [switch]$NoManifestSig,
    [switch]$TamperManifestSigAfterSigning
)

$ErrorActionPreference = 'Stop'
if (-not $SigningPfx) { $SigningPfx = Join-Path $KeyDir 'Leeway-LiberNT-Update-DEV.pfx' }
if (-not $SigningPasswordFile) { $SigningPasswordFile = Join-Path $KeyDir 'pfx-password.txt' }
$utf8 = New-Object Text.UTF8Encoding($false)

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'payload') | Out-Null

$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2($SigningPfx, ([IO.File]::ReadAllText($SigningPasswordFile)))
$certSha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($cert.RawData)).Replace('-', '').ToLower()

$payloads = @()
$n = 0
foreach ($it in $Items) {
    if ($it.Delete) {
        $payloads += [ordered]@{ action = 'delete'; target = $it.Target }
        continue
    }
    $rel = ('f{0}.bin' -f $n++)
    $file = Join-Path $OutDir ('payload\' + $rel)
    if ($it.ContainsKey('Source')) { Copy-Item -LiteralPath $it.Source -Destination $file } else { [IO.File]::WriteAllText($file, [string]$it.Content, $utf8) }
    $sha = (Get-FileHash -Algorithm SHA256 -Path $file).Hash.ToLower()
    $entry = [ordered]@{ path = $rel; target = $it.Target; sha256 = $sha }
    if ($it.ContainsKey('DriverStart')) { $entry.driverStart = [int]$it.DriverStart }
    if ($it.ContainsKey('Class')) { $entry.class = [string]$it.Class }
    $payloads += $entry
}

$manifest = [ordered]@{
    schemaVersion     = 1
    packageId         = $PackageId
    releaseVersion    = $Version
    type              = $Type
    targetFamily      = 'LiberNT'
    architecture      = 'amd64'
    certificateSha256 = $certSha
    payloads          = $payloads
}
if ($Applicability) { $manifest.applicability = $Applicability }
# optional, display only (wusa): the KB number, a title and a description; covered by the manifest signature
if ($Kb) { $manifest.kb = $Kb }
if ($Title) { $manifest.title = $Title }
if ($Description) { $manifest.description = $Description }
$manifestPath = Join-Path $OutDir 'manifest.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6), $utf8)

if (-not $NoManifestSig) {
    $rsa = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
    $sigBytes = $rsa.SignData([IO.File]::ReadAllBytes($manifestPath), [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    [IO.File]::WriteAllBytes((Join-Path $OutDir 'manifest.sig'), $sigBytes)
}

if (-not $Unsigned) {
    $tmpCat = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N') + '.cat')
    New-FileCatalog -Path $OutDir -CatalogFilePath $tmpCat -CatalogVersion 2 | Out-Null
    Move-Item $tmpCat (Join-Path $OutDir 'catalog.cat')
    if (-not $UnsignedCatalog) {
        Set-AuthenticodeSignature -FilePath (Join-Path $OutDir 'catalog.cat') -Certificate $cert -HashAlgorithm SHA256 | Out-Null
    }
}

if ($TamperPayloadAfterSigning) {
    $first = Get-ChildItem (Join-Path $OutDir 'payload') | Select-Object -First 1
    [IO.File]::AppendAllText($first.FullName, 'X', $utf8)
}
if ($TamperManifestSigAfterSigning) {
    $sp = Join-Path $OutDir 'manifest.sig'
    $b = [IO.File]::ReadAllBytes($sp)
    $b[10] = $b[10] -bxor 0xff
    [IO.File]::WriteAllBytes($sp, $b)
}
if ($TamperManifestAfterSigning) {
    $text = [IO.File]::ReadAllText($manifestPath)
    [IO.File]::WriteAllText($manifestPath, $text.Replace($Version, $Version + '1'), $utf8)
}

Write-Host "Package '$PackageId' $Version written to $OutDir (certificate sha256 $certSha)"
