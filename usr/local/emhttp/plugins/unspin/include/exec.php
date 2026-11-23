<?php
/* Unspin - POST action handler
 * Served at /plugins/unspin/include/exec.php
 * Called via jQuery $.post() from Unspin.php - bypasses dynamix page wrapper.
 */

$rc_script = "/etc/rc.d/rc.unspin";

header('Content-Type: application/json');

require_once __DIR__ . '/cfg.php';

function save_cfg($path, $cfg) {
    $header = "# Unspin Configuration - managed by UI\n# " . date('Y-m-d H:i:s') . "\n\n";
    $body = '';
    foreach ($cfg as $k => $v) {
        $body .= "$k=\"$v\"\n";
    }
    file_put_contents($path, "{$header}{$body}");
}

$action    = $_POST['action'] ?? '';
$ok        = true;
$msg       = '';
$saved_cfg = null;

if ($action === 'save') {
    $defs  = load_cfg(UNSPIN_DEFAULTS);
    $prev  = load_cfg(UNSPIN_CFG);
    $p     = fn($k)           => $prev[$k] ?? $defs[$k] ?? '';
    $yn    = fn($k)           => in_array($_POST[$k] ?? '', ['yes','no'])                                             ? $_POST[$k] : $p($k);
    $enum  = fn($k, $vals)    => in_array($_POST[$k] ?? '', $vals)                                                    ? $_POST[$k] : $p($k);
    $str   = fn($k)           => (($v = trim($_POST[$k] ?? '')) !== '')                                               ? $v         : $p($k);
    $pint  = fn($k, $m = 1)   => isset($_POST[$k]) && ($v = intval($_POST[$k])) >= $m                                 ? $v         : intval($p($k));
    $range = fn($k, $lo, $hi) => isset($_POST[$k]) && ($v = intval($_POST[$k])) >= $lo && $v <= $hi                   ? $v         : intval($p($k));
    $size  = fn($k)           => preg_match('/^\d+(\.\d+)?\s*(KB|MB|GB|TB|K|M|G|T)?$/i', $v = trim($_POST[$k] ?? '')) ? $v         : $p($k);

    $cfg = [];
    $k = 'SERVICE';                  $cfg[$k] = $enum($k, ['enabled','disabled']);
    $k = 'SCAN_PATHS';               $cfg[$k] = $str($k);

    // Excluded shares: posted as SHARE_EXCLUDE_<name>=1|0 checkboxes.
    $shares      = load_unraid_shares();
    $excluded    = [];
    foreach ($shares as $sname => $info) {
        if ($info['use_cache'] !== 'yes' && $info['use_cache'] !== 'prefer') continue;
        $post_key = 'SHARE_EXCLUDE_' . $sname;
        // Checkbox unticked = excluded. Posted "1" means "treated" (checked).
        $treated = ($_POST[$post_key] ?? '') === '1';
        if (!$treated) $excluded[] = $sname;
    }
    sort($excluded);
    $cfg['EXCLUDED_SHARES'] = implode(',', $excluded);
    $excluded_set = array_flip($excluded);
    $detected_pools = detect_promotable_pools($shares, $excluded_set);

    // Scan Paths must be array disk mount points - reject the share mount and any
    // path sitting on a detected cache pool (checked here, not earlier, since it
    // needs $detected_pools).
    foreach (scan_paths($cfg) as $sp) {
        if (strncmp($sp, '/mnt/user', 9) === 0) {
            echo json_encode(['ok' => false,
                'message' => "Scan Paths must be array disk mount points (e.g. /mnt/disk1), not share paths ($sp). Use \"Detect Array Disks\" to auto-populate.",
                'running' => daemon_running()]);
            exit;
        }
        foreach ($detected_pools as $pool) {
            if ($sp === "/mnt/$pool" || strncmp($sp, "/mnt/$pool/", strlen("/mnt/$pool/")) === 0) {
                echo json_encode(['ok' => false,
                    'message' => "Scan Paths must be array disk mount points (e.g. /mnt/disk1), not the cache pool \"$pool\" ($sp).",
                    'running' => daemon_running()]);
                exit;
            }
        }
    }

    // Per-pool fill thresholds. Only write entries for currently-detected promotable pools.
    // Migration: if the old config had MAX_HOT_FILL_PERCENT, seed any pool input that
    // wasn't posted with that value; otherwise fall back to 80.
    $legacy = $prev['MAX_HOT_FILL_PERCENT'] ?? '';
    $legacy = ($legacy !== '' && is_numeric($legacy)) ? intval($legacy) : 80;
    foreach ($detected_pools as $pool) {
        $pk  = 'MAX_FILL_PERCENT_' . $pool;
        $raw = $_POST[$pk] ?? '';
        $val = is_numeric($raw) ? intval($raw) : $legacy;
        if ($val < 10) $val = 10;
        if ($val > 99) $val = 99;
        $cfg[$pk] = $val;
    }
    $k = 'SMALL_FILE_THRESHOLD';      $cfg[$k] = $size($k);
    $k = 'SMALL_MIN_ACCESSES';        $cfg[$k] = $pint($k);
    $k = 'LARGE_SHORT_MIN_ACCESSES';  $cfg[$k] = $pint($k);
    $k = 'LARGE_SHORT_WINDOW_MINS';   $cfg[$k] = $pint($k);
    $k = 'LARGE_LONG_MIN_ACCESSES';   $cfg[$k] = $pint($k);
    $k = 'LARGE_LONG_WINDOW_HOURS';   $cfg[$k] = $pint($k);
    $k = 'EXCLUDE_PATTERNS';          $cfg[$k] = trim($_POST[$k] ?? '');
    $k = 'DRY_RUN';                   $cfg[$k] = $yn($k);
    $k = 'LOG_LEVEL';                 $cfg[$k] = $enum($k, ['info','debug']);
    $k = 'LOG_EXCLUDED_SHARES';       $cfg[$k] = $yn($k);
    $k = 'LOG_MAX_SIZE_MB';           $cfg[$k] = $pint($k);
    $k = 'LOG_TRIM_SIZE_MB';          $cfg[$k] = $pint($k);
    $k = 'RULE1_ENABLED';             $cfg[$k] = $yn($k);
    $k = 'RULE1_FALLTHROUGH';         $cfg[$k] = $yn($k);
    $k = 'RULE2_ENABLED';             $cfg[$k] = $yn($k);
    $k = 'RULE3_ENABLED';             $cfg[$k] = $yn($k);
    $k = 'RULE3_MIN_READS';           $cfg[$k] = $pint($k, 0);
    $k = 'PAUSE_ON_RSYNC';            $cfg[$k] = $yn($k);
    $k = 'ENABLE_POOL_ATIME';         $cfg[$k] = $yn($k);
    $k = 'TRACK_WAKEUPS';             $cfg[$k] = $yn($k);
    $k = 'PROMOTE_ON_SPINUP';         $cfg[$k] = $enum($k, ['yes','parity','no']);
    $k = 'MOUNT_WAIT_TIMEOUT_MINS';   $cfg[$k] = $pint($k, 0);
    $k = 'MOUNT_RETRY_INTERVAL_SECS'; $cfg[$k] = $pint($k, 1);
    // Not on the settings page, only in unspin.cfg: kept as they are
    $k = 'COMPANION_LABEL';           $cfg[$k] = $yn($k);
    $k = 'COMPANION_HINT';            $cfg[$k] = $yn($k);
    save_cfg(UNSPIN_CFG, $cfg);
    $saved_cfg = $cfg;

    // Pool access times switched off: the settings page asked whether to undo
    // Unspin's changes now or at the next array start. The daemon reads this
    // marker on the SIGHUP reload below.
    $atime_msg = '';
    if ($p('ENABLE_POOL_ATIME') === 'yes' && $cfg['ENABLE_POOL_ATIME'] === 'no') {
        if (($_POST['ATIME_OFF'] ?? '') === 'now') {
            touch('/var/run/unspind.atime-revert-now');
            $atime_msg = ' Access times are being turned off on the pools Unspin enabled them on.';
        } else {
            $atime_msg = ' Access times will be turned off on the pools Unspin enabled them on at the next array start.';
        }
    }

    if (file_exists($rc_script)) {
        if ($cfg['SERVICE'] === 'enabled') {
            if ($pid = daemon_pid()) {
                posix_kill($pid, SIGHUP); // reload config
            } else {
                exec("$rc_script start < /dev/null > /dev/null 2>&1 &");
            }
        } else {
            exec("$rc_script stop < /dev/null > /dev/null 2>&1 &");
        }
    }
    $msg = 'Settings saved.' . $atime_msg;

} elseif ($action === 'start') {
    if (file_exists($rc_script)) exec("$rc_script start < /dev/null > /dev/null 2>&1 &");
    $msg = 'Unspin start requested.';

} elseif ($action === 'stop') {
    if (file_exists($rc_script)) exec("$rc_script stop < /dev/null > /dev/null 2>&1 &");
    $msg = 'Unspin stopped.';

} elseif ($action === 'clear_log') {
    file_put_contents(UNSPIN_LOG, '');
    $msg = 'Log cleared.';

} elseif ($action === 'reset_stats') {
    reset_stats_file('stats');
    echo json_encode(['ok' => true, 'message' => 'Statistics reset.',
                      'running' => daemon_running(), 'stats' => promotion_stats()]);
    exit;

} elseif ($action === 'reset_wakeups') {
    reset_stats_file('wakeups');
    echo json_encode(['ok' => true, 'message' => 'Spin-up statistics reset.',
                      'running' => daemon_running(), 'wakeups' => disk_wakeups()]);
    exit;

} elseif ($action === 'promote' || $action === 'promote_cancel') {
    // Promote now: handed to rc.unspin, which writes the request for unspind
    if ($action === 'promote_cancel') {
        $args = 'cancel';
    } else {
        $path = rtrim(trim($_POST['path'] ?? ''), '/');
        $args = escapeshellarg($path) . (($_POST['recursive'] ?? '') === 'yes' ? ' --recursive' : '') .
                (($_POST['mode'] ?? '') === 'now' ? ' --now' : '');
    }
    $log_size = @filesize(UNSPIN_LOG) ?: 0;
    exec("$rc_script promote $args 2>&1", $out, $rc);
    $msg = implode(' ', $out);
    // unspind handles the request at once (rc.unspin signals it): show its answer from the log
    for ($i = 0; $rc === 0 && $i < 10; $i++) {
        usleep(200000);
        clearstatcache();
        $new = (string)@file_get_contents(UNSPIN_LOG, false, null, $log_size);
        if (preg_match_all('/^\S+ \S+ \[(INFO |WARN )\] ((?:\[promote [^\]]+\] \d+ file|Promote now: ).*)$/m', $new, $m)) {
            $msg = end($m[2]);
            if (end($m[1]) === 'WARN ') $rc = 1;
            break;
        }
    }
    echo json_encode(['ok' => $rc === 0, 'message' => $msg,
                      'running' => daemon_running(), 'stats' => promotion_stats()]);
    exit;

} elseif ($action === 'detect_disks') {
    $paths = trim(shell_exec(
        "awk '\$2 ~ /^\\/mnt\\/disk[0-9]+\$/ {print \$2}' /proc/mounts 2>/dev/null | sort -V | paste -sd,"
    ) ?? '');
    if ($paths !== '') {
        echo json_encode(['ok' => true,  'message' => "Detected: $paths",
                          'paths' => $paths, 'running' => daemon_running()]);
    } else {
        echo json_encode(['ok' => false, 'message' => 'No array disks found in /proc/mounts',
                          'running' => daemon_running()]);
    }
    exit;

} elseif ($action === 'poll') {
    require_once __DIR__ . '/log_scan.php';
    $poll_cfg     = load_cfg(UNSPIN_CFG);
    $exclude_skip = ($_POST['exclude_skip'] ?? '') === '1';
    $log_lines    = log_tail_lines(UNSPIN_LOG, log_tail_count($poll_cfg), $exclude_skip ? '[skip share]' : null);
    $pause_locks = pause_reasons();

    $recent = null;
    if (is_debug($poll_cfg)) {
        $scan_paths = scan_paths($poll_cfg);
        $offset     = isset($_POST['log_offset']) ? (int)$_POST['log_offset'] : null;
        $prev_lists = isset($_POST['log_lists']) ? json_decode($_POST['log_lists'], true) : null;

        if ($offset !== null && is_array($prev_lists)) {
            [$lists, $offset] = log_scan_incremental(UNSPIN_LOG, $offset, $scan_paths, $prev_lists);
        } else {
            [$lists, $offset] = log_scan_full(UNSPIN_LOG, $scan_paths);
        }
        $recent = ['offset' => $offset, 'lists' => $lists, 'html' => recent_panel($lists)];
    }

    echo json_encode([
        'ok'      => true,
        'running' => daemon_running(),
        'paused'  => count($pause_locks) > 0,
        'pause_locks' => $pause_locks,
        'log'     => $log_lines,
        'recent'  => $recent,
        'stats'   => promotion_stats(),
        'wakeups' => disk_wakeups(),
    ]);
    exit;

} else {
    $ok  = false;
    $msg = 'Unknown action.';
}

echo json_encode(['ok' => $ok, 'message' => $msg, 'running' => daemon_running(),
                  'cfg' => $saved_cfg]);
