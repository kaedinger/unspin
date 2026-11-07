<?php
/* Unspin - shared config/share-parsing helpers
 * Required by both Unspin.php (page render) and exec.php (POST handler).
 */

function load_cfg($path) {
    $cfg = [];
    if (!file_exists($path)) return $cfg;
    foreach (file($path, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
        $line = trim($line);
        if ($line === '' || $line[0] === '#') continue;
        if (strpos($line, '=') !== false) {
            [$k, $v] = explode('=', $line, 2);
            $cfg[trim($k)] = trim($v, " \t\"'");
        }
    }
    return $cfg;
}

function fmt_bytes($n) {
    $u = ['B','KB','MB','GB','TB','PB'];
    $i = 0;
    $v = (float)$n;
    while ($v >= 1024 && $i < count($u) - 1) { $v /= 1024; $i++; }
    return ($v >= 100 ? round($v) : round($v, 1)) . ' ' . $u[$i];
}

// Promotion statistics from the daemon's stats file (one "date\tpool\tfiles\tbytes"
// line per day and pool with promotions) - the current copy in RAM, or the one on
// the flash (written hourly) after a reboot: a one-line summary (plain text) shown
// while the section is folded, the header text shown while it's open, and a
// per-pool table (HTML) with an "All pools" sum row when there's more than one
// pool. Averages count every day since the first promotion on any pool, days
// without promotions included, so the per-pool averages add up to the sum.
function promotion_stats() {
    $today = date('Y-m-d');
    $since = null;
    $pools = [];   // pool => [today files, today bytes, total files, total bytes]
    $file  = file_exists('/var/run/unspind.stats') ? '/var/run/unspind.stats' : '/boot/config/plugins/unspin/stats';
    foreach (@file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        $f = explode("\t", $line);
        if (count($f) !== 4) continue;
        [$date, $pool, $files, $bytes] = $f;
        if ($since === null || $date < $since) $since = $date;
        $p = $pools[$pool] ?? [0, 0, 0, 0];
        if ($date === $today) { $p[0] += (int)$files; $p[1] += (float)$bytes; }
        $p[2] += (int)$files;
        $p[3] += (float)$bytes;
        $pools[$pool] = $p;
    }
    if (!$pools) return ['summary' => 'No promotions yet', 'open' => 'No promotions yet', 'detail' => ''];
    ksort($pools);

    $sum = [0, 0, 0, 0];
    foreach ($pools as $p) foreach ($p as $i => $v) $sum[$i] += $v;

    $days  = max(1, (int)round((strtotime($today) - strtotime($since)) / 86400) + 1);
    $cell  = fn($files, $bytes, $dec = 0) => number_format($files, $dec) . ' (' . fmt_bytes($bytes) . ')';
    $cells = function ($p) use ($cell, $days) {
        $avg = $p[2] / $days;
        return [$cell($p[0], $p[1]), $cell($avg, $p[3] / $days, $avg < 10 ? 1 : 0), $cell($p[2], $p[3])];
    };
    $row = function ($name, $p, $cls = '') use ($cells) {
        $html = "<span class=\"$cls\">" . htmlspecialchars($name) . '</span>';
        foreach ($cells($p) as $c) $html .= "<span class=\"$cls\">$c</span>";
        return $html;
    };

    [$t, $a, $s] = $cells($sum);
    $detail = '<div class="hf-stats-grid"><span></span><span>Today</span><span>Avg/day</span><span>Total</span>';
    foreach ($pools as $name => $p) $detail .= $row($name, $p);
    if (count($pools) > 1) $detail .= $row('All pools', $sum, 'hf-stats-sum');
    $detail .= '</div>';
    return ['summary' => "Today $t · Avg $a · Total $s",
            'open'    => 'Per pool, averaged over ' . number_format($days) . ($days == 1 ? ' day' : ' days'),
            'detail'  => $detail];
}

// Disk wake-ups from the daemon's wakeups file (RAM copy, or the flash copy after
// a reboot): "C date disk label count" lines per day, disk and cause, and
// "W epoch disk label name access path also" lines for the recent wake-ups.
// Returns the same shape as promotion_stats(): a summary line, the header text
// while open, and the detail HTML (per disk and cause, then the latest ones).
function disk_wakeups() {
    $today  = date('Y-m-d');
    $since  = null;
    $counts = [];   // disk => label => [today, total]
    $recent = [];
    $file   = file_exists('/var/run/unspind.wakeups') ? '/var/run/unspind.wakeups' : '/boot/config/plugins/unspin/wakeups';
    foreach (@file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        $f = explode("\t", $line);
        if ($f[0] === 'C' && count($f) === 5) {
            [, $date, $disk, $label, $n] = $f;
            if ($since === null || $date < $since) $since = $date;
            $c = $counts[$disk][$label] ?? [0, 0];
            if ($date === $today) $c[0] += (int)$n;
            $c[1] += (int)$n;
            $counts[$disk][$label] = $c;
        } elseif ($f[0] === 'W' && count($f) === 8) {
            $recent[] = ['at' => (int)$f[1], 'disk' => $f[2], 'label' => $f[3], 'name' => $f[4],
                         'access' => $f[5], 'path' => $f[6], 'also' => $f[7]];
        }
    }
    if (!$counts) return ['summary' => 'No wake-ups recorded yet', 'open' => 'No wake-ups recorded yet', 'detail' => ''];

    $days = max(1, (int)round((strtotime($today) - strtotime($since)) / 86400) + 1);
    $avg  = fn($n) => number_format($n / $days, $n / $days < 10 ? 1 : 0);
    $sum  = [0, 0];
    foreach ($counts as $labels) foreach ($labels as $c) { $sum[0] += $c[0]; $sum[1] += $c[1]; }

    $e = fn($s) => htmlspecialchars($s);
    $detail = '<div class="hf-stats-grid hf-wake-grid"><span></span><span>Today</span><span>Avg/day</span><span>Total</span>';
    uksort($counts, 'strnatcmp');
    foreach ($counts as $disk => $labels) {
        $dt = [array_sum(array_column($labels, 0)), array_sum(array_column($labels, 1))];
        $detail .= '<span class="hf-stats-sum">' . $e($disk) . '</span><span class="hf-stats-sum">' . $dt[0] .
                   '</span><span class="hf-stats-sum">' . $avg($dt[1]) . '</span><span class="hf-stats-sum">' . $dt[1] . '</span>';
        uasort($labels, fn($a, $b) => $b[1] <=> $a[1]);
        foreach ($labels as $label => $c)
            $detail .= '<span class="hf-wake-label" title="' . $e($label) . '">' . $e($label) . '</span><span>' . $c[0] .
                       '</span><span>' . $avg($c[1]) . '</span><span>' . $c[1] . '</span>';
    }
    $detail .= '</div>';

    if ($recent) {
        $detail .= '<div class="hf-wake-recent"><div class="hf-wake-head">Latest wake-ups</div>';
        foreach (array_slice(array_reverse($recent), 0, 20) as $w) {
            $when = date(date('Y-m-d', $w['at']) === $today ? 'H:i' : 'M j H:i', $w['at']);
            $what = $w['path'] !== '' ? $w['access'] . ' ' . basename($w['path']) : '';
            $tip  = trim($w['path'] . ($w['also'] !== '' ? "\nAlso active: " . $w['also'] : ''));
            $detail .= '<div title="' . $e($tip) . '"><span class="hf-wake-when">' . $e($when) . '</span> ' .
                       $e($w['disk']) . ' &middot; ' . $e($w['label']) . ($what !== '' ? ' &middot; ' . $e($what) : '') . '</div>';
        }
        $detail .= '</div>';
    }

    $last = end($recent);
    return ['summary' => 'Today ' . $sum[0] . ' · Avg ' . $avg($sum[1]) . ' · Total ' . $sum[1] .
                         ($last ? ' · Last: ' . $last['name'] : ''),
            'open'    => 'Per disk and cause, averaged over ' . number_format($days) . ($days == 1 ? ' day' : ' days'),
            'detail'  => $detail];
}

// Why the daemon is paused, for the status line: what it reports itself in
// /var/run/unspind.pause-reasons (a running Mover or rsync, a pause by signal),
// then the names of the pause locks in /var/run/unspind.pause.d.
function pause_reasons() {
    $labels  = ['Mover' => 'Mover running', 'rsync' => 'rsync running', 'signal' => 'signal'];
    $reasons = [];
    foreach (@file('/var/run/unspind.pause-reasons', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $r)
        $reasons[] = $labels[$r] ?? $r;
    foreach (@scandir('/var/run/unspind.pause.d') ?: [] as $f)
        if ($f !== '.' && $f !== '..') $reasons[] = $f;
    return $reasons;
}

function daemon_running($pid_file) {
    if (!file_exists($pid_file)) return false;
    $pid = (int)trim(file_get_contents($pid_file));
    return $pid > 0 && file_exists("/proc/$pid");
}

// Parse /boot/config/shares/*.cfg -> ['share' => ['use_cache'=>..,'cache_pool'=>..]].
function load_unraid_shares() {
    $out = [];
    $dir = '/boot/config/shares';
    if (!is_dir($dir)) return $out;
    foreach (glob("$dir/*.cfg") as $path) {
        $share = basename($path, '.cfg');
        $info  = ['use_cache' => '', 'cache_pool' => ''];
        foreach (file($path, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
            $line = trim($line);
            if ($line === '' || $line[0] === '#' || strpos($line, '=') === false) continue;
            [$k, $v] = explode('=', $line, 2);
            $k = trim($k); $v = trim($v, " \t\"'");
            if      ($k === 'shareUseCache')  $info['use_cache']  = $v;
            else if ($k === 'shareCachePool') $info['cache_pool'] = $v;
        }
        $out[$share] = $info;
    }
    ksort($out);
    return $out;
}

// Which pools are referenced by yes/prefer shares that aren't excluded by the user.
function detect_promotable_pools($shares, $excluded_set) {
    $pools = [];
    foreach ($shares as $name => $info) {
        if (isset($excluded_set[$name])) continue;
        if ($info['use_cache'] !== 'yes' && $info['use_cache'] !== 'prefer') continue;
        if ($info['cache_pool'] === '') continue;
        $pools[$info['cache_pool']] = true;
    }
    ksort($pools);
    return array_keys($pools);
}
