<?php
/* Unspin - POST action handler
 * Served at /plugins/unspin/include/exec.php
 * Called via jQuery $.post() from Unspin.php - bypasses dynamix page wrapper.
 */

$cfg_file  = "/boot/config/plugins/unspin/unspin.cfg";
$log_file  = "/var/log/unspin.log";
$pid_file  = "/var/run/unspind.pid";
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
    $defaults_file = "/usr/local/emhttp/plugins/unspin/unspin.cfg.default";
    $defs  = load_cfg($defaults_file);
    $prev  = load_cfg($cfg_file);
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
    foreach (array_map('trim', explode(',', $cfg['SCAN_PATHS'])) as $sp) {
        if ($sp === '') continue;
        if (strncmp($sp, '/mnt/user', 9) === 0) {
            echo json_encode(['ok' => false,
                'message' => "Scan Paths must be array disk mount points (e.g. /mnt/disk1), not share paths ($sp). Use \"Detect Array Disks\" to auto-populate.",
                'running' => daemon_running($pid_file)]);
            exit;
        }
        foreach ($detected_pools as $pool) {
            if ($sp === "/mnt/$pool" || strncmp($sp, "/mnt/$pool/", strlen("/mnt/$pool/")) === 0) {
                echo json_encode(['ok' => false,
                    'message' => "Scan Paths must be array disk mount points (e.g. /mnt/disk1), not the cache pool \"$pool\" ($sp).",
                    'running' => daemon_running($pid_file)]);
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
    $k = 'MOUNT_WAIT_TIMEOUT_MINS';   $cfg[$k] = $pint($k, 0);
    $k = 'MOUNT_RETRY_INTERVAL_SECS'; $cfg[$k] = $pint($k, 1);
    save_cfg($cfg_file, $cfg);
    $saved_cfg = $cfg;

    if (file_exists($rc_script)) {
        if ($cfg['SERVICE'] === 'enabled') {
            if (daemon_running($pid_file)) {
                $pid = (int)trim(file_get_contents($pid_file));
                posix_kill($pid, SIGHUP); // reload config
            } else {
                exec("$rc_script start < /dev/null > /dev/null 2>&1 &");
            }
        } else {
            exec("$rc_script stop < /dev/null > /dev/null 2>&1 &");
        }
    }
    $msg = 'Settings saved.';

} elseif ($action === 'start') {
    if (file_exists($rc_script)) exec("$rc_script start < /dev/null > /dev/null 2>&1 &");
    $msg = 'Daemon start requested.';

} elseif ($action === 'stop') {
    if (file_exists($rc_script)) exec("$rc_script stop < /dev/null > /dev/null 2>&1 &");
    $msg = 'Daemon stopped.';

} elseif ($action === 'clear_log') {
    file_put_contents($log_file, '');
    $msg = 'Log cleared.';

} elseif ($action === 'detect_disks') {
    $paths = trim(shell_exec(
        "awk '\$2 ~ /^\\/mnt\\/disk[0-9]+\$/ {print \$2}' /proc/mounts 2>/dev/null | sort -V | paste -sd,"
    ) ?? '');
    if ($paths !== '') {
        echo json_encode(['ok' => true,  'message' => "Detected: $paths",
                          'paths' => $paths, 'running' => daemon_running($pid_file)]);
    } else {
        echo json_encode(['ok' => false, 'message' => 'No array disks found in /proc/mounts',
                          'running' => daemon_running($pid_file)]);
    }
    exit;

} elseif ($action === 'poll') {
    require_once __DIR__ . '/log_scan.php';
    $log_lines = log_tail_lines($log_file, 200);
    $pause_dir = '/var/run/unspind.pause.d';
    $pause_locks = [];
    if (is_dir($pause_dir)) {
        foreach (scandir($pause_dir) as $f) {
            if ($f !== '.' && $f !== '..') $pause_locks[] = $f;
        }
    }

    $recent = null;
    $poll_cfg = load_cfg($cfg_file);
    if (($poll_cfg['LOG_LEVEL'] ?? '') === 'debug') {
        $scan_paths = array_values(array_filter(array_map('trim',
            explode(',', $poll_cfg['SCAN_PATHS'] ?? ''))));
        $offset     = isset($_POST['log_offset']) ? (int)$_POST['log_offset'] : null;
        $prev_lists = isset($_POST['log_lists']) ? json_decode($_POST['log_lists'], true) : null;

        if ($offset !== null && is_array($prev_lists)) {
            [$lists, $offset] = log_scan_incremental($log_file, $offset, $scan_paths, $prev_lists);
        } else {
            [$lists, $offset] = log_scan_full($log_file, $scan_paths);
        }
        $recent = ['offset' => $offset, 'lists' => $lists];
    }

    echo json_encode([
        'ok'      => true,
        'running' => daemon_running($pid_file),
        'paused'  => count($pause_locks) > 0,
        'pause_locks' => $pause_locks,
        'log'     => $log_lines,
        'recent'  => $recent,
    ]);
    exit;

} else {
    $ok  = false;
    $msg = 'Unknown action.';
}

echo json_encode(['ok' => $ok, 'message' => $msg, 'running' => daemon_running($pid_file),
                  'cfg' => $saved_cfg]);
