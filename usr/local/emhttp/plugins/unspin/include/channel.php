<?php
/* Unspin - release channels (stable / beta)
 * Served at /plugins/unspin/include/channel.php
 * Which channel is installed, and the newest version on each channel.
 */

const UNSPIN_REPO      = 'https://github.com/kaedinger/unspin';
const UNSPIN_PLG       = '/boot/config/plugins/unspin.plg';
const UNSPIN_CH_CACHE  = '/tmp/unspin-channel';
const UNSPIN_CH_TTL    = 3600;

function channel_urls() {
    return [
        'stable' => UNSPIN_REPO . '/releases/latest/download/unspin.plg',
        // releases are immutable, so the rolling beta .plg lives on the `channel` branch
        'beta'   => 'https://raw.githubusercontent.com/kaedinger/unspin/channel/unspin.plg',
    ];
}

// version, pluginURL and CHANGES of a .plg file, or null
function channel_plg_info($file) {
    if (!is_file($file)) return null;
    $xml = @simplexml_load_file($file, 'SimpleXMLElement', LIBXML_NOENT);
    if (!$xml || !isset($xml['version'])) return null;
    return [
        'version' => (string)$xml['version'],
        'url'     => (string)$xml['pluginURL'],
        'changes' => trim((string)$xml->CHANGES),
    ];
}

function channel_installed() {
    $info = channel_plg_info(UNSPIN_PLG);
    if (!$info) return ['version' => 'unknown', 'channel' => 'stable'];
    $channel = ($info['url'] === channel_urls()['beta']) ? 'beta' : 'stable';
    return ['version' => $info['version'], 'channel' => $channel];
}

// newest .plg of a channel, cached for UNSPIN_CH_TTL; a stale copy is used when GitHub is unreachable.
// Also refreshed once Unraid's own update check (Plugins page) ran after the cache was filled,
// so this page never shows less than the Plugins page.
function channel_latest($ch, $refresh) {
    @mkdir(UNSPIN_CH_CACHE, 0755, true);
    $file  = UNSPIN_CH_CACHE . "/$ch.plg";
    $fresh = is_file($file) && time() - filemtime($file) < UNSPIN_CH_TTL &&
             @filemtime('/tmp/plugins/unspin.plg') <= filemtime($file);
    if ($refresh || !$fresh) {
        $tmp = "$file.tmp";
        $rc  = 1;
        exec('curl -fsSL --max-time 10 -o ' . escapeshellarg($tmp) . ' ' . escapeshellarg(channel_urls()[$ch]) . ' 2>/dev/null', $out, $rc);
        if ($rc === 0 && channel_plg_info($tmp)) rename($tmp, $file); else @unlink($tmp);
    }
    $info = channel_plg_info($file);
    if (!$info) return null;
    // release notes for Unraid's own popup (showchanges only reads .txt files under /tmp/plugins)
    @mkdir('/tmp/plugins', 0755, true);
    $txt = "/tmp/plugins/unspin-$ch.txt";
    file_put_contents($txt, $info['changes']);
    return ['version' => $info['version'], 'url' => channel_urls()[$ch], 'notes' => $txt];
}

header('Content-Type: application/json');
$action = $_REQUEST['action'] ?? '';
if ($action === 'versions') {
    $refresh = !empty($_GET['refresh']);
    echo json_encode([
        'installed' => channel_installed(),
        'stable'    => channel_latest('stable', $refresh),
        'beta'      => channel_latest('beta', $refresh),
    ]);
} elseif ($action === 'check' && $_SERVER['REQUEST_METHOD'] === 'POST') {
    // after a switch or update: Unraid's update check follows the new pluginURL right away
    exec('/usr/local/sbin/plugin check unspin.plg >/dev/null 2>&1');
    array_map('unlink', glob(UNSPIN_CH_CACHE . '/*.plg') ?: []);
    echo json_encode(['ok' => true]);
} else {
    http_response_code(400);
    echo json_encode(['error' => 'unknown action']);
}
