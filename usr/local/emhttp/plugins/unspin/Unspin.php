<?php
/* Unspin - Settings page for Unraid
 * Included by Unspin.page
 * All POST actions are handled by include/exec.php via jQuery $.post().
 */

require_once __DIR__ . '/include/cfg.php';

// Insert a space between the numeric part and the unit (e.g. "500MB" → "500 MB")
function fmt_size($v) {
    return preg_replace('/(\d)([KMGT]B?)$/i', '$1 $2', trim($v));
}

// [total, used] bytes for a pool mount, or null. On ZFS, disk_total_space() and
// disk_free_space() only count the top-level dataset's own data (not child
// datasets or snapshots), so ask zfs: "used" of the dataset includes its children.
function pool_usage($mnt) {
    foreach (@file('/proc/self/mounts') ?: [] as $line) {
        $f = explode(' ', $line);
        if (count($f) < 3 || $f[1] !== $mnt || $f[2] !== 'zfs') continue;
        $out = [];
        exec('zfs get -Hp -o value used,available ' . escapeshellarg($f[0]) . ' 2>/dev/null', $out, $rc);
        if ($rc === 0 && count($out) === 2 && is_numeric($out[0]) && is_numeric($out[1]))
            return [(float)$out[0] + (float)$out[1], (float)$out[0]];
        break;
    }
    $total = @disk_total_space($mnt);
    $free  = @disk_free_space($mnt);
    if (!$total || $free === false) return null;
    return [$total, $total - $free];
}

function opt($val, $current, $label) {
    $sel = ($current === $val) ? ' selected' : '';
    return "<option value=\"" . htmlspecialchars($val) . "\"$sel>" . htmlspecialchars($label) . "</option>";
}

$defaults = load_cfg(UNSPIN_DEFAULTS);
$c        = settings();

$plg_file = '/boot/config/plugins/unspin.plg';
$version  = 'unknown';
if (file_exists($plg_file)) {
    $plg = file_get_contents($plg_file);
    if (preg_match('/<!ENTITY\s+version\s+"([^"]+)"/', $plg, $m))
        $version = $m[1];
}

$is_running = daemon_running();
$pause_locks = pause_reasons();
$is_paused = count($pause_locks) > 0;
$pause_label = $is_paused ? ' (Paused: ' . htmlspecialchars(implode(', ', $pause_locks)) . ')' : '';

require_once __DIR__ . '/include/log_scan.php';
$log_lines_raw = log_tail_lines(UNSPIN_LOG, log_tail_count($c));
$log_lines     = htmlspecialchars($log_lines_raw);

$recent_offset = 0;
$recent_lists  = [];
if (is_debug($c)) {
    [$recent_lists, $recent_offset] = log_scan_full(UNSPIN_LOG, scan_paths($c));
}

// Detected shares + excluded set for the Detected Shares table.
$shares         = load_unraid_shares();
$excluded_csv   = $c['EXCLUDED_SHARES'] ?? '';
$excluded_set   = array_flip(array_filter(array_map('trim', explode(',', $excluded_csv))));

// Pools currently referenced by promotable (yes/prefer, not excluded) shares.
$detected_pools = detect_promotable_pools($shares, $excluded_set);

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

/* Width of the settings column, as Unraid's form elements */
:root { --hf-col: 400px; }

/* Text kept on one line and cut at the edge; with an ellipsis where the script
   doesn't shorten it in the middle itself (fitMiddleEllipsis) */
.hf-fold-line, .hf-wake-label, .hf-wake-causes, .hf-pool-mnt, .recent-path, .hf-wake-cause {
  white-space: nowrap;
  overflow: hidden;
}
.hf-fold-line, .hf-wake-label, .hf-wake-causes, .hf-pool-mnt { text-overflow: ellipsis; }

/* Secondary text: times, empty-panel notes, hints */
.hf-muted, .hf-time { color: #888; }

/* Clickable help labels */
dt.hf-has-help {
  cursor: help;
  user-select: none;
}

/* Help text panels - hidden by default */
.hf-help {
  overflow: hidden;
  max-height: 0;
  max-width: var(--hf-col);
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

#hf-toggle-btn, #hf-apply-btn {
  width: var(--hf-col) !important;
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

/* Shared panel border - used by the recent-files panels and the log textarea */
.hf-bordered {
  border: 1px solid rgba(128,128,128,0.35);
  border-radius: 4px;
}

/* Recently Accessed Files panel */
.recent-disk {
  padding: 8px 10px;
  min-width: 0;
}
.recent-disk-title {
  font-family: monospace;
  font-weight: bold;
  margin-bottom: 8px;
}
.recent-table {
  width: 100%;
  table-layout: fixed;
  font-size: 0.85em;
  border-collapse: collapse;
}
.recent-table tr:nth-child(even) {
  background: rgba(128,128,128,0.08);
}
.recent-path { font-family: monospace; }
.recent-col { width: 70px; white-space: nowrap; }
.recent-col-skip { width: 140px; white-space: nowrap; }
#recent-disks.hf-hide-skip tr[data-type="skip"] { display: none; }
.hf-time { width: 100px; white-space: nowrap; text-align: right; }

.recent-disks-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}
@media (max-width: 700px), (orientation: portrait) {
  .recent-disks-grid { grid-template-columns: 1fr; }
}

/* Summary line of a foldable section (promotion statistics, detected shares) */
.hf-fold-line {
  width: var(--hf-col);
  max-width: 100%;
  cursor: pointer;
  user-select: none;
}

/* Promotion statistics */
.hf-stats-grid {
  display: grid;
  grid-template-columns: repeat(4, auto);
  justify-content: space-between;
  width: var(--hf-col);
  max-width: 100%;
  column-gap: 16px;
  row-gap: 2px;
  margin: 8px 0 4px;
}
.hf-stats-grid > span { white-space: nowrap; }
.hf-stats-grid > span:nth-child(-n+4) { opacity: 0.7; }
.hf-stats-sum { font-weight: bold; }

/* Disk wake-ups: a long cause (an SMB user@client) is cut, not widening the grid */
.hf-wake-grid { grid-template-columns: auto auto auto auto minmax(0, 1fr); }
.hf-wake-grid > span:nth-child(5) { opacity: 0.7; }
.hf-wake-causes { font-size: 0.85em; opacity: 0.8; margin-bottom: 8px; }
.hf-wake-cause { width: 30%; }
#hf-wake-cards .recent-path { padding-left: 8px; }
.hf-spin {
  display: inline-block;
  width: 8px;
  height: 8px;
  box-sizing: border-box;
  border: 1px solid #888;
  border-radius: 50%;
  margin-right: 6px;
  vertical-align: middle;
  position: relative;
  top: -1px;
}
.hf-spin.on { background: #4ade80; border-color: #4ade80; }

/* Pool Fill Limits row, kept within the settings column */
.hf-pool-row { width: var(--hf-col); max-width: 100%; margin-bottom: 4px; }
.hf-pool-mnt { flex: 0 0 9em; font-family: monospace; }
.hf-pool-size { margin-left: auto; opacity: 0.7; white-space: nowrap; }
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

<?php $stats = promotion_stats(); ?>
  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Promoted to Cache</dt>
  <dd>
    <div class="hf-fold-line" onclick="hfFold('stats')" title="Files (size) - click for details per pool">
      <span id="hf-fold-caret-stats">&#9656;</span> <span id="hf-stats-line"><?= htmlspecialchars($stats['summary']) ?></span>
    </div>
    <div class="hf-fold-stats" style="display:none;">
      <div id="hf-stats-table"><?= $stats['detail'] ?></div>
      <input type="button" value="Reset Statistics" onclick="hfResetStats()">
    </div>
    <div class="hf-help">Files Unspin moved to the cache pools: today, average per day and in total, summed over all pools. Click the line for the numbers per pool. Dry run promotions are not counted. The average counts every day since the first promotion, including days without any. Updated about once a minute.</div>
  </dd>

<?php $wakeups = disk_wakeups(); ?>
  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Disk Wake-ups</dt>
  <dd>
    <div class="hf-fold-line" onclick="hfFold('wakeups')" title="Click for the numbers per disk">
      <span id="hf-fold-caret-wakeups">&#9656;</span> <span id="hf-wakeups-line"><?= htmlspecialchars($wakeups['summary']) ?></span>
    </div>
    <div class="hf-fold-wakeups" style="display:none;">
      <div id="hf-wakeups-table"><?= $wakeups['detail'] ?></div>
      <input type="button" value="Reset Wake-ups" onclick="hfResetWakeups()">
    </div>
    <div class="hf-help">How often sleeping array disks were woken: today, average per day and in total, and the cause of the last wake-up. Click the line for the numbers per disk, each with its most frequent cause. Which program, container, VM or SMB client woke each disk is shown in <em>Latest Disk Wake-ups</em> further down: the causes with their counts, and the last five wake-ups; hover over one for the full path. The dot before each disk shows whether it is spinning (green) or spun down. A read served from RAM doesn't wake a disk and isn't counted.
      <br><em>Listing a directory</em>, <em>looking up a name</em>, <em>reading attributes</em>: the disk woke for something that opens no file; the program shown was found waiting for the disk. <em>User share (requester not found)</em>: only the user share file system was found waiting, not the program that asked it.
      <br><em>No file opened</em>: the same, but no waiting program was found. The <em>Dynamix Cache Directories</em> plugin prevents many directory listings, but not every name lookup.
      <br><em>No I/O seen</em>: a command was sent to the disk itself, like a spin-up from the Main page or a SMART check.
      <br>Parity is not listed: it wakes when a data disk is written, and that write is listed under the data disk. Pools are not watched.</div>
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
      <?= opt('yes', $c['DRY_RUN'], 'Yes - log decisions only, no promotions') ?>
      <?= opt('no',  $c['DRY_RUN'], 'No - promote files') ?>
    </select>
    <div class="hf-help">Leave enabled until you are satisfied with the promotion decisions in the log. <strong>Default: Yes.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Log Level</dt>
  <dd>
    <select name="LOG_LEVEL" id="hf_LOG_LEVEL">
      <?= opt('info',  $c['LOG_LEVEL'], 'Info') ?>
      <?= opt('debug', $c['LOG_LEVEL'], 'Debug (verbose - logs every access with counts)') ?>
    </select>
    <div class="hf-help">Debug also enables the "5 Recently Accessed Files" panels above the Activity Log, showing per-disk read counts and promotion status. <strong>Default: Info.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Include excluded shares in Debug log</dt>
  <dd>
    <select name="LOG_EXCLUDED_SHARES" id="hf_LOG_EXCLUDED_SHARES">
      <?= opt('no',  $c['LOG_EXCLUDED_SHARES'] ?? 'no', 'No') ?>
      <?= opt('yes', $c['LOG_EXCLUDED_SHARES'] ?? 'no', 'Yes') ?>
    </select>
    <div class="hf-help">On Debug logging, also log every access to excluded/non-cached shares. A noisy excluded share (e.g. a download folder) can otherwise flood the log and crowd out the "Recently Accessed Files" history for other shares. Only enable this temporarily, for instance to reconsider an excluded share for tiering. <strong>Default: No.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Log Max Size</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_LOG_MAX_SIZE_MB" name="LOG_MAX_SIZE_MB" min="1"
        value="<?= htmlspecialchars($c['LOG_MAX_SIZE_MB'] ?? '64') ?>" style="width:6em">
      <span>MB</span>
      <span class="hf-reset" onclick="hfReset('LOG_MAX_SIZE_MB')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">The log file is trimmed once it exceeds this size (see Log Trim Size below). Checked continuously (throttled to once per minute), so the log can't balloon unbounded between checks. <strong>Default: 64.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Log Trim Size</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_LOG_TRIM_SIZE_MB" name="LOG_TRIM_SIZE_MB" min="1"
        value="<?= htmlspecialchars($c['LOG_TRIM_SIZE_MB'] ?? '5') ?>" style="width:6em">
      <span>MB</span>
      <span class="hf-reset" onclick="hfReset('LOG_TRIM_SIZE_MB')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">Once Log Max Size is exceeded, the log is trimmed down to this size. <strong>Default: 5.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Pause on rsync</dt>
  <dd>
    <select name="PAUSE_ON_RSYNC" id="hf_PAUSE_ON_RSYNC">
      <?= opt('yes', $c['PAUSE_ON_RSYNC'], 'Yes') ?>
      <?= opt('no',  $c['PAUSE_ON_RSYNC'], 'No') ?>
    </select>
    <div class="hf-help">Pause monitoring while any <code>rsync</code> process is running on the system. Unraid's Mover always pauses Unspin regardless of this setting. Tools like <em>Unbalanced</em> and <em>Mover Tuning</em> use rsync to move files between array disks - leaving this enabled prevents their activity from inflating access counters and triggering false promotions. <strong>Default: Yes.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Track Disk Wake-ups</dt>
  <dd>
    <select name="TRACK_WAKEUPS" id="hf_TRACK_WAKEUPS">
      <?= opt('yes', $c['TRACK_WAKEUPS'] ?? 'yes', 'Yes') ?>
      <?= opt('no',  $c['TRACK_WAKEUPS'] ?? 'yes', 'No') ?>
    </select>
    <div class="hf-help">Log and count which program, container, VM or SMB client wakes a sleeping array disk, shown under <strong>Disk Wake-ups</strong> above. Works on every array disk Unspin watches, excluded shares included, and keeps running while Unspin is paused. <strong>Default: Yes.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Pool Access Times</dt>
  <dd>
    <select name="ENABLE_POOL_ATIME" id="hf_ENABLE_POOL_ATIME">
      <?= opt('yes', $c['ENABLE_POOL_ATIME'] ?? 'no', 'Yes') ?>
      <?= opt('no',  $c['ENABLE_POOL_ATIME'] ?? 'no', 'No') ?>
    </select>
    <div class="hf-help">Turn access times on for the pools Unspin promotes into, so <em>Mover Tuning</em>'s <strong>Use ATIME</strong> filter moves files back to the array only once they haven't been read for a while. Unraid mounts all pools with <code>noatime</code>, so without this a file's access time never changes. Btrfs/XFS pools are remounted with <code>relatime</code> (re-applied after every array start); on ZFS pools the <code>atime</code> and <code>relatime</code> properties are set to <code>on</code> (ZFS support is untested). Pools that already have access times on are left alone, and switching this off only undoes Unspin's own changes - you can choose to do that right away or at the next array start. <strong>Default: No.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Mount Wait Timeout</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_MOUNT_WAIT_TIMEOUT_MINS" name="MOUNT_WAIT_TIMEOUT_MINS" min="0"
        value="<?= htmlspecialchars($c['MOUNT_WAIT_TIMEOUT_MINS']) ?>" style="width:6em">
      <span>min</span>
      <span class="hf-reset" onclick="hfReset('MOUNT_WAIT_TIMEOUT_MINS')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">On a slow boot the array - especially ZFS pools - can take <strong>20-30 minutes</strong> to mount. Rather than start with nothing to watch, Unspin waits up to this many minutes for each scan path to mount, raising respective Unraid notifications. <code>0</code> disables waiting (skip missing mounts immediately). <strong>Default: 45.</strong></div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Mount Retry Interval</dt>
  <dd>
    <div class="hf-row">
      <input type="number" id="hf_MOUNT_RETRY_INTERVAL_SECS" name="MOUNT_RETRY_INTERVAL_SECS" min="1"
        value="<?= htmlspecialchars($c['MOUNT_RETRY_INTERVAL_SECS']) ?>" style="width:6em">
      <span>sec</span>
      <span class="hf-reset" onclick="hfReset('MOUNT_RETRY_INTERVAL_SECS')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="hf-help">How often to re-check whether a pending scan path has mounted, while waiting (see Mount Wait Timeout). <strong>Default: 30.</strong></div>
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
    <div class="hf-fold-line" onclick="hfFold('shares')">
      <span id="hf-fold-caret-shares">&#9656;</span> <span id="hf-shares-summary"></span>
    </div>
    <div class="hf-share-row hf-share-head hf-fold-shares" style="display:none;">
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
    <dt class="hf-share-name hf-fold-shares" style="display:none;"><code><?= htmlspecialchars($sname) ?></code></dt>
    <dd class="hf-fold-shares" style="display:none;">
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
    <div class="hf-help" id="hf-shares-help">Untick a share to prevent Unspin from promoting its files, even when <code>shareUseCache</code> is <code>yes</code> or <code>prefer</code>. Shares with <code>no</code> or <code>only</code> are always skipped and cannot be toggled. Don't forget to click <strong>Apply</strong> below!</div>
  </dd>

  <dt class="hf-has-help" onclick="hfToggleHelp(this)">Pool Fill Limits</dt>
  <dd>
  <?php if (empty($detected_pools)): ?>
    <div style="opacity:0.6;">No promotable pools detected - enable at least one share above.</div>
  <?php else: foreach ($detected_pools as $pool):
      $key      = 'MAX_FILL_PERCENT_' . $pool;
      $val      = isset($c[$key]) && is_numeric($c[$key]) ? intval($c[$key]) : $legacy_fill;
      $mnt      = '/mnt/' . $pool;
      $usage    = pool_usage($mnt);
      $size_str = '';
      if ($usage) {
          [$total, $used] = $usage;
          $pct     = $total > 0 ? round($used * 100 / $total) : 0;
          $size_str = fmt_bytes($total) . ', ' . $pct . '% used';
      }
  ?>
    <div class="hf-row hf-pool-row">
      <span class="hf-pool-mnt" title="<?= htmlspecialchars($mnt) ?>"><?= htmlspecialchars($mnt) ?></span>
      <input type="number" id="hf_<?= htmlspecialchars($key) ?>" name="<?= htmlspecialchars($key) ?>"
             min="10" max="99" value="<?= $val ?>" style="width:5em"
             data-hf-pool="1">
      <span>%</span>
      <span class="hf-reset" onclick="document.getElementById('hf_<?= htmlspecialchars($key) ?>').value='80';document.getElementById('hf_<?= htmlspecialchars($key) ?>').dispatchEvent(new Event('input',{bubbles:true}));" title="Reset to default (80)">&#x21ba;</span>
      <?php if ($size_str): ?><span class="hf-pool-size"><?= htmlspecialchars($size_str) ?></span><?php endif; ?>
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

<div id="hf-wake-panel" style="margin-bottom:24px;<?= ($c['TRACK_WAKEUPS'] ?? 'yes') === 'no' ? 'display:none;' : '' ?>">
  <strong class="hf-fold-line" style="display:block;margin-bottom:10px;" onclick="hfFold('wakepanel')"><span id="hf-fold-caret-wakepanel">&#9662;</span> Latest Disk Wake-ups</strong>
  <div id="hf-wake-cards" class="hf-fold-wakepanel recent-disks-grid"><?= $wakeups['panel'] ?></div>
</div>

<div id="recent-panel" style="margin-bottom:12px;<?= is_debug($c) ? '' : 'display:none;' ?>">
  <strong style="display:block;margin-bottom:10px;">5 Recently Accessed Files</strong>
  <div id="recent-disks" class="recent-disks-grid"><?= recent_panel($recent_lists) ?></div>
</div>

<label style="display:block;margin-bottom:8px;">
  <input type="checkbox" id="hf-exclude-skipped"> Exclude skipped shares
  <span class="hf-muted" style="font-size:0.85em;margin-left:6px;">(filters this view only - won't show anything unless "Include excluded shares in Debug log" above is also set to Yes)</span>
</label>

<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:6px;">
  <strong style="margin-bottom: -26px;">Activity Log</strong>
  <input type="button" value="Clear Log" onclick="hfClearLog()">
</div>
<textarea id="log-box" class="hf-bordered" readonly rows="18"
  style="width:100%;font-family:monospace;font-size:0.8em;resize:vertical;box-sizing:border-box;appearance:none;-webkit-appearance:none;"><?= $log_lines ?: 'No log entries yet.' ?></textarea>

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
var HF_ATIME_SAVED = <?= json_encode($c['ENABLE_POOL_ATIME'] ?? 'no') ?>;
var HF_RECENT   = { offset: <?= json_encode($recent_offset) ?>, lists: <?= json_encode($recent_lists) ?> };
(function () {
  var EXEC = '/plugins/unspin/include/exec.php';

  var applyBtn  = document.getElementById('hf-apply-btn');
  var msgEl     = document.getElementById('hf-message');
  var lb        = document.getElementById('log-box');
  var skipCb    = document.getElementById('hf-exclude-skipped');
  var rawLog    = <?= json_encode($log_lines_raw) ?>;

  if (lb) lb.scrollTop = lb.scrollHeight;

  // Hidden element used purely to measure text width in the exact font the
  // path cells use, so truncation matches the actual available space instead
  // of guessing a fixed character count.
  var measureSpan = document.createElement('span');
  measureSpan.style.position   = 'absolute';
  measureSpan.style.visibility = 'hidden';
  measureSpan.style.whiteSpace = 'pre';
  measureSpan.style.left       = '-9999px';
  document.body.appendChild(measureSpan);

  function textWidth(str, font) {
    measureSpan.style.font = font;
    measureSpan.textContent = str;
    return measureSpan.offsetWidth;
  }

  function fitMiddleEllipsis(str, availablePx, font) {
    if (textWidth(str, font) <= availablePx) return str;
    var lo = 0, hi = str.length, best = '…';
    while (lo <= hi) {
      var mid = Math.floor((lo + hi) / 2);
      var head = Math.ceil(mid / 2);
      var tail = mid - head;
      var candidate = str.slice(0, head) + '…' + str.slice(str.length - tail);
      if (textWidth(candidate, font) <= availablePx) { best = candidate; lo = mid + 1; }
      else hi = mid - 1;
    }
    return best;
  }

  // Re-fit every path cell's displayed text to its actual rendered width.
  function fitRecentPaths() {
    document.querySelectorAll('#recent-disks td.recent-path[data-full], #hf-wake-cards td[data-full]').forEach(function (td) {
      if (!td.clientWidth) return;   // folded or hidden: fitted when shown
      var full = td.getAttribute('data-full');
      var font = getComputedStyle(td).font;
      td.textContent = fitMiddleEllipsis(full, td.clientWidth - 4, font);
    });
  }

  // The cards come rendered from the server (recent_panel() in log_scan.php).
  function renderRecent(html) {
    var container = document.getElementById('recent-disks');
    if (!container) return;
    container.innerHTML = html;
    fitRecentPaths();
  }

  // Skipped rows are hidden by a class on the container, so a re-render keeps it.
  function applySkipClass() {
    var container = document.getElementById('recent-disks');
    if (container) container.classList.toggle('hf-hide-skip', !!(skipCb && skipCb.checked));
  }
  applySkipClass();

  function applySkipFilter() {
    applySkipClass();
    // The log line quota is now filled server-side (see log_tail_lines()), so
    // toggling the filter needs a fresh fetch, not just a re-render of what's
    // already on screen - otherwise a flooded tail can leave nothing to show.
    hfPoll();
  }

  if (skipCb) skipCb.addEventListener('change', applySkipFilter);

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
      updateSharesSummary();
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

  function pollData() {
    return { action: 'poll', log_offset: HF_RECENT.offset, log_lists: JSON.stringify(HF_RECENT.lists),
             exclude_skip: (skipCb && skipCb.checked) ? '1' : '0' };
  }

  // Fetch the current state and refresh the status, the log and the panels.
  function hfPoll() {
    hfPost(pollData(), function (r) {
      updateStatus(r.running, r.paused, r.pause_locks);
      applyPollResult(r);
    });
  }

  // A yes/no question in Unraid's dialog, or the browser's if it's missing;
  // fallbackText replaces the text there, for labels other than OK/Cancel.
  function hfConfirm(title, text, okLabel, cancelLabel, cb, fallbackText) {
    if (typeof swal === 'function') {
      swal({ title: title, text: text, type: 'warning', showCancelButton: true,
             confirmButtonText: okLabel, cancelButtonText: cancelLabel }, cb);
    } else {
      cb(confirm(fallbackText || text));
    }
  }

  // Shared handling for every poll response: refresh the log (the server already
  // applied the skip-share filter, scanning as far back as needed to fill the
  // line quota - see log_tail_lines()) and the condensed recent-files panel.
  function applyPollResult(r) {
    rawLog = r.log || '';
    updateLog(rawLog);
    if (r.recent) {
      HF_RECENT = r.recent;
      renderRecent(HF_RECENT.html);
    }
    if (r.stats)   applyFoldData('stats', r.stats);
    if (r.wakeups) applyFoldData('wakeups', r.wakeups);
  }

  // Statistics sections (promotions, wake-ups): summary, header and detail table.
  var HF_FOLD = { stats: <?= json_encode($stats) ?>, wakeups: <?= json_encode($wakeups) ?> };
  function applyFoldData(name, data) {
    HF_FOLD[name] = data;
    document.getElementById('hf-' + name + '-table').innerHTML = data.detail;
    if (data.panel !== undefined) {
      document.getElementById('hf-wake-cards').innerHTML = data.panel;
      fitRecentPaths();
    }
    renderFoldLine(name);
  }
  function renderStatsLines() { renderFoldLine('stats'); renderFoldLine('wakeups'); }

  // Statistics header: the summary while folded, the header text while open.
  // A summary too wide for the column is shrunk in steps down to 75%; past
  // that it's cut off with an ellipsis and shown in full as a tooltip.
  function renderFoldLine(name) {
    var line = document.getElementById('hf-' + name + '-line');
    var box  = line.parentNode;
    var open = document.querySelector('.hf-fold-' + name).style.display !== 'none';
    line.textContent = open ? HF_FOLD[name].open : HF_FOLD[name].summary;
    line.style.fontSize = '';
    line.removeAttribute('title');
    // Measures the text itself, with a 1px margin: scrollWidth is rounded, so an
    // overflow under 1px passed as fitting while the ellipsis still hid the end.
    // The clipping is off while measuring, so the ellipsis can't shorten the text.
    box.style.overflow = 'visible';
    var tooWide = function () {
      return line.getBoundingClientRect().right > box.getBoundingClientRect().right - 1;
    };
    for (var pct = 95; pct >= 75 && tooWide(); pct -= 5) {
      line.style.fontSize = pct + '%';
    }
    if (tooWide()) line.title = line.textContent;
    box.style.overflow = '';
  }

  // Foldable sections: the summary line toggles every element with class
  // hf-fold-<name>. The state is remembered per browser.
  window.hfFold = function (name, open) {
    var items = document.querySelectorAll('.hf-fold-' + name);
    if (!items.length) return;
    if (open === undefined) open = items[0].style.display === 'none';
    items.forEach(function (el) { el.style.display = open ? '' : 'none'; });
    document.getElementById('hf-fold-caret-' + name).innerHTML = open ? '&#9662;' : '&#9656;';
    if (HF_FOLD[name]) renderFoldLine(name);
    if (open) fitRecentPaths();
    try { localStorage.setItem('unspin-fold-' + name, open ? '1' : '0'); } catch (e) {}
  };

  ['stats', 'wakeups', 'shares'].forEach(function (name) {
    var open = false;
    try { open = localStorage.getItem('unspin-fold-' + name) === '1'; } catch (e) {}
    if (open) hfFold(name, true);
  });
  try { if (localStorage.getItem('unspin-fold-wakepanel') === '0') hfFold('wakepanel', false); } catch (e) {}   // open by default

  function updateSharesSummary() {
    var el = document.getElementById('hf-shares-summary');
    if (!el) return;
    var total    = document.querySelectorAll('dt.hf-share-name').length;
    var cbs      = document.querySelectorAll('input.hf-share-cb');
    var promoted = document.querySelectorAll('input.hf-share-cb:checked').length;
    el.textContent = total + (total === 1 ? ' share: ' : ' shares: ') + promoted + ' promotable, ' +
                     (cbs.length - promoted) + ' excluded, ' + (total - cbs.length) + ' untouched';
  }
  updateSharesSummary();

  // Reset button of a statistics section: action reset_NAME answers with the
  // section's new data under NAME.
  function hfResetFold(name, title, text) {
    hfConfirm(title, text, 'Reset', 'Cancel', function (ok) {
      if (!ok) return;
      hfPost({ action: 'reset_' + name }, function (r) {
        showMsg(r.message, r.ok);
        if (r[name]) applyFoldData(name, r[name]);
      });
    });
  }
  window.hfResetStats = function () {
    hfResetFold('stats', 'Reset Statistics', 'Reset the promotion statistics for all pools? This cannot be undone.');
  };
  window.hfResetWakeups = function () {
    hfResetFold('wakeups', 'Reset Wake-ups', 'Reset the disk wake-up statistics for all disks? This cannot be undone.');
  };

  var fields = ['SERVICE','SCAN_PATHS',
                'SMALL_FILE_THRESHOLD','SMALL_MIN_ACCESSES',
                'LARGE_SHORT_MIN_ACCESSES','LARGE_SHORT_WINDOW_MINS',
                'LARGE_LONG_MIN_ACCESSES','LARGE_LONG_WINDOW_HOURS',
                'EXCLUDE_PATTERNS','DRY_RUN','LOG_LEVEL','LOG_EXCLUDED_SHARES',
                'LOG_MAX_SIZE_MB','LOG_TRIM_SIZE_MB','PAUSE_ON_RSYNC','TRACK_WAKEUPS','ENABLE_POOL_ATIME',
                'MOUNT_WAIT_TIMEOUT_MINS','MOUNT_RETRY_INTERVAL_SECS',
                'RULE1_ENABLED','RULE1_FALLTHROUGH','RULE2_ENABLED','RULE3_ENABLED',
                'RULE3_MIN_READS'];

  // Switching Pool Access Times off: undo Unspin's changes now or at the next array start?
  function hfAskAtimeOff(cb) {
    var text = 'Turn access times off on the pools Unspin enabled them on now, or at the next array start?';
    hfConfirm('Pool Access Times', text, 'Now', 'At next array start',
              function (now) { cb(now ? 'now' : 'later'); },
              text + '\n\nOK = now, Cancel = at the next array start');
  }

  window.hfSave = function (atimeOff) {
    var atimeEl = document.getElementById('hf_ENABLE_POOL_ATIME');
    if (atimeOff === undefined && atimeEl && HF_ATIME_SAVED === 'yes' && atimeEl.value === 'no') {
      hfAskAtimeOff(hfSave);
      return;
    }
    var toggleBtn  = document.getElementById('hf-toggle-btn');
    var wasRunning = document.getElementById('hf-status').textContent.trim() === 'Running';
    setBusy(applyBtn, 'Applying\u2026');
    if (toggleBtn && wasRunning) setBusy(toggleBtn, 'Stopping\u2026');
    var data = { action: 'save' };
    if (atimeOff) data.ATIME_OFF = atimeOff;
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
        if (r.cfg.ENABLE_POOL_ATIME !== undefined) HF_ATIME_SAVED = r.cfg.ENABLE_POOL_ATIME;
        var panel = document.getElementById('recent-panel');
        if (panel) panel.style.display = r.cfg.LOG_LEVEL === 'debug' ? '' : 'none';
        var wakePanel = document.getElementById('hf-wake-panel');
        if (wakePanel) wakePanel.style.display = r.cfg.TRACK_WAKEUPS === 'no' ? 'none' : '';
      }
      applyBtn.disabled  = true;
      applyBtn.innerHTML = 'Apply';
      // Delayed poll to re-enable toggle button once daemon has restarted/stopped
      setTimeout(hfPoll, 3000);
    });
  };

  window.hfDaemonToggle = function () {
    var btn     = document.getElementById('hf-toggle-btn');
    var running = document.getElementById('hf-status').textContent.trim() === 'Running';
    setBusy(btn, running ? 'Stopping\u2026' : 'Starting\u2026');
    hfPost({ action: running ? 'stop' : 'start' }, function (r) {
      showMsg(r.message, r.ok);
      setTimeout(hfPoll, 3000);
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

  setInterval(hfPoll, 10000);

  fitRecentPaths();
  renderStatsLines();
  var resizeTimer;
  window.addEventListener('resize', function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () { fitRecentPaths(); renderStatsLines(); }, 150);
  });
}());
</script>
