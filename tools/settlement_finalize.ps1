# settlement_finalize.ps1
#
# Merge the client's normal end-of-run writes back into its own warehouse save.
# The live run is allowed to write its save normally.  At the end we retain the
# baseline file's plot/progression tables and take only the durable settlement
# state from the run file: the baseline cats that still exist, equipment bags,
# and the house/adventure food and gold counters.
#
# The script is intentionally external to the injected DLL.  SQLite keeps the
# save open while the game is in the House, so the watcher retries until the
# game has closed (or until -Once is used after closing it).  This avoids
# replacing an open database and also gives us an atomic whole-file promotion.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Baseline,
    [Parameter(Mandatory)][string]$Target,
    [string]$ClientLogDir = '',
    [switch]$Watch,
    [switch]$Once
)

$ErrorActionPreference = 'Stop'

if (-not $ClientLogDir) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    $ClientLogDir = Join-Path $scriptRoot '..\..\mgmp\client'
}

function Find-Sqlite {
    $cmd = Get-Command sqlite3.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:CONDA_PREFIX 'Library\bin\sqlite3.exe'),
        (Join-Path $env:CONDA_PREFIX 'Library\sqlite3.exe'),
        'C:\Program Files\SQLite\sqlite3.exe'
    )
    foreach ($p in $candidates) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }
    throw 'sqlite3.exe not found. Install SQLite CLI or add it to PATH.'
}

function Assert-NoSidecars([string]$path) {
    foreach ($suffix in @('-wal','-shm','-journal')) {
        if (Test-Path -LiteralPath ($path + $suffix)) { return $false }
    }
    return $true
}

function Invoke-Merge {
    param([string]$sqlite)

    if (-not (Test-Path -LiteralPath $Baseline)) { throw "baseline not found: $Baseline" }
    if (-not (Test-Path -LiteralPath $Target)) { throw "target not found: $Target" }
    if (-not (Assert-NoSidecars $Target)) { return $false }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = "$Target.settlement-before-$stamp.bak"
    $temp = "$Target.settlement-$PID.tmp"
    $sql = "$Target.settlement-$PID.sql"

    Copy-Item -LiteralPath $Target -Destination $backup -Force
    Copy-Item -LiteralPath $Baseline -Destination $temp -Force

    # SQLite single-quote escaping is sufficient for Windows paths here.
    $runPath = $Target.Replace("'", "''")
    @"
PRAGMA busy_timeout=5000;
ATTACH DATABASE '$runPath' AS run;
BEGIN IMMEDIATE;

-- Preserve the warehouse roster identity.  Only rows that already belonged to
-- the client's baseline may be updated; host/session-only cat IDs are ignored.
INSERT OR REPLACE INTO main.cats(key,data)
  SELECT r.key,r.data FROM run.cats AS r
  WHERE EXISTS (SELECT 1 FROM main.cats AS b WHERE b.key=r.key);

-- These are the three buckets the game itself persists.  They contain the
-- equipment after its ordinary random durability/decay has been applied.
DELETE FROM main.files
  WHERE key IN ('inventory_backpack','inventory_storage','inventory_trash');
INSERT OR REPLACE INTO main.files(key,data)
  SELECT key,data FROM run.files
  WHERE key IN ('inventory_backpack','inventory_storage','inventory_trash');

-- House counters are the player-facing settlement.  Adventure counters are
-- included because the game writes them immediately before returning home;
-- all map/chapter/event properties remain from the baseline.
UPDATE main.properties
   SET data=(SELECT r.data FROM run.properties AS r WHERE r.key=main.properties.key)
 WHERE key IN ('house_food','house_gold','adventure_food','adventure_coins',
               'adventure_furniture_boxes')
   AND EXISTS (SELECT 1 FROM run.properties AS r WHERE r.key=main.properties.key);

COMMIT;
DETACH DATABASE run;
PRAGMA wal_checkpoint(TRUNCATE);
"@ | Set-Content -LiteralPath $sql -Encoding ascii

    try {
        & $sqlite $temp ".read '$sql'" 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "sqlite3 returned $LASTEXITCODE" }
        Move-Item -LiteralPath $temp -Destination $Target -Force
        Remove-Item -LiteralPath $sql -Force -ErrorAction SilentlyContinue
        Write-Host "settlement merged: $Target (backup: $backup)"
        return $true
    } catch {
        Remove-Item -LiteralPath $temp,$sql -Force -ErrorAction SilentlyContinue
        Write-Warning "settlement merge deferred: $($_.Exception.Message)"
        return $false
    }
}

$sqlite = Find-Sqlite
if (-not $Watch -and -not $Once) { $Once = $true }

if ($Watch) {
    Write-Host 'settlement watcher armed; waiting for the client to report the host leaving the run'
    $seen = $false
    while (-not $seen) {
        $log = Get-ChildItem -LiteralPath $ClientLogDir -Filter 'mgmp_client_*.log' -File -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime | Select-Object -Last 1
        if ($log) {
            $seen = [bool](Select-String -LiteralPath $log.FullName -Pattern 'the host has left the run|in the House \(the warehouse\)' -Quiet)
        }
        if (-not $seen) { Start-Sleep -Seconds 2 }
    }
    # Give the final save/scene transition a chance to flush before the first
    # attempt.  Later attempts cover a still-open SQLite handle.
    Start-Sleep -Seconds 3
}

if ($Once) {
    while ($true) {
        # Never rename a live game's database.  The process can still flush a
        # second time after the House transition, so the watcher waits for both
        # peers to exit before promoting the merged temporary file.
        if (Get-Process -Name Mewgenics -ErrorAction SilentlyContinue) {
            if (-not $Watch) { break }
            Start-Sleep -Seconds 3
            continue
        }
        if (Invoke-Merge $sqlite) { break }
        if (-not $Watch) { break }
        Start-Sleep -Seconds 3
    }
}
