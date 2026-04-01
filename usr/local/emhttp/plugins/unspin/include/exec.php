<?php
/* Unspin — POST action handler
 * Served at /plugins/unspin/include/exec.php
 * Called via jQuery $.post() from Unspin.php — bypasses dynamix page wrapper.
 */

$cfg_file  = "/boot/config/plugins/unspin/unspin.cfg";
$log_file  = "/var/log/unspin.log";
$pid_file  = "/var/run/unspind.pid";
$rc_script = "/etc/rc.d/rc.unspin";

header('Content-Type: application/json');

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

function save_cfg($path, $cfg) {
    $header = "# Unspin Configuration — managed by UI\n# " . date('Y-m-d H:i:s') . "\n\n";
    $body = '';
    foreach ($cfg as $k => $v) {
        $body .= "$k=\"$v\"\n";
    }
    file_put_contents($path, "{$header}{$body}");
}

function daemon_running($pid_file) {
    if (!file_exists($pid_file)) return false;
    $pid = (int)trim(file_get_contents($pid_file));
    return $pid > 0 && file_exists("/proc/$pid");
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
    $yn    = fn($k)           => in_array($_POST[$k] ?? '', ['yes','no'])                                          ? $_POST[$k] : $p($k);
    $enum  = fn($k, $vals)    => in_array($_POST[$k] ?? '', $vals)                                                 ? $_POST[$k] : $p($k);
    $str   = fn($k)           => (($v = trim($_POST[$k] ?? '')) !== '')                                            ? $v         : $p($k);
    $pint  = fn($k, $m = 1)   => isset($_POST[$k]) && ($v = intval($_POST[$k])) >= $m                             ? $v         : intval($p($k));
    $range = fn($k, $lo, $hi) => isset($_POST[$k]) && ($v = intval($_POST[$k])) >= $lo && $v <= $hi               ? $v         : intval($p($k));
    $size  = fn($k)           => preg_match('/^\d+(\.\d+)?\s*(KB|MB|GB|TB|K|M|G|T)?$/i', $v = trim($_POST[$k] ?? '')) ? $v    : $p($k);

    $cfg = [];
    $k = 'SERVICE';                  $cfg[$k] = $enum($k, ['enabled','disabled']);
    $k = 'HOT_PATH';                 $cfg[$k] = $str($k);
    $k = 'SCAN_PATHS';               $cfg[$k] = $str($k);
    foreach (array_map('trim', explode(',', $cfg['SCAN_PATHS'])) as $sp) {
        if ($sp !== '' && strncmp($sp, '/mnt/user', 9) === 0) {
            echo json_encode(['ok' => false,
                'message' => "Scan Paths must be array disk mount points (e.g. /mnt/disk1), not share paths ($sp). Use \"Detect Array Disks\" to auto-populate.",
                'running' => daemon_running($pid_file)]);
            exit;
        }
    }
    $k = 'MAX_HOT_FILL_PERCENT';     $cfg[$k] = $range($k, 10, 99);
    $k = 'SMALL_FILE_THRESHOLD';     $cfg[$k] = $size($k);
    $k = 'SMALL_MIN_ACCESSES';       $cfg[$k] = $pint($k);
    $k = 'LARGE_SHORT_MIN_ACCESSES'; $cfg[$k] = $pint($k);
    $k = 'LARGE_SHORT_WINDOW_MINS';  $cfg[$k] = $pint($k);
    $k = 'LARGE_LONG_MIN_ACCESSES';  $cfg[$k] = $pint($k);
    $k = 'LARGE_LONG_WINDOW_HOURS';  $cfg[$k] = $pint($k);
    $k = 'EXCLUDE_PATTERNS';         $cfg[$k] = trim($_POST[$k] ?? '');
    $k = 'DRY_RUN';                  $cfg[$k] = $yn($k);
    $k = 'LOG_LEVEL';                $cfg[$k] = $enum($k, ['info','debug']);
    $k = 'RULE1_ENABLED';            $cfg[$k] = $yn($k);
    $k = 'RULE1_FALLTHROUGH';        $cfg[$k] = $yn($k);
    $k = 'RULE2_ENABLED';            $cfg[$k] = $yn($k);
    $k = 'RULE3_ENABLED';            $cfg[$k] = $yn($k);
    $k = 'RULE3_MIN_READS';          $cfg[$k] = $pint($k, 0);
    $k = 'PAUSE_ON_RSYNC';          $cfg[$k] = $yn($k);
    save_cfg($cfg_file, $cfg);
    $saved_cfg = $cfg;

    if (file_exists($rc_script)) {
        if ($cfg['SERVICE'] === 'enabled') {
            exec("$rc_script restart < /dev/null > /dev/null 2>&1 &");
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
    $log_lines = '';
    if (file_exists($log_file)) {
        $log_lines = implode('', array_slice(file($log_file), -200));
    }
    $pause_dir = '/var/run/unspind.pause.d';
    $pause_locks = [];
    if (is_dir($pause_dir)) {
        foreach (scandir($pause_dir) as $f) {
            if ($f !== '.' && $f !== '..') $pause_locks[] = $f;
        }
    }
    echo json_encode([
        'ok'      => true,
        'running' => daemon_running($pid_file),
        'paused'  => count($pause_locks) > 0,
        'pause_locks' => $pause_locks,
        'log'     => $log_lines,
    ]);
    exit;

} else {
    $ok  = false;
    $msg = 'Unknown action.';
}

echo json_encode(['ok' => $ok, 'message' => $msg, 'running' => daemon_running($pid_file),
                  'cfg' => $saved_cfg]);
