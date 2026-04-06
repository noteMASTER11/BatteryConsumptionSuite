$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sessionsPath = Join-Path $root 'sessions.csv'
$screenPath = Join-Path $root 'screen.csv'
$kernelStatePath = Join-Path $root 'kernel_state.txt'
$titleCachePath = Join-Path $root 'title_cache.csv'
$samplesPath = Join-Path $root 'samples.csv'
$jsonOut = Join-Path $root 'expected_stats_rows.json'
$textOut = Join-Path $root 'expected_stats_rows.txt'

function Test-TitleId([string]$Value) {
    return $Value -match '^[A-Z]{4}[0-9]{5}$'
}

function Load-TitleCache([string]$Path) {
    $map = @{}
    if (-not (Test-Path $Path)) { return $map }
    Get-Content $Path | ForEach-Object {
        if ([string]::IsNullOrWhiteSpace($_)) { return }
        $parts = $_.Split(',', 2)
        if ($parts.Count -eq 2) {
            $id = $parts[0].Trim()
            $name = $parts[1].Trim()
            if ((Test-TitleId $id) -and $name) {
                $map[$id] = $name
            }
        }
    }
    return $map
}

function Resolve-Label([string]$App, $TitleCache) {
    if (-not $App) { return '-' }
    if ((Test-TitleId $App) -and $TitleCache.ContainsKey($App)) {
        return $TitleCache[$App]
    }
    return $App
}

function Load-Sessions([string]$Path) {
    $rows = @()
    Get-Content $Path | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
        $p = $_.Split(',')
        if ($p.Count -eq 12) {
            $rows += [pscustomobject]@{
                app = $p[2].Trim()
                d_pct = [int]$p[5]
                d_mah = [int]$p[8]
                minutes = [double]$p[11]
            }
        }
    }
    return $rows
}

function Load-Screen([string]$Path) {
    $screen = [ordered]@{ consumed_mah = 0.0; consumed_pct = 0.0; minutes = 0.0; sessions = 0 }
    Get-Content $Path | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
        $p = $_.Split(',')
        if ($p.Count -eq 13 -and $p[2].Trim() -eq 'SCREEN_ON') {
            $screen.sessions += 1
            $screen.minutes += [double]$p[11]
            $dmah = [int]$p[8]
            $dpct = [int]$p[5]
            if ($dmah -gt 0) { $screen.consumed_mah += $dmah }
            if ($dpct -gt 0) { $screen.consumed_pct += $dpct }
        }
    }
    return [pscustomobject]$screen
}

function Load-KernelState([string]$Path) {
    $state = [ordered]@{ active = 0; app = ''; start_tick = 0; start_mah = 0; start_pct = 0; start_mv = 0 }
    if (-not (Test-Path $Path)) { return [pscustomobject]$state }
    Get-Content $Path | ForEach-Object {
        if ($_ -match '=') {
            $k, $v = $_.Split('=', 2)
            $k = $k.Trim(); $v = $v.Trim()
            switch ($k) {
                'active' { $state.active = [int]$v }
                'app' { $state.app = $v }
                'start_tick' { $state.start_tick = [uint64]$v }
                'start_mah' { $state.start_mah = [int]$v }
                'start_pct' { $state.start_pct = [int]$v }
                'start_mv' { $state.start_mv = [int]$v }
            }
        }
    }
    return [pscustomobject]$state
}

function Load-Samples([string]$Path) {
    $rows = @()
    if (-not (Test-Path $Path)) { return $rows }
    Get-Content $Path | ForEach-Object {
        if ([string]::IsNullOrWhiteSpace($_) -or $_.StartsWith('#')) { return }
        $p = $_.Split(',')
        if ($p.Count -eq 8) {
            $rows += [pscustomobject]@{
                tick = [uint64]$p[0]
                percent = [int]$p[1]
                remain_mah = [int]$p[2]
                full_mah = [int]$p[3]
                mv = [int]$p[4]
                temp_centi = [int]$p[5]
                charging = [int]$p[6]
                online = [int]$p[7]
            }
        }
    }
    return $rows
}

$titleCache = Load-TitleCache $titleCachePath
$sessions = Load-Sessions $sessionsPath
$screen = Load-Screen $screenPath
$active = Load-KernelState $kernelStatePath
$samples = Load-Samples $samplesPath
$snapshot = if ($samples.Count -gt 0) { $samples[-1] } else { [pscustomobject]@{ remain_mah = 0 } }

$aggs = @{}
foreach ($row in $sessions) {
    $app = $row.app.Trim()
    if (-not $app) { continue }
    if (-not $aggs.ContainsKey($app)) {
        $aggs[$app] = [ordered]@{
            app = $app
            label = Resolve-Label $app $titleCache
            consumed_mah = 0.0
            consumed_pct = 0.0
            minutes = 0.0
            sessions = 0
        }
    }
    $aggs[$app].sessions += 1
    $aggs[$app].minutes += $row.minutes
    if ($row.d_mah -gt 0) { $aggs[$app].consumed_mah += $row.d_mah }
    if ($row.d_pct -gt 0) { $aggs[$app].consumed_pct += $row.d_pct }
}

$ranked = @($aggs.Values | ForEach-Object { [pscustomobject]$_ })
$ranked += [pscustomobject]@{
    app = 'System'
    label = 'System'
    consumed_mah = [double]$screen.consumed_mah
    consumed_pct = [double]$screen.consumed_pct
    minutes = [double]$screen.minutes
    sessions = [int]$screen.sessions
}
$ranked = @($ranked | Sort-Object @{Expression='consumed_mah';Descending=$true}, @{Expression='app';Descending=$false})

$rows = @()
for ($i = 0; $i -lt [Math]::Min(7, $ranked.Count); $i++) {
    $r = $ranked[$i]
    $curMah = 0.0
    if ($active.active -eq 1 -and $snapshot.remain_mah -gt 0 -and $active.start_mah -gt 0 -and $r.app -eq $active.app) {
        $dmah = $active.start_mah - $snapshot.remain_mah
        if ($dmah -gt 0) { $curMah = [double]$dmah }
    } elseif (($r.app -eq 'System' -or $r.app -eq 'SYSTEM') -and $samples.Count -ge 2) {
        $dmahSys = $samples[-2].remain_mah - $samples[-1].remain_mah
        if ($dmahSys -gt 0) { $curMah = [double]$dmahSys }
    }

    $rate = 0.0
    if ([double]$r.minutes -gt 0.01) {
        $rate = [Math]::Round(([double]$r.consumed_mah * 60.0 / [double]$r.minutes), 1)
    }

    $rows += [pscustomobject]@{
        rank = $i + 1
        app = $r.app
        label = $r.label
        cur_mAh = [Math]::Round($curMah, 1)
        total_mAh = [Math]::Round([double]$r.consumed_mah, 1)
        up_min = [Math]::Round([double]$r.minutes, 1)
        mAh_per_h = $rate
        sessions = [int]$r.sessions
    }
}

$rows | ConvertTo-Json -Depth 4 | Set-Content $jsonOut

$table = @()
$table += 'Rank | App | Cur mAh | Total mAh | Up min | mAh/h | Sessions'
$table += '--- | --- | ---: | ---: | ---: | ---: | ---:'
foreach ($r in $rows) {
    $table += ('{0} | {1} | {2:N1} | {3:N1} | {4:N1} | {5:N1} | {6}' -f $r.rank, $r.label, $r.cur_mAh, $r.total_mAh, $r.up_min, $r.mAh_per_h, $r.sessions)
}
$table += ''
$table += ('Snapshot remain_mah: {0}' -f $snapshot.remain_mah)
$table += ('Active app: {0}' -f $active.app)
$table | Set-Content $textOut

Get-Content $textOut
