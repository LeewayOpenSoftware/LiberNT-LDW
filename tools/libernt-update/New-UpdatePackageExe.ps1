<#
.SYNOPSIS
    Builds a standalone LiberNT Update package .exe (LNPK, S1-PACKAGE-EXE-DESIGN.md).
.DESCRIPTION
    Appends an S1 package directory (manifest.json, manifest.sig, catalog.cat, payload\) to the launcher
    stub (base/applications/libernt-update-package) as an overlay: the entry bytes, a table of contents and
    a 64-byte trailer. Either -PackageDir names an existing, already signed package directory, or the
    package is built first with New-FixturePackage.ps1 from -PackageId/-Version/-Items and signed with the
    key in -KeyDir. The signing key must live OUTSIDE the source tree: a key under the repository is refused.
    The stub carries the package; trust comes from manifest.sig inside, verified by libservicing.
    Switches for negative fixtures: -Truncate (drop the last 100 bytes), -TamperToc (flip a byte of the table
    after hashing), -TamperPayload (flip a byte of the first payload after hashing), -BadMagic.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutFile,
    [Parameter(Mandatory)][string]$Stub,
    [string]$PackageDir,
    [string]$PackageId,
    [string]$Version,
    [hashtable[]]$Items,
    [hashtable]$Applicability,
    [string]$Type = 'system-component',
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [switch]$Unsigned,
    [switch]$Truncate,
    [switch]$TamperToc,
    [switch]$TamperPayload,
    [switch]$BadMagic
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path.TrimEnd('\')
$keyFull = (Resolve-Path $KeyDir -ErrorAction SilentlyContinue)
if ($keyFull -and $keyFull.Path.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
    throw "the signing key directory '$KeyDir' is inside the source tree; keys never enter the repository"
}
if (-not (Test-Path $Stub)) { throw "stub not found: $Stub" }

$tmp = $null
if (-not $PackageDir) {
    if (-not $PackageId -or -not $Version -or -not $Items) { throw 'give -PackageDir, or -PackageId -Version -Items' }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('lnpk-' + [Guid]::NewGuid().ToString('N'))
    $mk = Join-Path $here 'New-FixturePackage.ps1'
    $mkArgs = @{ OutDir = $tmp; PackageId = $PackageId; Version = $Version; Items = $Items; KeyDir = $KeyDir; Type = $Type }
    if ($Applicability) { $mkArgs.Applicability = $Applicability }
    if ($Unsigned) { $mkArgs.Unsigned = $true }
    & $mk @mkArgs | Out-Null
    $PackageDir = $tmp
}
$PackageDir = (Resolve-Path $PackageDir).Path.TrimEnd('\')
if (-not (Test-Path (Join-Path $PackageDir 'manifest.json'))) { throw "no manifest.json in $PackageDir" }

try {
    $sha = [Security.Cryptography.SHA256]::Create()
    $utf8 = New-Object Text.UTF8Encoding($false)
    $stubBytes = [IO.File]::ReadAllBytes($Stub)
    $ms = New-Object IO.MemoryStream
    $bw = New-Object IO.BinaryWriter($ms)
    $bw.Write($stubBytes)
    $overlayOffset = [uint64]$ms.Position

    $entries = @()
    $files = Get-ChildItem -Path $PackageDir -Recurse -File | Sort-Object FullName
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($PackageDir.Length + 1)
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        $entries += [pscustomobject]@{ Name = $rel; Offset = [uint64]$ms.Position; Size = [uint64]$bytes.Length; Sha = $sha.ComputeHash($bytes) }
        $bw.Write($bytes)
    }
    if ($TamperPayload) {
        $first = $entries | Where-Object { $_.Name -like 'payload\*' } | Select-Object -First 1
        if ($first) { $buf = $ms.GetBuffer(); $buf[[int]$first.Offset] = $buf[[int]$first.Offset] -bxor 0xff }
    }

    $tocOffset = [uint64]$ms.Position
    $toc = New-Object IO.MemoryStream
    $tw = New-Object IO.BinaryWriter($toc)
    foreach ($e in $entries) {
        $name = $utf8.GetBytes($e.Name)
        $tw.Write([uint64]$e.Offset)
        $tw.Write([uint64]$e.Size)
        $tw.Write([byte[]]$e.Sha)
        $tw.Write([uint16]$name.Length)
        $tw.Write($name)
    }
    $tw.Flush()
    $tocBytes = $toc.ToArray()
    $tocSha = $sha.ComputeHash($tocBytes)
    if ($TamperToc) { $tocBytes[3] = $tocBytes[3] -bxor 0xff }
    $bw.Write($tocBytes)

    $bw.Write([byte[]]$utf8.GetBytes($(if ($BadMagic) { 'LNPX' } else { 'LNPK' })))
    $bw.Write([uint32]1)
    $bw.Write([uint64]$overlayOffset)
    $bw.Write([uint64]$tocOffset)
    $bw.Write([uint32]$entries.Count)
    $bw.Write([uint32]0)
    $bw.Write([byte[]]$tocSha)
    $bw.Flush()
    $all = $ms.ToArray()
    if ($Truncate) { $all = $all[0..($all.Length - 101)] }
    $outDir = Split-Path -Parent $OutFile
    if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    [IO.File]::WriteAllBytes($OutFile, $all)
    Write-Host ("Package exe written: {0} ({1} entries, {2} bytes)" -f $OutFile, $entries.Count, $all.Length)
}
finally {
    if ($tmp) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}
