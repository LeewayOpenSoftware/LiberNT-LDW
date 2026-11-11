<#
.SYNOPSIS
    Host build and unit tests of the LiberNT Update stage 1 core (libservicing + libernt-update.exe).

.DESCRIPTION
    Standalone: it does not use the LiberNT build. It compiles the sources with llvm-mingw clang (the
    RosBE package) for x86_64 Windows, builds signed fixture packages with the DEVELOPMENT certificate,
    and runs the tests against a temporary Image target. Compilation and tests are reported separately.

    Needs: the development certificate (New-DevSigningCert.ps1), Windows PowerShell 5.1 (New-FileCatalog,
    Set-AuthenticodeSignature) and the RosBE llvm-mingw toolchain.
#>
[CmdletBinding()]
param(
    [string]$Clang = (Join-Path $env:LOCALAPPDATA 'RosBE\pkgs\20261015\llvm-mingw\bin\clang.exe'),
    [string]$OutDir = (Join-Path $env:TEMP 'libernt-update-host'),
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [switch]$SkipFixtures
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$lib = Join-Path $repo 'base\system\servicing\libservicing'
$cli = Join-Path $repo 'base\applications\cmdutils\libernt-update'
$cer = Join-Path $here 'certs\Leeway-LiberNT-Update-DEV.cer'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path (Join-Path $KeyDir 'Leeway-LiberNT-Update-DEV.pfx')) -or -not (Test-Path $cer)) {
    & (Join-Path $here 'New-DevSigningCert.ps1') -KeyDir $KeyDir
}

# ---- 1. COMPILATION ---------------------------------------------------------
Write-Host '=== COMPILATION ==='
$srcs = 'sha256','util','json','target','manifest','verify','journal','store','pending','servicing','fetch','package' | ForEach-Object { Join-Path $lib "$_.c" }
$srcs += (Join-Path $repo 'base\system\servicing\pending\s1core.c')    # the portable core shared with the boot applier
$common = @('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare', '-municode')
$libs = @('-lwintrust', '-lcrypt32', '-ladvapi32', '-lws2_32')
$compileOk = $true

function Invoke-Cc([string[]]$cargs, [string]$what) {
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $out = & $Clang @cargs 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { Write-Host "FAILED: $what"; $out | ForEach-Object { Write-Host "  $_" }; return $false }
    $warn = @($out | Where-Object { "$_" -match 'warning:' })
    Write-Host ("ok: {0} ({1} warnings)" -f $what, $warn.Count)
    $warn | ForEach-Object { Write-Host "  $_" }
    return $true
}

$exe = Join-Path $OutDir 'libernt-update.exe'
$tst = Join-Path $OutDir 'libservicing-tests.exe'
if (-not (Invoke-Cc ($common + $srcs + @((Join-Path $cli 'main.c'), '-o', $exe) + $libs) 'libernt-update.exe')) { $compileOk = $false }
if (-not (Invoke-Cc ($common + @('-DLN_TESTING') + $srcs + @((Join-Path $lib 'tests\tests.c'), '-o', $tst) + $libs + @('-lole32')) 'libservicing-tests.exe (with the test hooks)')) { $compileOk = $false }
$stub = Join-Path $OutDir 'libernt-update-package.exe'
if (-not (Invoke-Cc (@('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-Wextra', '-municode', '-mwindows', (Join-Path $repo 'base\applications\libernt-update-package\main.c'), '-o', $stub, '-lshell32', '-luser32', '-ladvapi32')) 'libernt-update-package.exe (package stub)')) { $compileOk = $false }
# wusa.exe (production flags), its test build (LN_TESTING: LNWUSA_UPDATER / LNWUSA_IMAGE hooks), the KB stub's test build
# (LN_TESTING: LNSTUB_WUSA), a fake wusa and the unit tests of the argument parser and the container view
$wd = Join-Path $repo 'base\applications\wusa'
$exeT = Join-Path $OutDir 'libernt-update-test.exe'
if (-not (Invoke-Cc ($common + @('-DLN_TESTING') + $srcs + @((Join-Path $cli 'main.c'), '-o', $exeT) + $libs) 'libernt-update-test.exe (CLI with the test hooks)')) { $compileOk = $false }
$wsrc = @((Join-Path $wd 'wusa.c'), (Join-Path $wd 'args.c'), (Join-Path $wd 'lnpkview.c'))
$wusa = Join-Path $OutDir 'wusa.exe'
$wusaT = Join-Path $OutDir 'wusa-test.exe'
$stubT = Join-Path $OutDir 'libernt-update-package-test.exe'
$fake = Join-Path $OutDir 'fakewusa.exe'
$wtests = Join-Path $OutDir 'wusa-tests.exe'
$gui = @('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-municode', '-mwindows')
if (-not (Invoke-Cc ($gui + $wsrc + @('-o', $wusa, '-lcomctl32', '-lshell32', '-luser32', '-ladvapi32')) 'wusa.exe')) { $compileOk = $false }
if (-not (Invoke-Cc ($gui + @('-DLN_TESTING') + $wsrc + @('-o', $wusaT, '-lcomctl32', '-lshell32', '-luser32', '-ladvapi32')) 'wusa-test.exe (with the test hooks)')) { $compileOk = $false }
if (-not (Invoke-Cc ($gui + @('-DLN_TESTING', (Join-Path $repo 'base\applications\libernt-update-package\main.c'), '-o', $stubT, '-lshell32', '-luser32')) 'libernt-update-package-test.exe')) { $compileOk = $false }
if (-not (Invoke-Cc (@('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-municode', (Join-Path $wd 'tests\fakewusa.c'), '-o', $fake)) 'fakewusa.exe')) { $compileOk = $false }
if (-not (Invoke-Cc (@('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-municode', (Join-Path $wd 'tests\wusa-tests.c'), (Join-Path $wd 'args.c'), (Join-Path $wd 'lnpkview.c'), '-o', $wtests)) 'wusa-tests.exe')) { $compileOk = $false }
Write-Host ("COMPILATION: " + $(if ($compileOk) { 'OK' } else { 'FAILED' }))
if (-not $compileOk) { exit 1 }

# ---- 2. FIXTURES --------------------------------------------------------------
$fx = Join-Path $OutDir 'fixtures'
if (-not $SkipFixtures) {
    Write-Host '=== FIXTURES ==='
    $mk = Join-Path $here 'New-FixturePackage.ps1'
    $good = @(@{ Target = 'Windows\System32\lnfixture.dll'; Content = 'version one' })
    & $mk -OutDir "$fx\good1" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir | Out-Null
    # standalone package .exe fixtures (LNPK): good, damaged in each way, and the applicability pair
    $mkexe = Join-Path $here 'New-UpdatePackageExe.ps1'
    & $mkexe -OutFile "$fx\kb-good.exe" -Stub $stub -PackageDir "$fx\good1" | Out-Null
    & $mkexe -KbNumber 9999001 -OutDir $fx -Stub $stub -Version 1.0 -KeyDir $KeyDir -Title 'Fixture update for LiberNT' -Description 'Test update that replaces lnfixture.dll.' -Items $good | Out-Null
    & $mkexe -OutFile "$fx\kb-truncated.exe" -Stub $stub -PackageDir "$fx\good1" -Truncate | Out-Null
    & $mkexe -OutFile "$fx\kb-toc-tamper.exe" -Stub $stub -PackageDir "$fx\good1" -TamperToc | Out-Null
    & $mkexe -OutFile "$fx\kb-payload-tamper.exe" -Stub $stub -PackageDir "$fx\good1" -TamperPayload | Out-Null
    & $mkexe -OutFile "$fx\kb-badmagic.exe" -Stub $stub -PackageDir "$fx\good1" -BadMagic | Out-Null
    & $mkexe -OutFile "$fx\kb-requires.exe" -Stub $stub -PackageId ln.kbreq -Version 1.0 -KeyDir $KeyDir -Items @(@{ Target = 'Windows\System32\lnkbreq.dll'; Content = 'needs the fixture' }) -Applicability @{ requires = @('ln.fixture>=1.0') } | Out-Null
    & $mkexe -OutFile "$fx\kb-supersedes.exe" -Stub $stub -PackageId ln.kbsup -Version 2.0 -KeyDir $KeyDir -Items @(@{ Target = 'Windows\System32\lnkbsup.dll'; Content = 'newer' }) -Applicability @{ supersedes = @('ln.fixture') } | Out-Null
    & $mk -OutDir "$fx\tampered-payload" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -TamperPayloadAfterSigning | Out-Null
    & $mk -OutDir "$fx\tampered-manifest" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -TamperManifestAfterSigning | Out-Null
    & $mk -OutDir "$fx\tampered-sig" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -TamperManifestSigAfterSigning | Out-Null
    & $mk -OutDir "$fx\no-manifest-sig" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -NoManifestSig | Out-Null
    & $mk -OutDir "$fx\junction" -PackageId ln.junction -Version 1.0 -KeyDir $KeyDir -Items @(@{ Target = 'Windows\System32\jn\evil.dll'; Content = 'evil' }) | Out-Null
    & $mk -OutDir "$fx\driver-cat" -PackageId ln.driver -Version 1.0 -KeyDir $KeyDir -Items @(@{ Target = 'Windows\System32\drivers\lndrv.sys'; Content = 'driver'; DriverStart = 3 }) | Out-Null
    & $mk -OutDir "$fx\driver-nocat" -PackageId ln.driver -Version 1.0 -KeyDir $KeyDir -Unsigned -Items @(@{ Target = 'Windows\System32\drivers\lndrv.sys'; Content = 'driver'; DriverStart = 3 }) | Out-Null
    & $mk -OutDir "$fx\unsigned" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -Unsigned | Out-Null
    & $mk -OutDir "$fx\unsigned-catalog" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -UnsignedCatalog | Out-Null
    & $mk -OutDir "$fx\kernel" -PackageId ln.kernel -Version 1.0 -Items @(@{ Target = 'Windows\System32\ntoskrnl.exe'; Content = 'no' }) -KeyDir $KeyDir | Out-Null
    & $mk -OutDir "$fx\twoitems" -PackageId ln.twoitems -Version 1.0 -KeyDir $KeyDir -Items @(
        @{ Target = 'Windows\System32\first.dll'; Content = 'first new' },
        @{ Target = 'Windows\System32\blocked\second.dll'; Content = 'second' }) | Out-Null
    & $mk -OutDir "$fx\missing-payload" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir | Out-Null
    Remove-Item -Force (Join-Path $fx 'missing-payload\payload\f0.bin')

    # a valid signature by a certificate that is NOT the pinned one (throwaway, never stored)
    $tmpKey = Join-Path $env:TEMP ('lnwrong-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmpKey | Out-Null
    try {
        $c = New-SelfSignedCertificate -Subject 'CN=Not The LiberNT Dev Cert' -Type CodeSigningCert -CertStoreLocation 'Cert:\CurrentUser\My'
        $pw = 'throwaway'
        Export-PfxCertificate -Cert $c -FilePath "$tmpKey\w.pfx" -Password (ConvertTo-SecureString $pw -AsPlainText -Force) | Out-Null
        [IO.File]::WriteAllText("$tmpKey\pw.txt", $pw)
        Remove-Item -Path ("Cert:\CurrentUser\My\" + $c.Thumbprint) -DeleteKey -ErrorAction SilentlyContinue
        & $mk -OutDir "$fx\wrongcert" -PackageId ln.fixture -Version 1.0 -Items $good -SigningPfx "$tmpKey\w.pfx" -SigningPasswordFile "$tmpKey\pw.txt" | Out-Null
    }
    finally { Remove-Item -Recurse -Force $tmpKey -ErrorAction SilentlyContinue }
}

# ---- 3. TESTS -------------------------------------------------------------------
Write-Host '=== TESTS ==='
$httpRoot = Join-Path $OutDir 'httproot'
if (-not $SkipFixtures) {
    Remove-Item -Recurse -Force $httpRoot -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $httpRoot | Out-Null
    [IO.File]::WriteAllText((Join-Path $httpRoot 'blob.bin'), ('A' * 1000))
    [IO.File]::WriteAllText((Join-Path $httpRoot 'big.bin'), ('0123456789' * 1000))
    foreach ($pkg in 'good1', 'tampered-payload', 'tampered-manifest', 'no-manifest-sig', 'wrongcert') {
        Copy-Item -Recurse -Force (Join-Path $fx $pkg) (Join-Path $httpRoot $pkg)
    }
    # review F1: a manifest naming a ~5000-char payload path must fail cleanly, not overflow the URL buffer
    $hp = Join-Path $httpRoot 'hugepath'
    New-Item -ItemType Directory -Force -Path $hp | Out-Null
    $hpCert = New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $KeyDir 'Leeway-LiberNT-Update-DEV.pfx'), [IO.File]::ReadAllText((Join-Path $KeyDir 'pfx-password.txt')))
    $hpSha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($hpCert.RawData)).Replace('-', '').ToLower()
    $hpMan = [ordered]@{
        schemaVersion     = 1
        packageId         = 'ln.hugepath'
        releaseVersion    = '1.0'
        type              = 'system-component'
        targetFamily      = 'LiberNT'
        architecture      = 'amd64'
        certificateSha256 = $hpSha
        payloads          = @(@{ path = (('a/' * 2500) + 'f0.bin'); target = 'Windows\System32\x.dll'; sha256 = ('0' * 64) })
    }
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $hp 'manifest.json'), ($hpMan | ConvertTo-Json -Depth 6), $utf8)
    $hpRsa = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($hpCert)
    [IO.File]::WriteAllBytes((Join-Path $hp 'manifest.sig'), $hpRsa.SignData([IO.File]::ReadAllBytes((Join-Path $hp 'manifest.json')), [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1))
}
$server = $null
try {
    if (-not $SkipFixtures) {
        $server = Start-Job -ScriptBlock { param($d) python -m http.server 8097 --bind 127.0.0.1 --directory $d } -ArgumentList $httpRoot
        $up = $false
        for ($i = 0; $i -lt 40; $i++) {
            try {
                $c = New-Object Net.Sockets.TcpClient
                $iar = $c.BeginConnect('127.0.0.1', 8097, $null, $null)
                if ($iar.AsyncWaitHandle.WaitOne(500) -and $c.Connected) { $up = $true }
                $c.Close()
                if ($up) { break }
            } catch { }
            Start-Sleep -Milliseconds 250
        }
        if ($up) { $env:LN_FETCH_TEST_URL = 'http://127.0.0.1:8097' }
        else { Write-Host 'WARN: fetch test server did not come up; fetch tests will skip' }
    }
    & $tst $fx $cer
    $rc = $LASTEXITCODE
} finally {
    if (Test-Path Env:LN_FETCH_TEST_URL) { Remove-Item Env:LN_FETCH_TEST_URL }
    if ($server) { Stop-Job $server -ErrorAction SilentlyContinue; Remove-Job $server -ErrorAction SilentlyContinue }
}
Write-Host ("TESTS: " + $(if ($rc -eq 0) { 'PASSED' } else { 'FAILED (exit ' + $rc + ')' }))

# ---- 4. WUSA ----------------------------------------------------------------------
Write-Host '=== WUSA ==='
$wrc = 0
$kb = Join-Path $fx 'KB9999001-x64.exe'
if ($SkipFixtures) { Write-Host '  skipped (-SkipFixtures)' } else {
    & $wtests $kb
    if ($LASTEXITCODE -ne 0) { $wrc = 1 }
    $w = Join-Path $OutDir ('wusa-work-' + $PID)      # a fresh tree per run: the Image targets of earlier runs have a protected DACL
    New-Item -ItemType Directory -Force -Path $w | Out-Null
    function Wcheck([string]$what, $cond) {
        if ($cond) { Write-Host "  ok: $what" } else { Write-Host "  FAIL: $what"; $script:wrc = 1 }
    }
    function Run-Gui([string]$exe, [string[]]$a) {
        $pr = Start-Process -FilePath $exe -ArgumentList $a -PassThru -Wait -WindowStyle Hidden
        return $pr.ExitCode
    }
    # (a) stub -> wusa hand-off: the fake wusa sees `"<own path>" <the switches>` and its exit code comes back
    $env:LNSTUB_WUSA = $fake
    $env:LNFAKE_OUT = Join-Path $w 'fake.out'
    foreach ($case in @(@{ Exit = 0; Sw = @('/quiet', '/norestart') }, @{ Exit = 3010; Sw = @('/quiet') }, @{ Exit = 87; Sw = @('/uninstall', '/quiet') })) {
        $env:LNFAKE_EXIT = [string]$case.Exit
        Remove-Item -Force $env:LNFAKE_OUT -ErrorAction SilentlyContinue
        $code = Run-Gui $stubT $case.Sw
        $got = if (Test-Path $env:LNFAKE_OUT) { ((Get-Content -Raw $env:LNFAKE_OUT) -replace '[^ -~]', '') } else { '' }
        $want = ('[' + $stubT + ']' + (($case.Sw | ForEach-Object { "[$_]" }) -join ''))
        Wcheck ("stub hand-off {0} -> exit {1}" -f ($case.Sw -join ' '), $case.Exit) (($code -eq $case.Exit) -and ($got -ieq $want))
    }
    $env:LNSTUB_WUSA = Join-Path $w 'missing-wusa.exe'
    Wcheck 'stub: wusa.exe missing -> exit 1' ((Run-Gui $stubT @('/quiet')) -eq 1)
    Remove-Item Env:LNSTUB_WUSA, Env:LNFAKE_OUT, Env:LNFAKE_EXIT -ErrorAction SilentlyContinue

    # (b) the whole chain in Image mode: KB9999001-x64.exe (stub) -> wusa -> libernt-update -> libservicing
    $root = Join-Path $w ('img-' + $PID)
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'Windows\System32') | Out-Null
    $dll = Join-Path $root 'Windows\System32\lnfixture.dll'
    [IO.File]::WriteAllText($dll, 'original')
    $kbRun = Join-Path $w 'KB9999001-x64.exe'                    # the fixture KB is built with the production stub
    $kbT = Join-Path $w 'KB9999001-test.exe'                     # and a copy built on the test stub, so LNSTUB_WUSA works
    & $mkexe -KbNumber 9999001 -OutFile $kbT -Stub $stubT -Version 1.0 -KeyDir $KeyDir -Title 'Fixture update for LiberNT' -Description 'Test update that replaces lnfixture.dll.' -Items $good | Out-Null
    Copy-Item $kb $kbRun
    $env:LNWUSA_UPDATER = $exeT
    $env:LNWUSA_IMAGE = $root
    $env:LNSTUB_WUSA = $wusaT
    $env:LNUPDATE_SYSTEM_DIRS = ''
    & $exeT --image $root init --trust-cer $cer | Out-Null
    Wcheck 'image initialised' ($LASTEXITCODE -eq 0)
    Wcheck 'KB install through stub + wusa -> exit 0' ((Run-Gui $kbT @('/quiet', '/norestart')) -eq 0)
    Wcheck 'the file was replaced' ((Get-Content -Raw $dll) -eq 'version one')
    Wcheck 'KB uninstall through stub + wusa -> exit 0' ((Run-Gui $kbT @('/uninstall', '/quiet')) -eq 0)
    Wcheck 'the file is back' ((Get-Content -Raw $dll) -eq 'original')
    Wcheck 'reinstall -> 0' ((Run-Gui $kbT @('/quiet', '/norestart')) -eq 0)
    Wcheck 'wusa /uninstall /kb:9999001 -> 0' ((Run-Gui $wusaT @('/uninstall', '/kb:9999001', '/quiet')) -eq 0)
    Wcheck 'the file is back (by KB number)' ((Get-Content -Raw $dll) -eq 'original')
    Wcheck 'wusa /uninstall /kb:123 (not installed) -> ERROR_NOT_FOUND' ((Run-Gui $wusaT @('/uninstall', '/kb:123', '/quiet')) -eq 1168)
    $bad = Join-Path $fx 'kb-truncated.exe'
    if (Test-Path $bad) {
        Wcheck 'damaged KB refused, file untouched' (((Run-Gui $wusaT @($bad, '/quiet')) -ne 0) -and ((Get-Content -Raw $dll) -eq 'original'))
    }
    Wcheck 'wusa on a non-package -> 11' ((Run-Gui $wusaT @($cer, '/quiet')) -eq 11)
    Wcheck 'wusa with bad arguments -> 87' ((Run-Gui $wusaT @('/bogus', '/quiet')) -eq 87)
    $xd = Join-Path $w 'extract'
    Wcheck 'wusa /extract writes the container files' (((Run-Gui $wusaT @($kb, "/extract:$xd")) -eq 0) -and (Test-Path (Join-Path $xd 'manifest.json')) -and (Test-Path (Join-Path $xd 'manifest.sig')))
    # (c) a KB built from two install trees: changed PE files only, boot-class routed, the rest refused with a list
    $old = Join-Path $w 'tree-old'; $new = Join-Path $w 'tree-new'
    foreach ($r in $old, $new) {
        New-Item -ItemType Directory -Force -Path "$r\Windows\System32\drivers", "$r\Windows\System32\config", "$r\Program Files\App" | Out-Null
        foreach ($f in 'Windows\System32\same.dll', 'Windows\System32\a.dll', 'Windows\System32\hal.dll', 'Windows\System32\drivers\usbstor.sys', 'Windows\System32\drivers\lan.sys', 'Program Files\App\app.exe') {
            [IO.File]::WriteAllText("$r\$f", 'old')
        }
    }
    foreach ($f in 'Windows\System32\a.dll', 'Windows\System32\hal.dll', 'Windows\System32\drivers\usbstor.sys', 'Windows\System32\drivers\lan.sys', 'Program Files\App\app.exe') {
        [IO.File]::WriteAllText("$new\$f", 'new')
    }
    $diffOut = Join-Path $w 'diff'
    $bad = $false
    try { & $mkexe -KbNumber 9999002 -OutDir $diffOut -Stub $stub -KeyDir $KeyDir -OldImageRoot $old -NewImageRoot $new | Out-Null } catch { $bad = $true }
    Wcheck 'image diff: unsigned changed files are refused by default' ($bad -and -not (Test-Path (Join-Path $diffOut 'KB9999002-x64.exe')))
    [IO.File]::WriteAllText("$new\Windows\System32\config\SYSTEM", 'hive')
    [IO.File]::WriteAllText("$new\Windows\System32\notes.ini", 'ini')
    $bad = $false
    try { & $mkexe -KbNumber 9999002 -OutDir $diffOut -Stub $stub -KeyDir $KeyDir -OldImageRoot $old -NewImageRoot $new -IncludeUnsigned -DriverStarts @{ 'usbstor.sys' = 0 } | Out-Null } catch { $bad = $true }
    Wcheck 'image diff: hives and non-PE files are refused even with -IncludeUnsigned' ($bad -and -not (Test-Path (Join-Path $diffOut 'KB9999002-x64.exe')))
    & $mkexe -KbNumber 9999002 -OutDir $diffOut -Stub $stub -KeyDir $KeyDir -OldImageRoot $old -NewImageRoot $new -IncludeUnsigned -SkipRefused -DriverStarts @{ 'usbstor.sys' = 0 } -Title 'Diff update' | Out-Null
    $kb2 = Join-Path $diffOut 'KB9999002-x64.exe'
    Wcheck 'image diff: KB9999002-x64.exe written with -SkipRefused' (Test-Path $kb2)
    if (Test-Path $kb2) {
        $xd2 = Join-Path $w 'extract2'
        Run-Gui $wusaT @($kb2, "/extract:$xd2") | Out-Null
        $m = Get-Content -Raw (Join-Path $xd2 'manifest.json') | ConvertFrom-Json
        $targets = @($m.payloads | ForEach-Object { $_.target })
        Wcheck 'image diff: manifest carries exactly the five changed packageable files' ($targets.Count -eq 5 -and ($targets -notcontains 'Windows\System32\same.dll'))
        Wcheck 'image diff: hal.dll and usbstor.sys (Start 0) are class boot, a.dll and lan.sys are not' (
            (@($m.payloads | Where-Object { $_.class -eq 'boot' } | ForEach-Object { $_.target } | Sort-Object) -join '|') -eq 'Windows\System32\drivers\usbstor.sys|Windows\System32\hal.dll')
        Wcheck 'image diff: lan.sys defaults to demand start 3, usbstor.sys keeps 0' ((@($m.payloads | Where-Object { $_.target -like '*lan.sys' })[0].driverStart -eq 3) -and (@($m.payloads | Where-Object { $_.target -like '*usbstor.sys' })[0].driverStart -eq 0))
        Wcheck 'image diff: kb and title in the signed manifest' (($m.kb -eq 'KB9999002') -and ($m.title -eq 'Diff update') -and ($m.packageId -eq 'KB9999002'))
    }
    Remove-Item Env:LNWUSA_UPDATER, Env:LNWUSA_IMAGE, Env:LNSTUB_WUSA, Env:LNUPDATE_SYSTEM_DIRS -ErrorAction SilentlyContinue
}
Write-Host ("WUSA: " + $(if ($wrc -eq 0) { 'PASSED' } else { 'FAILED' }))
if ($wrc -ne 0) { $rc = 1 }
exit $rc
