<?php
/* Unspin - Settings page for Unraid
 * Included by Unspin.page (GET only - rendering).
 * All POST actions are handled by include/exec.php via jQuery $.post().
 */

$cfg_file = "/boot/config/plugins/unspin/unspin.cfg";
$log_file = "/var/log/unspin.log";
$pid_file = "/var/run/unspind.pid";

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

function daemon_running($pid_file) {
    if (!file_exists($pid_file)) return false;
    $pid = (int)trim(file_get_contents($pid_file));
    return $pid > 0 && file_exists("/proc/$pid");
}

// Insert a space between the numeric part and the unit (e.g. "500MB" → "500 MB")
function fmt_size($v) {
    return preg_replace('/(\d)([KMGT]B?)$/i', '$1 $2', trim($v));
}

function fmt_bytes($n) {
    $u = ['B','KB','MB','GB','TB','PB'];
    $i = 0;
    $v = (float)$n;
    while ($v >= 1024 && $i < count($u) - 1) { $v /= 1024; $i++; }
    return ($v >= 100 ? round($v) : round($v, 1)) . ' ' . $u[$i];
}

function opt($val, $current, $label) {
    $sel = ($current === $val) ? ' selected' : '';
    return "<option value=\"" . htmlspecialchars($val) . "\"$sel>" . htmlspecialchars($label) . "</option>";
}

// Parse /boot/config/shares/*.cfg -> ['share' => ['use_cache'=>..,'cache_pool'=>..]].
// Mirrors load_shares() in exec.php; duplicated to keep the page render self-contained.
function unspin_load_shares() {
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

$defaults_file = "/usr/local/emhttp/plugins/unspin/unspin.cfg.default";
$defaults = load_cfg($defaults_file);
$cfg      = load_cfg($cfg_file);
$c        = [...$defaults, ...$cfg];

$plg_file = '/boot/config/plugins/unspin.plg';
$version  = 'unknown';
if (file_exists($plg_file)) {
    $plg = file_get_contents($plg_file);
    if (preg_match('/<!ENTITY\s+version\s+"([^"]+)"/', $plg, $m))
        $version = $m[1];
}

$is_running = daemon_running($pid_file);
$pause_dir  = '/var/run/unspind.pause.d';
$pause_locks = [];
if (is_dir($pause_dir)) {
    foreach (scandir($pause_dir) as $f) {
        if ($f !== '.' && $f !== '..') $pause_locks[] = $f;
    }
}
$is_paused = count($pause_locks) > 0;
$pause_label = $is_paused ? ' (Paused: ' . htmlspecialchars(implode(', ', $pause_locks)) . ')' : '';

$log_lines = '';
if (file_exists($log_file)) {
    $log_lines = htmlspecialchars(implode('', array_slice(file($log_file), -200)));
}

// Detected shares + excluded set for the Detected Shares table.
$shares         = unspin_load_shares();
$excluded_csv   = $c['EXCLUDED_SHARES'] ?? '';
$excluded_set   = array_flip(array_filter(array_map('trim', explode(',', $excluded_csv))));

// Pools currently referenced by promotable (yes/prefer, not excluded) shares.
$detected_pools = [];
foreach ($shares as $sname => $info) {
    if (isset($excluded_set[$sname])) continue;
    if ($info['use_cache'] !== 'yes' && $info['use_cache'] !== 'prefer') continue;
    if ($info['cache_pool'] === '') continue;
    $detected_pools[$info['cache_pool']] = true;
}
ksort($detected_pools);
$detected_pools = array_keys($detected_pools);

// Legacy migration seed for fill % inputs.
$legacy_fill = $c['MAX_HOT_FILL_PERCENT'] ?? '';
$legacy_fill = is_numeric($legacy_fill) ? intval($legacy_fill) : 80;
?>

<style>
/* Spinner used inside busy buttons */
.hf-spinner {
  display: inline-block;
  width: 11px;
  height: 11px;
  border: 2px solid currentColor;
  border-top-color: transparent;
  border-radius: 50%;
  opacity: 0.7;
  animation: hf-spin 0.7s linear infinite;
  vertical-align: middle;
  margin-right: 5px;
}
@keyframes hf-spin { to { transform: rotate(360deg); } }

/* Clickable help labels */
dt.hf-has-help {
  cursor: help;
  user-select: none;
}

/* Help text panels - hidden by default */
.hf-help {
  overflow: hidden;
  max-height: 0;
  max-width: 400px;
  opacity: 0;
  transition: max-height 0.25s ease, opacity 0.2s ease,
              padding 0.2s ease, margin-top 0.2s ease, border-color 0.2s ease;
  background: #fff;
  color: #111;
  font-size: 0.9em;
  line-height: 1.5;
  border-radius: 4px;
  border: 1px solid transparent;
  padding: 0 10px;
  margin-top: 0;
  box-sizing: border-box;
}
.hf-help.open {
  max-height: 300px;
  opacity: 1;
  padding: 7px 10px;
  margin-top: 5px;
  border-color: #ccc;
}

/* Match Unraid's standard form element width */
#hf-toggle-btn, #hf-apply-btn {
  width: 400px !important;
}

/* Rule section header labels */
dt.hf-rule-label {
  text-decoration: underline;
}

/* Inline control row - input(s) + reset icon(s) + optional text labels */
.hf-row {
  display: flex;
  align-items: center;
  gap: 10px;
}

/* Author footer */
.hf-footer {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 12px;
  font-size: 0.85em;
  transition: opacity 0.2s;
}
.hf-footer-version {
  margin-left: auto;
  opacity: 0.5;
}
.hf-footer:hover { opacity: 1; }
.hf-footer-avatar {
  width: 28px;
  height: 28px;
  border-radius: 50%;
}
.hf-footer-sep { opacity: 0.35; }

/* Reset-to-default icon buttons */
.hf-reset {
  font-size: 1.35em;
  color: #f5a623;
  opacity: 0.8;
  cursor: pointer;
  line-height: 1;
  display: inline-flex;
  align-items: center;
  user-select: none;
}
.hf-reset:hover { opacity: 1; }

/* Per-share row in Detected Shares */
dt.hf-share-name {
  text-align: right;
  font-weight: normal;
}
.hf-share-row {
  display: grid;
  grid-template-columns: 6em 8em 11em 3em;
  align-items: center;
  gap: 10px;
}
.hf-share-head { font-weight: bold; opacity: 0.75; }
</style>

<div id="hf-message" style="min-height:1.4em;margin-bottom:4px;"></div>

<dl>
  <dt>Daemon Status</dt>
  <dd>
    <strong id="hf-status" style="<?= $is_running ? ($is_paused ? 'color:#facc15' : 'color:#4ade80') : 'color:#f87171' ?>">
      <?= $is_running ? ('Running' . $pause_label) : 'Stopped' ?>
    </strong>
    &nbsp;&nbsp;
    <button type="button" id="hf-toggle-btn" onclick="hfDaemonToggle()">
      <?= $is_running ? 'Stop Daemon' : 'Start Daemon' ?>
    </button>
  </dd>
</dl>

<hr>

<dl>
  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Enable Unspin</dt>
  <dd>
    <select name="SERVICE" id="hf_SERVICE">
      <?= opt('disabled', $c['SERVICE'], 'No') ?>
      <?= opt('enabled',  $c['SERVICE'], 'Yes') ?>
    </select>
    <div class="hf-help">Master switch. When set to Yes the daemon starts immediately and restarts after reboots. <strong>Default: No.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Dry Run Mode</dt>
  <dd>
    <select name="DRY_RUN" id="hf_DRY_RUN">
      <?= opt('yes', $c['DRY_RUN'], 'Yes - log decisions only, no moves') ?>
      <?= opt('no',  $c['DRY_RUN'], 'No - move files') ?>
    </select>
    <div class="hf-help">Leave enabled until you are satisfied with the promotion decisions in the log. <strong>Default: Yes.</strong></div>
  </dd>

  <dt>Log Level</dt>
  <dd>
    <select name="LOG_LEVEL" id="hf_LOG_LEVEL">
      <?= opt('info',  $c['LOG_LEVEL'], 'Info') ?>
      <?= opt('debug', $c['LOG_LEVEL'], 'Debug (verbose - logs every access with counts)') ?>
    </select>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Pause on rsync</dt>
  <dd>
    <select name="PAUSE_ON_RSYNC" id="hf_PAUSE_ON_RSYNC">
      <?= opt('yes', $c['PAUSE_ON_RSYNC'], 'Yes') ?>
      <?= opt('no',  $c['PAUSE_ON_RSYNC'], 'No') ?>
    </select>
    <div class="hf-help">Pause event counting while any <code>rsync</code> process is running on the system. Unraid's Mover always pauses Unspin regardless of this setting. Tools like <em>Unbalanced</em> use rsync to move files between array disks - leaving this enabled prevents their activity from inflating access counters and triggering false promotions. <strong>Default: Yes.</strong></div>
  </dd>
</dl>

<hr>

<dl>
  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Scan Paths</dt>
  <dd>
    <div class="hf-row">
      <input type="text" id="hf_SCAN_PATHS" name="SCAN_PATHS"
        value="<?= htmlspecialchars($c['SCAN_PATHS']) ?>" style="flex:1;min-width:0">
      <span class="hf-reset" onclick="hfDetectDisks()" title="Detect array disks">&#x21ba;</span>
    </div>
    <div class="hf-help">Comma-separated <strong>array disk</strong> mount points to watch, e.g. <code>/mnt/disk1,/mnt/disk2,/mnt/disk3</code>. Use disk paths, not shares!</div>
  </dd>

  <dt class="hf-has-help" data-help-target="hf-shares-help" onclick="hfToggleHelp(this)">Detected Shares</dt>
  <dd>
    <?php if (!empty($shares)): ?>
    <div class="hf-share-row hf-share-head">
      <span>Use Cache</span>
      <span>Pool</span>
      <span>Status</span>
      <span>Watch</span>
    </div>
    <?php endif; ?>
  </dd>
  <?php if (empty($shares)): ?>
    <dt></dt>
    <dd style="opacity:0.6;">No shares found in <code>/boot/config/shares</code>.</dd>
  <?php else: foreach ($shares as $sname => $info):
    $uc    = $info['use_cache'] ?: '-';
    $pool  = $info['cache_pool'] ?: '-';
    $promo = ($info['use_cache'] === 'yes' || $info['use_cache'] === 'prefer') && $info['cache_pool'] !== '';
    $skip  = '';
    if (!$promo) {
        if      ($info['use_cache'] === '')     $skip = 'no use_cache';
        else if ($info['use_cache'] === 'no')   $skip = 'cache disabled';
        else if ($info['use_cache'] === 'only') $skip = 'cache-only';
        else if ($info['cache_pool'] === '')    $skip = 'no pool';
        else                                    $skip = 'not promotable';
    }
    $ticked = $promo && !isset($excluded_set[$sname]);
    $status = !$promo
            ? '<span style="opacity:0.6;">' . htmlspecialchars($skip) . '</span>'
            : ($ticked
                ? '<span style="color:#4ade80;">promotable</span>'
                : '<span style="color:#facc15;">excluded</span>');
  ?>
    <dt class="hf-share-name"><code><?= htmlspecialchars($sname) ?></code></dt>
    <dd>
      <div class="hf-share-row">
        <span><?= htmlspecialchars($uc) ?></span>
        <span><?= htmlspecialchars($pool) ?></span>
        <span><?= $status ?></span>
        <span>
          <?php if ($promo): ?>
            <input type="hidden" name="SHARE_EXCLUDE_<?= htmlspecialchars($sname) ?>" value="0">
            <input type="checkbox" class="hf-share-cb"
                   id="hf_SHARE_EXCLUDE_<?= htmlspecialchars($sname) ?>"
                   name="SHARE_EXCLUDE_<?= htmlspecialchars($sname) ?>" value="1"
                   <?= $ticked ? 'checked' : '' ?>>
          <?php else: ?>
            <input type="checkbox" disabled>
          <?php endif; ?>
        </span>
      </div>
    </dd>
  <?php endforeach; endif; ?>

  <dt>&nbsp;</dt>
  <dd>
    <div class="hf-help" id="hf-shares-help">Untick a share to prevent Unspin from promoting its files, even when <code>shareUseCache</code> is <code>yes</code> or <code>prefer</code>. Shares with <code>no</code> or <code>only</code> are always skipped and cannot be toggled. Click <strong>Apply</strong> below to save your changes.</div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Pool Fill Limits</dt>
  <dd>
  <?php if (empty($detected_pools)): ?>
    <div style="opacity:0.6;">No promotable pools detected - enable at least one share above.</div>
  <?php else: foreach ($detected_pools as $pool):
      $key      = 'MAX_FILL_PERCENT_' . $pool;
      $val      = isset($c[$key]) && is_numeric($c[$key]) ? intval($c[$key]) : $legacy_fill;
      $mnt      = '/mnt/' . $pool;
      $total    = @disk_total_space($mnt);
      $free     = @disk_free_space($mnt);
      $size_str = '';
      if ($total && $free !== false) {
          $used    = $total - $free;
          $pct     = $total > 0 ? round($used * 100 / $total) : 0;
          $size_str = fmt_bytes($total) . ', ' . $pct . '% used';
      }
  ?>
    <div class="hf-row" style="margin-bottom:4px;">
      <span style="min-width:12em;font-family:monospace;"><?= htmlspecialchars($mnt) ?></span>
      <input type="number" id="hf_<?= htmlspecialchars($key) ?>" name="<?= htmlspecialchars($key) ?>"
             min="10" max="99" value="<?= $val ?>" style="width:5em"
             data-hf-pool="1">
      <span>%</span>
      <span class="hf-reset" onclick="document.getElementById('hf_<?= htmlspecialchars($key) ?>').value='80';document.getElementById('hf_<?= htmlspecialchars($key) ?>').dispatchEvent(new Event('input',{bubbles:true}));" title="Reset to default (80)">&#x21ba;</span>
      <?php if ($size_str): ?><span style="opacity:0.7;margin-left:1em;"><?= htmlspecialchars($size_str) ?></span><?php endif; ?>
    </div>
  <?php endforeach; endif; ?>
    <div class="hf-help">Stop promoting to a pool once its fill exceeds this percentage. One value per detected pool. Default <strong>80%</strong>.</div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Exclude Patterns</dt>
  <dd>
    <input type="text" id="hf_EXCLUDE_PATTERNS" name="EXCLUDE_PATTERNS"
      value="<?= htmlspecialchars($c['EXCLUDE_PATTERNS']) ?>">
    <div class="hf-help">Comma-separated partial path strings to skip, e.g. <code>/pagefile,/qbittorrent</code>. Orthogonal to the Detected Shares table.</div>
  </dd>
</dl>

<hr>

<dl>
  <dt class="hf-has-help hf-rule-label" onclick="hfToggleHelp(this)">Rule 1 - Small Files</dt>
  <dd>
    <select name="RULE1_ENABLED" id="hf_RULE1_ENABLED">
      <?= opt('yes', $c['RULE1_ENABLED'], 'Enabled') ?>
      <?= opt('no',  $c['RULE1_ENABLED'], 'Disabled') ?>
    </select>
    <div class="hf-help">Promote files at or below the threshold after a set number of reads.</div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Fallthrough</dt>
  <dd>
    <div class="hf-row">
      <select name="RULE1_FALLTHROUGH" id="hf_RULE1_FALLTHROUGH">
        <?= opt('no',  $c['RULE1_FALLTHROUGH'], 'No - small files only use Rule 1') ?>
        <?= opt('yes', $c['RULE1_FALLTHROUGH'], 'Yes - evaluate Rules 2 & 3 if Rule 1 is off or unmet') ?>
      </select>
      <span class="hf-reset" onclick="hfReset('RULE1_FALLTHROUGH')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">When enabled, small files that don't qualify under Rule 1 are also evaluated by Rules 2 and 3.</div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Threshold</dt>
  <dd>
    <div class="hf-row">
      <input type="text" id="hf_SMALL_FILE_THRESHOLD" name="SMALL_FILE_THRESHOLD"
        value="<?= htmlspecialchars(fmt_size($c['SMALL_FILE_THRESHOLD'])) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('SMALL_FILE_THRESHOLD')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">Files at or below this size use the small-file rule. Accepts human (<code>MB</code> / <code>GB</code>...) suffixes.</div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Min Accesses</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_SMALL_MIN_ACCESSES" name="SMALL_MIN_ACCESSES" min="1"
        value="<?= htmlspecialchars($c['SMALL_MIN_ACCESSES']) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('SMALL_MIN_ACCESSES')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">Promote a small file after this many total reads. Default <code>1</code> = promote on first access.</div>
  </dd>
</dl>

<hr>

<dl>
  <dt class="hf-has-help hf-rule-label" onclick="hfToggleHelp(this)">Rule 2 - Large Files, Short Window</dt>
  <dd>
    <select name="RULE2_ENABLED" id="hf_RULE2_ENABLED">
      <?= opt('yes', $c['RULE2_ENABLED'], 'Enabled') ?>
      <?= opt('no',  $c['RULE2_ENABLED'], 'Disabled') ?>
    </select>
    <div class="hf-help">Promote large files during active streaming (e.g. 100 reads in 5 min while watching a video).</div>
  </dd>

  <dt>Short Window Reads</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_LARGE_SHORT_MIN_ACCESSES" name="LARGE_SHORT_MIN_ACCESSES" min="1"
        value="<?= htmlspecialchars($c['LARGE_SHORT_MIN_ACCESSES']) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('LARGE_SHORT_MIN_ACCESSES')" title="Reset to default">&#x21ba;</span>
      reads in
      <input type="number" id="hf_LARGE_SHORT_WINDOW_MINS" name="LARGE_SHORT_WINDOW_MINS" min="1"
        value="<?= htmlspecialchars($c['LARGE_SHORT_WINDOW_MINS']) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('LARGE_SHORT_WINDOW_MINS')" title="Reset to default">&#x21ba;</span>
      minutes
    </div>
  </dd>
</dl>

<hr>

<dl>
  <dt class="hf-has-help hf-rule-label" onclick="hfToggleHelp(this)">Rule 3 - Large Files, Long Window</dt>
  <dd>
    <select name="RULE3_ENABLED" id="hf_RULE3_ENABLED">
      <?= opt('yes', $c['RULE3_ENABLED'], 'Enabled') ?>
      <?= opt('no',  $c['RULE3_ENABLED'], 'Disabled') ?>
    </select>
    <div class="hf-help">Promote large files accessed periodically (e.g. 2 opens in 26 h for a regularly used PDF).</div>
  </dd>

  <dt>Long Window Opens</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_LARGE_LONG_MIN_ACCESSES" name="LARGE_LONG_MIN_ACCESSES" min="1"
        value="<?= htmlspecialchars($c['LARGE_LONG_MIN_ACCESSES']) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('LARGE_LONG_MIN_ACCESSES')" title="Reset to default">&#x21ba;</span>
      opens in
      <input type="number" id="hf_LARGE_LONG_WINDOW_HOURS" name="LARGE_LONG_WINDOW_HOURS" min="1"
        value="<?= htmlspecialchars($c['LARGE_LONG_WINDOW_HOURS']) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('LARGE_LONG_WINDOW_HOURS')" title="Reset to default">&#x21ba;</span>
      hours
    </div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Min Reads Filter</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_RULE3_MIN_READS" name="RULE3_MIN_READS" min="0"
        value="<?= htmlspecialchars($c['RULE3_MIN_READS']) ?>" style="width:6em">
      <span class="hf-reset" onclick="hfReset('RULE3_MIN_READS')" title="Reset to default">&#x21ba;</span>
      reads
    </div>
    <div class="hf-help">'Thumbnail filter': skip opens where the read count is between 1 and this value. Opens with <code>0</code> reads (mmap, e.g. PDF viewers) and opens with at least this many reads are always counted. Set to <code>0</code> to disable. <strong>Default: 6.</strong></div>
  </dd>

  <dt>&nbsp;</dt>
  <dd>
    <button type="button" id="hf-apply-btn" onclick="hfSave()" disabled>Apply</button>
  </dd>
</dl>

<br>

<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:6px;">
  <strong>Activity Log</strong>
  <input type="button" value="Clear Log" onclick="hfClearLog()">
</div>
<textarea id="log-box" readonly rows="18"
  style="width:100%;font-family:monospace;font-size:0.8em;resize:vertical;"><?= $log_lines ?: 'No log entries yet.' ?></textarea>

<div class="hf-footer">
  Made with ♥ by
  <img src="https://github.com/kaedinger.png" alt="kaedinger" class="hf-footer-avatar">
  <a href="https://kaedinger.de" target="_blank" rel="noopener">kaedinger</a>
  <span class="hf-footer-sep">·</span>
  <a href="https://github.com/kaedinger/unspin" target="_blank" rel="noopener">GitHub / Issues</a>
  <span class="hf-footer-sep">·</span>
  <a href="https://forums.unraid.net/topic/198091-plugin-unspin-rudimentary-hot-file-tiering-for-unraid/" target="_blank" rel="noopener">Support forum</a>
  <span class="hf-footer-sep">·</span>
  <a href="https://github.com/sponsors/kaedinger" target="_blank" rel="noopener">Sponsor ♥ - THANK YOU</a>
  <span class="hf-footer-sep">·</span>
  <a href="https://www.paypal.com/donate/?hosted_button_id=ASP53VDRXMDYE" target="_blank" rel="noopener">Donate ♥ - THANK YOU</a>
  <span class="hf-footer-version">Unspin<?= " v{$version}" ?></span>
</div>

<script>
var HF_DEFAULTS = <?= json_encode($defaults) ?>;
(function () {
  var EXEC = '/plugins/unspin/include/exec.php';

  var applyBtn = document.getElementById('hf-apply-btn');
  var msgEl    = document.getElementById('hf-message');
  var lb       = document.getElementById('log-box');

  if (lb) lb.scrollTop = lb.scrollHeight;

  document.querySelectorAll('[id^="hf_"]').forEach(function (el) {
    el.addEventListener('change', function () { applyBtn.disabled = false; });
    el.addEventListener('input',  function () { applyBtn.disabled = false; });
  });

  // Live-update the Status column when a share is ticked/unticked, so the
  // displayed promotable/excluded badge reflects the pending state without a reload.
  document.querySelectorAll('input.hf-share-cb').forEach(function (cb) {
    cb.addEventListener('change', function () {
      var row = cb.closest('.hf-share-row');
      if (!row) return;
      var statusCell = row.children[2];
      if (!statusCell) return;
      statusCell.innerHTML = cb.checked
        ? '<span style="color:#4ade80;">promotable</span>'
        : '<span style="color:#facc15;">excluded</span>';
    });
  });

  function showMsg(text, ok) {
    msgEl.innerHTML = '<span style="color:' + (ok ? '#4ade80' : '#f87171') + '">' +
                      text.replace(/</g, '&lt;') + '</span>';
  }

  function hfPost(data, cb) {
    $.post(EXEC, data, cb, 'json').fail(function (xhr) {
      showMsg('Error: ' + xhr.statusText, false);
    });
  }

  // Re-enables toggle button and updates status label.
  // Called on every poll so the button always recovers after a busy state.
  function updateStatus(running, paused, pauseLocks) {
    var el  = document.getElementById('hf-status');
    var btn = document.getElementById('hf-toggle-btn');
    if (el) {
      if (!running) {
        el.textContent = 'Stopped';
        el.style.color = '#f87171';
      } else if (paused) {
        var reasons = (pauseLocks && pauseLocks.length) ? pauseLocks.join(', ') : '';
        el.textContent = reasons ? 'Running (Paused: ' + reasons + ')' : 'Running (Paused)';
        el.style.color = '#facc15';
      } else {
        el.textContent = 'Running';
        el.style.color = '#4ade80';
      }
    }
    if (btn) {
      btn.disabled  = false;
      btn.innerHTML = running ? 'Stop Daemon' : 'Start Daemon';
    }
  }

  function setBusy(btn, text) {
    btn.disabled  = true;
    btn.innerHTML = '<span class="hf-spinner"></span>' + text;
  }

  function updateLog(log) {
    if (!lb || log === undefined) return;
    var atBottom = lb.scrollTop + lb.clientHeight >= lb.scrollHeight - 10;
    lb.value = log || 'No log entries yet.';
    if (atBottom) lb.scrollTop = lb.scrollHeight;
  }

  var fields = ['SERVICE','SCAN_PATHS',
                'SMALL_FILE_THRESHOLD','SMALL_MIN_ACCESSES',
                'LARGE_SHORT_MIN_ACCESSES','LARGE_SHORT_WINDOW_MINS',
                'LARGE_LONG_MIN_ACCESSES','LARGE_LONG_WINDOW_HOURS',
                'EXCLUDE_PATTERNS','DRY_RUN','LOG_LEVEL','PAUSE_ON_RSYNC',
                'RULE1_ENABLED','RULE1_FALLTHROUGH','RULE2_ENABLED','RULE3_ENABLED',
                'RULE3_MIN_READS'];

  window.hfSave = function () {
    var toggleBtn  = document.getElementById('hf-toggle-btn');
    var wasRunning = document.getElementById('hf-status').textContent.trim() === 'Running';
    setBusy(applyBtn, 'Applying\u2026');
    if (toggleBtn && wasRunning) setBusy(toggleBtn, 'Stopping\u2026');
    var data = { action: 'save' };
    fields.forEach(function (f) {
      var el = document.getElementById('hf_' + f);
      if (el) data[f] = el.value;
    });
    // Per-pool fill thresholds (dynamic inputs).
    document.querySelectorAll('input[data-hf-pool="1"]').forEach(function (el) {
      data[el.name] = el.value;
    });
    // Share checkboxes: submit posts hidden 0 + checked 1; we send only the effective value.
    document.querySelectorAll('input.hf-share-cb').forEach(function (el) {
      data[el.name] = el.checked ? '1' : '0';
    });
    hfPost(data, function (r) {
      showMsg(r.message, r.ok);
      if (r.cfg) {
        fields.forEach(function (f) {
          var el = document.getElementById('hf_' + f);
          if (el && r.cfg[f] !== undefined) el.value = r.cfg[f];
        });
        document.querySelectorAll('input[data-hf-pool="1"]').forEach(function (el) {
          if (r.cfg[el.name] !== undefined) el.value = r.cfg[el.name];
        });
      }
      applyBtn.disabled  = true;
      applyBtn.innerHTML = 'Apply';
      // Delayed poll to re-enable toggle button once daemon has restarted/stopped
      setTimeout(function () {
        hfPost({ action: 'poll' }, function (pr) {
          updateStatus(pr.running, pr.paused, pr.pause_locks);
          updateLog(pr.log);
        });
      }, 3000);
    });
  };

  window.hfDaemonToggle = function () {
    var btn     = document.getElementById('hf-toggle-btn');
    var running = document.getElementById('hf-status').textContent.trim() === 'Running';
    setBusy(btn, running ? 'Stopping\u2026' : 'Starting\u2026');
    hfPost({ action: running ? 'stop' : 'start' }, function (r) {
      showMsg(r.message, r.ok);
      setTimeout(function () {
        hfPost({ action: 'poll' }, function (pr) {
          updateStatus(pr.running, pr.paused, pr.pause_locks);
          updateLog(pr.log);
        });
      }, 3000);
    });
  };

  window.hfDetectDisks = function () {
    hfPost({ action: 'detect_disks' }, function (r) {
      showMsg(r.message, r.ok);
      if (r.ok && r.paths) {
        var el = document.getElementById('hf_SCAN_PATHS');
        if (el) { el.value = r.paths; applyBtn.disabled = false; }
      }
    });
  };

  window.hfClearLog = function () {
    hfPost({ action: 'clear_log' }, function (r) {
      showMsg(r.message, r.ok);
      if (lb) lb.value = '';
    });
  };

  window.hfReset = function (key) {
    var el = document.getElementById('hf_' + key);
    if (el) { el.value = HF_DEFAULTS[key]; applyBtn.disabled = false; }
  };

  window.hfToggleHelp = function (dt) {
    var help = null;
    var detached = !!dt.dataset.helpTarget;
    if (detached) {
      help = document.getElementById(dt.dataset.helpTarget);
    } else {
      var dd = dt.nextElementSibling;
      while (dd && dd.tagName !== 'DD') dd = dd.nextElementSibling;
      if (dd) help = dd.querySelector('.hf-help');
    }
    if (!help) return;
    help.classList.toggle('open');
    // Help text rendered far from its click target (e.g. Detected Shares help
    // sits below the share table) — scroll it into view after expansion.
    if (detached && help.classList.contains('open')) {
      setTimeout(function () {
        help.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
      }, 280);
    }
  };

  setInterval(function () {
    hfPost({ action: 'poll' }, function (r) {
      updateStatus(r.running, r.paused, r.pause_locks);
      updateLog(r.log);
    });
  }, 10000);
}());
</script>
