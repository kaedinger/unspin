<?php
/* Unspin - shared config/share-parsing helpers
 * Required by both Unspin.php (page render) and exec.php (POST handler).
 */

const UNSPIN_CFG      = '/boot/config/plugins/unspin/unspin.cfg';
const UNSPIN_DEFAULTS = '/usr/local/emhttp/plugins/unspin/unspin.cfg.default';
const UNSPIN_LOG      = '/var/log/unspin.log';
const UNSPIN_PID      = '/var/run/unspind.pid';

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

// The settings in effect: the saved ones over the defaults
function settings() {
    return [...load_cfg(UNSPIN_DEFAULTS), ...load_cfg(UNSPIN_CFG)];
}

// Debug logging also shows the Recently Accessed Files panels and a longer log
function is_debug($cfg) {
    return ($cfg['LOG_LEVEL'] ?? '') === 'debug';
}

// Lines of the log shown on the page
function log_tail_count($cfg) {
    return is_debug($cfg) ? 1000 : 200;
}

// The array disks Unspin watches, from the comma separated SCAN_PATHS setting
function scan_paths($cfg) {
    return array_values(array_filter(array_map('trim', explode(',', $cfg['SCAN_PATHS'] ?? ''))));
}

// Lines of a statistics file the daemon keeps (stats, wakeups): the current copy
// in RAM, or the one on the flash (written hourly) after a reboot.
function stats_lines($name) {
    $ram = "/var/run/unspind.$name";
    return @file(file_exists($ram) ? $ram : "/boot/config/plugins/unspin/$name", FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [];
}

// Reset a statistics file. The daemon also holds the numbers in memory; the
// marker makes it drop them before its next write. Marker first, so a write in
// between can't bring the deleted file back for good.
function reset_stats_file($name) {
    touch("/var/run/unspind.$name-reset");
    @unlink("/var/run/unspind.$name");
    @unlink("/boot/config/plugins/unspin/$name");
}

// Days from $since (Y-m-d) to today, both included
function days_since($since) {
    return max(1, (int)round((strtotime(date('Y-m-d')) - strtotime($since)) / 86400) + 1);
}

// Header text of an open statistics section
function averaged_over($per, $days) {
    return "Per $per, averaged over " . number_format($days) . ($days == 1 ? ' day' : ' days');
}

function fmt_bytes($n) {
    $u = ['B','KB','MB','GB','TB','PB'];
    $i = 0;
    $v = (float)$n;
    while ($v >= 1024 && $i < count($u) - 1) { $v /= 1024; $i++; }
    return ($v >= 100 ? round($v) : round($v, 1)) . ' ' . $u[$i];
}

// Shown time of an event: the time only today, else with the date
function fmt_when($epoch) {
    return date(date('Y-m-d', $epoch) === date('Y-m-d') ? 'H:i:s' : 'M j H:i:s', $epoch);
}

// One card of the per-disk panels (Latest Disk Wake-ups, Recently Accessed Files):
// the disk as title, optionally after a mark (HTML), an optional line under it
// (HTML), and the table rows (HTML).
function disk_card($disk, $rows, $sub = '', $mark = '') {
    return '<div class="recent-disk hf-bordered"><div class="recent-disk-title">' . $mark . htmlspecialchars($disk) . '</div>' .
           $sub . '<table class="recent-table"><tbody>' . $rows . '</tbody></table></div>';
}

// Current spin state of the array disks from Unraid's disks.ini: name => spinning
function disks_spinning() {
    $out = [];
    foreach (@parse_ini_file('/var/local/emhttp/disks.ini', true) ?: [] as $d)
        if (isset($d['name'], $d['spundown'])) $out[$d['name']] = $d['spundown'] !== '1';
    return $out;
}

// Dot before a disk name: green while spinning, an empty circle while spun down
function spin_mark($spinning) {
    return $spinning ? '<span class="hf-spin on" title="Spinning"></span>' : '<span class="hf-spin" title="Spun down"></span>';
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
    foreach (stats_lines('stats') as $line) {
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

    $days  = days_since($since);
    $cell = fn($files, $bytes, $dec = 0) => number_format($files, $dec) . ' (' . fmt_bytes($bytes) . ')';
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
            'open'    => averaged_over('pool', $days),
            'detail'  => $detail];
}

// Disk wake-ups from the daemon's wakeups file (RAM copy, or the flash copy after
// a reboot): "C date disk label count" lines per day, disk and cause, and
// "W epoch disk label name access path also" lines for the recent wake-ups.
// Returns the same shape as promotion_stats() - a summary line, the header text
// while open, and the detail HTML (one row per disk) - plus 'panel': one card per
// disk with its counts per cause and its latest wake-ups, shown above the log.
function disk_wakeups() {
    $today  = date('Y-m-d');
    $since  = null;
    $counts = [];   // disk => label => [today, total]
    $recent = [];
    foreach (stats_lines('wakeups') as $line) {
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
    $e = fn($s) => htmlspecialchars($s);
    uksort($counts, 'strnatcmp');
    $by_disk = [];
    foreach (array_reverse($recent) as $w) $by_disk[$w['disk']][] = $w;   // newest first

    // One card per watched disk, wake-ups or not, plus disks no longer watched
    // that still have some.
    $disks = array_unique(array_merge(array_map('basename', scan_paths(settings())), array_keys($counts)));
    usort($disks, 'strnatcmp');
    $spin  = disks_spinning();
    $panel = '';
    foreach ($disks as $disk) {
        $labels = $counts[$disk] ?? [];
        uasort($labels, fn($a, $b) => $b[1] <=> $a[1]);
        $causes = [];
        foreach ($labels as $label => $c) $causes[] = $e($label) . ' ' . $c[1];
        $rows = '';
        foreach (array_slice($by_disk[$disk] ?? [], 0, 5) as $w) {
            $what  = $w['path'] !== '' ? $w['access'] . ' ' . basename($w['path']) : '';
            $tip   = trim($w['path'] . ($w['also'] !== '' ? "\nAlso active: " . $w['also'] : ''));
            $rows .= '<tr title="' . $e($tip) . '">' .
                     '<td class="hf-wake-cause" data-full="' . $e($w['label']) . '">' . $e($w['label']) . '</td>' .
                     '<td class="recent-path" data-full="' . $e($what) . '">' . $e($what) . '</td>' .
                     '<td class="hf-time">' . $e(fmt_when($w['at'])) . '</td></tr>';
        }
        if ($rows === '') $rows = '<tr><td class="hf-muted">No wake-ups recorded yet.</td></tr>';
        $panel .= disk_card($disk, $rows, !$causes ? '' : '<div class="hf-wake-causes" title="' . $e(implode(', ', array_keys($labels))) . '">' .
                                                          implode(' &middot; ', $causes) . '</div>',
                            isset($spin[$disk]) ? spin_mark($spin[$disk]) : '');
    }

    if (!$counts) return ['summary' => 'No wake-ups recorded yet', 'open' => 'No wake-ups recorded yet',
                          'detail' => '', 'panel' => $panel];

    $days = days_since($since);
    $avg = fn($n) => number_format($n / $days, $n / $days < 10 ? 1 : 0);
    $sum  = [0, 0];
    foreach ($counts as $labels) foreach ($labels as $c) { $sum[0] += $c[0]; $sum[1] += $c[1]; }

    $detail = '<div class="hf-stats-grid hf-wake-grid"><span></span><span>Today</span><span>Avg/day</span>' .
              '<span>Total</span><span>Top cause</span>';
    foreach ($counts as $disk => $labels) {
        uasort($labels, fn($a, $b) => $b[1] <=> $a[1]);
        $top = array_key_first($labels);
        $dt  = [array_sum(array_column($labels, 0)), array_sum(array_column($labels, 1))];
        $detail .= '<span>' . $e($disk) . '</span><span>' . $dt[0] . '</span><span>' . $avg($dt[1]) .
                   '</span><span>' . $dt[1] . '</span><span class="hf-wake-label" title="' . $e($top) . '">' . $e($top) . '</span>';
    }
    $detail .= '</div>';

    $last = end($recent);
    return ['summary' => 'Today ' . $sum[0] . ' · Avg ' . $avg($sum[1]) . ' · Total ' . $sum[1] .
                         ($last ? ' · Last: ' . $last['name'] : ''),
            'open'    => averaged_over('disk', $days),
            'detail'  => $detail,
            'panel'   => $panel];
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

// PID of the running daemon, or 0
function daemon_pid() {
    $pid = (int)@file_get_contents(UNSPIN_PID);
    return $pid > 0 && file_exists("/proc/$pid") ? $pid : 0;
}

function daemon_running() {
    return daemon_pid() > 0;
}

// Parse /boot/config/shares/*.cfg -> ['share' => ['use_cache'=>..,'cache_pool'=>..]].
function load_unraid_shares() {
    $out = [];
    $dir = '/boot/config/shares';
    if (!is_dir($dir)) return $out;
    foreach (glob("$dir/*.cfg") as $path) {
        $c = load_cfg($path);
        $out[basename($path, '.cfg')] = ['use_cache' => $c['shareUseCache'] ?? '', 'cache_pool' => $c['shareCachePool'] ?? ''];
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
