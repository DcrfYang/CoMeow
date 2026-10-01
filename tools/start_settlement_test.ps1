[CmdletBinding()]
param(
    [string]$Root,
    [string]$SaveDir = (Join-Path (@(Get-ChildItem (Join-Path $env:APPDATA 'Glaiel Games\Mewgenics') -Directory -ErrorAction SilentlyContinue)[0].FullName) 'saves'),
    [switch]$ValidateOnly,
    [switch]$RestoreOnly,
    [switch]$AllowDllMismatch
)
$ErrorActionPreference = 'Stop'

# WHY $Root IS NOT A param() DEFAULT ANY MORE (2026-09-27, the reported error):
#   [string]$Root = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
# threw "无法将参数绑定到参数"Path"，因为该参数为空字符串" at line 3, because
# ***$PSScriptRoot IS NOT POPULATED WHILE param() DEFAULTS ARE EVALUATED*** in Windows
# PowerShell 5.1 (it is filled in when the script BODY starts). $PSCommandPath and
# $MyInvocation.MyCommand.Path are empty at that moment too. So the root is resolved here,
# in the body, with fallbacks -- and with an actionable error instead of a binding failure.
if (-not $Root) {
    $here = $PSScriptRoot
    if (-not $here -and $PSCommandPath) { $here = Split-Path -Parent $PSCommandPath }
    if (-not $here -and $MyInvocation.MyCommand.Path) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }
    if (-not $here) {
        throw 'Cannot resolve this script''s own path -- run it as a file, e.g. ' +
              'powershell -NoProfile -ExecutionPolicy Bypass -File <this.ps1>'
    }
    $Root = Split-Path -Parent (Split-Path -Parent $here)
}
# Say what was resolved, and fail with the PATH in the message if it is not the workspace:
# a wrong $Root used to surface as "manifest not found", which does not name the cause.
if (-not (Test-Path -LiteralPath (Join-Path $Root 'mgmp'))) {
    throw "Resolved root does not look like the workspace: $Root"
}
Write-Host "Workspace root: $Root"
$baseline = Join-Path $Root '存档备份\结算测试基准档'
$manifest = Get-Content -LiteralPath (Join-Path $baseline 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.version -ne 2 -or -not $manifest.paired -or @($manifest.saves).Count -ne 2) {
    throw 'Expected the paired slot1/slot2 settlement benchmark manifest.'
}
function Assert-NoSidecars([string]$file) {
    foreach ($suffix in @('-wal','-shm','-journal')) {
        if (Test-Path -LiteralPath ($file + $suffix)) { throw "Database sidecar exists: $file$suffix" }
    }
}
$entries = @($manifest.saves | Sort-Object slot)
for ($i = 0; $i -lt 2; ++$i) {
    $entry = $entries[$i]
    $expected = 'steamcampaign0' + ($i + 2) + '.sav'
    $role = if ($i -eq 0) { 'host' } else { 'client' }
    if ($entry.slot -ne ($i + 1) -or $entry.file -ne $expected -or $entry.role -ne $role) {
        throw 'Unexpected slot mapping in benchmark manifest.'
    }
    $source = Join-Path $baseline $entry.file
    Assert-NoSidecars $source
    if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256 -or
        (Get-Item -LiteralPath $source).Length -ne $entry.size) { throw "Benchmark hash mismatch: $source" }
    if ([IO.Path]::GetFullPath($SaveDir).TrimEnd('\') -eq [IO.Path]::GetFullPath($baseline).TrimEnd('\')) {
        throw 'The benchmark directory cannot be used as the live save directory.'
    }
}
$server = Join-Path $Root 'mgmp\server\mgmp_server.exe'
$hostLoader = Join-Path $Root 'mgmp\host\mgmp_loader.exe'
$clientLoader = Join-Path $Root 'mgmp\client\mgmp_loader.exe'
foreach ($file in @($server,$hostLoader,$clientLoader)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing executable: $file" }
}
# --- the DLL the test will actually run, checked against the benchmark's own record ---
#
# A paired benchmark is only reproducible together with the build it was taken on, which is why
# the manifest stores dll_sha256. It was NOT being checked: two peers can agree with each other
# (the old check) and both be a build the benchmark was never paired with. Measured 2026-09-27:
# the manifest names 9B0808FB... while the deployed pair was DED57240..., so this is a live case,
# not a hypothetical one. A mismatch is fatal unless -AllowDllMismatch says it is deliberate.
$dllHost   = Join-Path $Root 'mgmp\host\mgmp.dll'
$dllClient = Join-Path $Root 'mgmp\client\mgmp.dll'
$dllBuild  = Join-Path $Root 'build_manual\mgmp.dll'
$dllHostHash   = (Get-FileHash -LiteralPath $dllHost   -Algorithm SHA256).Hash
$dllClientHash = (Get-FileHash -LiteralPath $dllClient -Algorithm SHA256).Hash
if ($dllHostHash -ne $dllClientHash) { throw 'Host/client DLL mismatch.' }
if (Test-Path -LiteralPath $dllBuild) {
    $dllBuildHash = (Get-FileHash -LiteralPath $dllBuild -Algorithm SHA256).Hash
    if ($dllBuildHash -ne $dllHostHash) {
        Write-Warning ("build_manual\mgmp.dll is NOT the deployed build.`n  build_manual : " +
                       $dllBuildHash + "`n  deployed     : " + $dllHostHash)
    }
}
if ($manifest.dll_sha256 -and $dllHostHash -ne $manifest.dll_sha256) {
    $dllMsg = "The deployed DLL does not match the one this benchmark was paired with.`n" +
              "  manifest : $($manifest.dll_sha256)`n" +
              "  deployed : $dllHostHash"
    if ($AllowDllMismatch) {
        Write-Warning ($dllMsg + "`n  -AllowDllMismatch given -- continuing with the newer DLL.")
    } else {
        throw ($dllMsg + "`n  Deploy that build, or re-run with -AllowDllMismatch to test this one deliberately.")
    }
} else {
    Write-Host ("DLL: matches the benchmark manifest (" + $dllHostHash.Substring(0,16) + "...)")
}
if ($ValidateOnly) {
    Write-Host 'Paired benchmark and executables verified. Host = slot1; client = slot2.'
    foreach ($entry in $entries) {
        Write-Host ("  slot{0} ({1})  {2}  sha={3}..." -f $entry.slot, $entry.role, $entry.file,
                    $entry.sha256.Substring(0,16))
    }
    return
}

$games = @(Get-Process Mewgenics -ErrorAction SilentlyContinue)
foreach ($game in $games) { [void]$game.CloseMainWindow() }
foreach ($game in $games) {
    if (-not $game.WaitForExit(30000)) { throw 'Game did not exit; close it and retry.' }
}
if (Get-Process Mewgenics,mgmp_loader -ErrorAction SilentlyContinue) { throw 'Game or loader still running.' }
foreach ($entry in $entries) { Assert-NoSidecars (Join-Path $SaveDir $entry.file) }
if (-not $RestoreOnly) {
    $listeners = @(Get-NetTCPConnection -LocalPort 27700 -State Listen -ErrorAction SilentlyContinue)
    foreach ($listener in $listeners) {
        if ((Get-Process -Id $listener.OwningProcess).Path -ne $server) { throw 'Port 27700 belongs to a different program.' }
    }
}
$backup = Join-Path $Root ('存档备份\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-before-paired-restore')
New-Item -ItemType Directory -Path $backup | Out-Null
# PRINTED BEFORE ANYTHING IS TOUCHED. A half-finished run must leave the operator knowing where
# to restore from, which is exactly what a previous script of this shape failed to do: it threw
# while overwriting the SECOND target, so the first one was already changed and the backup
# directory had only been mentioned in the last line it never reached.
Write-Host "Backup directory: $backup"
# Back up BOTH targets before changing either. A failed restore rolls back what was backed up.
# A target that does not exist yet is "nothing to back up", NOT an error: the copy below creates
# it. (The previous version called Copy-Item on the missing file and aborted the whole script.)
$backedUp = @()
foreach ($entry in $entries) {
    $live = Join-Path $SaveDir $entry.file
    if (Test-Path -LiteralPath $live) {
        Copy-Item -LiteralPath $live -Destination (Join-Path $backup $entry.file) -Force
        $backedUp += $entry.file
        Write-Host ("  backed up " + $entry.file)
    } else {
        Write-Host ("  " + $entry.file + " is not present -- nothing to back up; it will be created")
    }
}
try {
    foreach ($entry in $entries) {
        $target = Join-Path $SaveDir $entry.file
        Copy-Item -LiteralPath (Join-Path $baseline $entry.file) -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target).Hash -ne $entry.sha256) { throw "Restore mismatch: $target" }
    }
} catch {
    # Only roll back what was actually saved, otherwise the rollback throws too and hides the
    # original failure.
    foreach ($name in $backedUp) {
        Copy-Item -LiteralPath (Join-Path $backup $name) -Destination (Join-Path $SaveDir $name) -Force
    }
    Write-Host ("Restore failed -- rolled back: " + ($(if ($backedUp.Count) { $backedUp -join ', ' } else { '(nothing had been backed up)' })))
    throw
}
Write-Host "Restored slot1 + slot2 as one pair. Previous saves: $backup"
if ($RestoreOnly) { return }
if (-not (Get-NetTCPConnection -LocalPort 27700 -State Listen -ErrorAction SilentlyContinue)) {
    $proc = Start-Process -FilePath $server -ArgumentList '--port','27700' -WorkingDirectory (Split-Path $server) -WindowStyle Hidden -PassThru
    $ready = $false
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) { throw 'Server exited before accepting connections.' }
        if (Get-NetTCPConnection -LocalPort 27700 -State Listen -ErrorAction SilentlyContinue | Where-Object OwningProcess -eq $proc.Id) { $ready = $true; break }
    }
    if (-not $ready) { throw 'Server did not listen on port 27700 within 10 seconds.' }
}
Start-Process -FilePath $hostLoader -WorkingDirectory (Split-Path $hostLoader) -WindowStyle Hidden
Start-Sleep -Seconds 15
Start-Process -FilePath $clientLoader -WorkingDirectory (Split-Path $clientLoader) -WindowStyle Hidden
Write-Host 'Server and both clients ready. Select slot1 on HOST, slot2 on CLIENT.'
