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
    KB packages (what wusa.exe installs): -KbNumber 9999001 names the package KB9999001 (id), writes
    KB9999001-x64.exe into -OutDir (or to -OutFile), and puts "kb", -Title and -Description into the signed
    manifest for wusa's dialog. -OldImageRoot/-NewImageRoot builds the Items from two install file trees
    (a CI image mounted or extracted: the folders that hold Windows\ and Program Files\): every file
    that is new or changed under Windows\, Program Files\ and Program Files (x86)\ is packaged if it is a
    signed PE (Authenticode status Valid or NotTrusted, i.e. a signature is present; -IncludeUnsigned lets unsigned PEs through,
    they are then covered by the manifest signature only). Kernel/HAL/bootvid/kdcom, boot/system-start drivers
    (give their Start in -DriverStarts @{ 'usbstor.sys' = 0 }) and ntdll/win32k/smss/csrss/wininit/winlogon/
    services/lsass/ci.dll get class "boot". Everything the update engine can never carry (config hives, boot\,
    the loaders, autochk, the updater's own applier, ServiceProfiles, non-PE files, unsigned files) is REFUSED
    with a list and the build stops, unless -SkipRefused packages the acceptable rest. Files deleted in the new
    tree are listed and not packaged. Dev test key only; the release key never enters the repository.
    Switches for negative fixtures: -Truncate (drop the last 100 bytes), -TamperToc (flip a byte of the table
    after hashing), -TamperPayload (flip a byte of the first payload after hashing), -BadMagic.
#>
[CmdletBinding()]
param(
    [string]$OutFile,
    [string]$OutDir,
    [Parameter(Mandatory)][string]$Stub,
    [string]$KbNumber,
    [string]$Title,
    [string]$Description,
    [string]$OldImageRoot,
    [string]$NewImageRoot,
    [hashtable]$DriverStarts = @{},
    [switch]$IncludeUnsigned,
    [switch]$SkipRefused,
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

$Kb = $null
if ($KbNumber) {
    if ($KbNumber -notmatch '^(?i:kb)?(\d{1,12})$') { throw "-KbNumber '$KbNumber' is not a KB number (digits, optional KB prefix)" }
    $Kb = 'KB' + $Matches[1]
    if (-not $PackageId) { $PackageId = $Kb }
    if (-not $OutFile) {
        if (-not $OutDir) { throw '-KbNumber needs -OutDir (or -OutFile)' }
        $OutFile = Join-Path $OutDir ($Kb + '-x64.exe')
    }
}
if (-not $OutFile) { throw 'give -OutFile, or -KbNumber with -OutDir' }

if ($OldImageRoot -or $NewImageRoot) {
    if (-not ($OldImageRoot -and $NewImageRoot)) { throw 'give both -OldImageRoot and -NewImageRoot' }
    if ($PackageDir -or $Items) { throw '-OldImageRoot/-NewImageRoot build the items themselves; do not combine with -PackageDir/-Items' }
    if (-not $PackageId) { throw 'give -KbNumber or -PackageId' }
    if (-not $Version) { $Version = '1.0' }
    $OldImageRoot = (Resolve-Path $OldImageRoot).Path.TrimEnd('\')
    $NewImageRoot = (Resolve-Path $NewImageRoot).Path.TrimEnd('\')
    $roots = 'Windows', 'Program Files', 'Program Files (x86)'
    $deniedPrefix = 'Windows\System32\config\', 'Windows\System32\boot\', 'Windows\ServiceProfiles\', 'LiberNT\'
    $deniedName = 'ntldr', 'bootmgr', 'freeldr.sys', 'freeldr.ini', 'rosload.exe', 'autochk.exe', 'liberntupdatepending.exe'
    $trio = 'ntoskrnl.exe', 'ntkrnlmp.exe', 'ntkrnlpa.exe', 'ntkrpamp.exe', 'bootvid.dll', 'kdcom.dll'
    $group3 = 'ntdll.dll', 'win32k.sys', 'win32kbase.sys', 'win32kfull.sys', 'smss.exe', 'csrss.exe', 'wininit.exe', 'winlogon.exe', 'services.exe', 'lsass.exe', 'ci.dll'
    $peExt = '.dll', '.exe', '.sys', '.ocx', '.cpl', '.scr', '.drv'
    $accepted = @()
    $refused = @()
    $removed = @()
    foreach ($r in $roots) {
        $newDir = Join-Path $NewImageRoot $r
        if (Test-Path -LiteralPath $newDir) {
            foreach ($f in Get-ChildItem -LiteralPath $newDir -Recurse -File -Force) {
                $rel = $f.FullName.Substring($NewImageRoot.Length + 1)
                $oldFile = Join-Path $OldImageRoot $rel
                if ((Test-Path -LiteralPath $oldFile) -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $oldFile).Hash -eq (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)) { continue }
                $name = $f.Name.ToLower()
                $ext = $f.Extension.ToLower()
                $why = $null
                foreach ($d in $deniedPrefix) { if ($rel.StartsWith($d, [StringComparison]::OrdinalIgnoreCase)) { $why = "under $d" } }
                if (-not $why -and ($deniedName -contains $name)) { $why = 'loader or updater-owned file' }
                if (-not $why -and ($name -like 'ntuser.dat*' -or $name -like 'usrclass.dat*' -or $name -like '*.regtrans-ms' -or $name -like '*.blf')) { $why = 'registry hive file' }
                if (-not $why -and ($peExt -notcontains $ext)) { $why = 'not a PE file (only signed binaries are packaged)' }
                if (-not $why) {
                    $sig = Get-AuthenticodeSignature -LiteralPath $f.FullName
                    if ($sig.Status -eq 'HashMismatch') { $why = 'Authenticode signature does not match the file' }
                    elseif (@('Valid', 'NotTrusted') -notcontains [string]$sig.Status -and -not $IncludeUnsigned) { $why = "not signed (Authenticode status $($sig.Status))" }
                }
                if ($why) { $refused += [pscustomobject]@{ Path = $rel; Reason = $why }; continue }
                $item = @{ Target = $rel; Source = $f.FullName }
                $isBoot = ($trio -contains $name) -or ($group3 -contains $name) -or ($name -like 'hal*.dll')
                if ($ext -eq '.sys') {
                    if ($group3 -contains $name) { $isBoot = $true }
                    elseif (-not $rel.StartsWith('Windows\System32\drivers\', [StringComparison]::OrdinalIgnoreCase)) {
                        $refused += [pscustomobject]@{ Path = $rel; Reason = '.sys outside Windows\System32\drivers' }
                        continue
                    }
                    else {
                        $start = 3
                        if ($DriverStarts.ContainsKey($f.Name)) { $start = [int]$DriverStarts[$f.Name] }
                        elseif ($DriverStarts.ContainsKey($name)) { $start = [int]$DriverStarts[$name] }
                        $item.DriverStart = $start
                        if ($start -le 1) { $isBoot = $true }
                    }
                }
                if ($isBoot) { $item.Class = 'boot' }
                $accepted += $item
            }
        }
        $oldDir = Join-Path $OldImageRoot $r
        if (Test-Path -LiteralPath $oldDir) {
            foreach ($f in Get-ChildItem -LiteralPath $oldDir -Recurse -File -Force) {
                $rel = $f.FullName.Substring($OldImageRoot.Length + 1)
                if (-not (Test-Path -LiteralPath (Join-Path $NewImageRoot $rel))) { $removed += $rel }
            }
        }
    }
    if ($removed.Count) {
        Write-Host ("Removed in the new tree, not packaged ({0}):" -f $removed.Count)
        $removed | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    }
    if ($refused.Count) {
        Write-Host ("REFUSED ({0}):" -f $refused.Count)
        $refused | ForEach-Object { Write-Host ("  {0}  <- {1}" -f $_.Path, $_.Reason) }
        if (-not $SkipRefused) { throw "$($refused.Count) changed file(s) cannot go into a KB; fix the trees or use -SkipRefused" }
    }
    if (-not $accepted.Count) { throw 'nothing to package: the two trees do not differ in packageable signed files' }
    $groups = @()
    foreach ($a in $accepted) {
        if (-not $a.ContainsKey('Class')) { continue }
        $n = (Split-Path $a.Target -Leaf).ToLower()
        if (($trio -contains $n) -or ($n -like 'hal*.dll')) { $groups += 'trio' }
        elseif ($group3 -contains $n) { $groups += 'critical' }
        else { $groups += 'drivers' }
    }
    $groups = @($groups | Sort-Object -Unique)
    if ($groups.Count -gt 1) { Write-Warning ("boot-class groups mixed in one KB ({0}); the engine applies one group per transaction, so split it" -f ($groups -join ', ')) }
    Write-Host ("Packaging {0} file(s), {1} boot-class" -f $accepted.Count, @($accepted | Where-Object { $_.ContainsKey('Class') }).Count)
    $Items = $accepted
}

$tmp = $null
if (-not $PackageDir) {
    if (-not $Version) { $Version = '1.0' }
    if (-not $PackageId -or -not $Items) { throw 'give -PackageDir, or -PackageId (or -KbNumber) with -Items or the two image roots' }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('lnpk-' + [Guid]::NewGuid().ToString('N'))
    $mk = Join-Path $here 'New-FixturePackage.ps1'
    $mkArgs = @{ OutDir = $tmp; PackageId = $PackageId; Version = $Version; Items = $Items; KeyDir = $KeyDir; Type = $Type }
    if ($Applicability) { $mkArgs.Applicability = $Applicability }
    if ($Kb) { $mkArgs.Kb = $Kb }
    if ($Title) { $mkArgs.Title = $Title }
    if ($Description) { $mkArgs.Description = $Description }
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
