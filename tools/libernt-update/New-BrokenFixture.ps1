<#
.SYNOPSIS
    Builds a correctly signed but non-working boot-class package for the fallback lab legs (LAB-S1-BOOTCLASS-JOB.md).
.DESCRIPTION
    The failure legs (D2/D3, G2, G3) need a package that verifies against the pinned certificate yet does not
    work on the target, so that only the fallback boot recovers. This makes one with no judgment call: a tiny
    fixture that fails by ordinary means stands in for the component, signed with the DEVELOPMENT key.

      -Kind win32   csrss / wininit / winlogon / services / lsass: a console .exe (stubs\lnfailstub_win32.c)
                    that logs to COM1 and returns 1. Compiled here with clang; no build tree needed.
      -Kind driver  a boot-start / system-start driver or win32k: a .sys (stubs\lnfailstub_driver.c) whose
                    DriverEntry returns a failure status. A .sys cannot be produced by bare clang (it needs the
                    generated DDK); build it from the LiberNT tree with tools\libernt-update\stubs\CMakeLists.txt
                    (target lnfailstub) and pass it as -StubBinary.
      -Kind native  smss: a native .exe (stubs\lnfailstub_native.c) that terminates itself with a failure
                    status; build target lnfailstub_smss the same way and pass it as -StubBinary.

    The fixture is NOT a corruption of the real file and never crashes by fault injection: it is a signed
    component that cleanly declines to do its job. Trust still comes from the manifest signature; the point is
    that a valid signature plus a non-working file is exactly what the fallback and rollback must handle.

    The result is a directory package (via New-FixturePackage.ps1, class "boot"); add -AsExe to also wrap it as
    a package .exe (via New-UpdatePackageExe.ps1, needs -Stub).
.EXAMPLE
    # winlogon that exits non-zero (G3)
    New-BrokenFixture.ps1 -OutDir out\kb-winlogon -Target 'Windows\System32\winlogon.exe' -Kind win32 `
        -PackageId ln.badwinlogon -Version 1.0
.EXAMPLE
    # a boot-start driver that fails DriverEntry (D2); .sys built from the tree first
    New-BrokenFixture.ps1 -OutDir out\kb-drv -Target 'Windows\System32\drivers\lndrv.sys' -Kind driver `
        -DriverStart 0 -StubBinary build\lnfailstub.sys -PackageId ln.baddrv -Version 1.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string]$Target,                 # relative to the target root, e.g. Windows\System32\winlogon.exe
    [Parameter(Mandatory)][ValidateSet('win32', 'driver', 'native')][string]$Kind,
    [Parameter(Mandatory)][string]$PackageId,
    [Parameter(Mandatory)][string]$Version,
    [int]$DriverStart = -1,                                 # required for a .sys (0..4)
    [string]$StubBinary,                                   # pre-built .sys / native .exe for -Kind driver|native
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [string]$Clang = (Join-Path $env:LOCALAPPDATA 'RosBE\pkgs\20261015\llvm-mingw\bin\clang.exe'),
    [switch]$AsExe,
    [string]$Stub                                          # the package-stub exe, required with -AsExe
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stubs = Join-Path $here 'stubs'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('lnbroken-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    if ($Kind -eq 'win32') {
        $bin = Join-Path $tmp 'payload.bin'
        $src = Join-Path $stubs 'lnfailstub_win32.c'
        $cargs = @('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-Wextra', '-mconsole', $src, '-o', $bin, '-lkernel32')
        & $Clang @cargs
        if ($LASTEXITCODE -ne 0) { throw "clang failed to build the Win32 stub" }
    }
    else {
        if (-not $StubBinary -or -not (Test-Path $StubBinary)) {
            throw "-Kind $Kind needs -StubBinary <pre-built file>; build it from tools\libernt-update\stubs\CMakeLists.txt (target $(if ($Kind -eq 'driver') { 'lnfailstub' } else { 'lnfailstub_smss' }))"
        }
        $bin = $StubBinary
    }
    if ($Kind -eq 'driver' -and ($DriverStart -lt 0 -or $DriverStart -gt 4)) {
        throw "-Kind driver needs -DriverStart 0..4 (the Start the signed manifest declares)"
    }

    $item = @{ Target = $Target; Content = [IO.File]::ReadAllBytes($bin); Class = 'boot' }
    if ($Kind -eq 'driver') { $item.DriverStart = $DriverStart }

    $mk = Join-Path $here 'New-FixturePackage.ps1'
    & $mk -OutDir $OutDir -PackageId $PackageId -Version $Version -KeyDir $KeyDir -Items @($item) `
          -Type $(if ($Kind -eq 'driver') { 'driver' } else { 'system-component' })

    if ($AsExe) {
        if (-not $Stub) { throw "-AsExe needs -Stub <libernt-update-package.exe stub>" }
        $mkexe = Join-Path $here 'New-UpdatePackageExe.ps1'
        & $mkexe -OutFile ($OutDir.TrimEnd('\') + '.exe') -Stub $Stub -PackageDir $OutDir
    }
    Write-Host ("Broken-but-signed fixture written: {0} ({1}, target {2})" -f $OutDir, $Kind, $Target)
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
