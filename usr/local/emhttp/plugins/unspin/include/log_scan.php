<?php
/* Condense the activity log into a "last accessed files per
 * watched disk" view. Shared between Unspin.php (initial render) 
 * and exec.php (poll updates).
 *
 * Log lines this relies on (see src/unspind.cpp):
 *   - access events (Read/Open/[already hot]/Promoting/[DRY RUN]) end in
 *     " reads=N promoted=yes|no"
 *   - "[skip share] <path>" - share not promotable, no reads/promoted data
 */

const UNSPIN_ACCESS_CAP = 5;
const UNSPIN_SKIP_CAP   = 5;
const UNSPIN_CHUNK_BYTES = 65536;

// Last $n raw lines of the log, read backward in chunks - never loads the
// whole file (replaces the old `array_slice(file($log_file), -200)`, which
// could exceed PHP's memory limit on a large log despite the daemon's own
// size-based trim, since file() materializes every line as a PHP string).
// (Ask me how I know)
//
// $exclude, if set, is a substring whose matching lines are skipped entirely -
// they don't count towards $n and don't appear in the output. Scanning keeps
// going further back until $n surviving lines are found (or start-of-file),
// so a burst of excluded noise (e.g. a flooding [skip share] share) can't
// crowd out the last $n lines that actually matter - it's "keep scanning
// until $n lines pass the filter", not "take the last $n lines, then filter".
function log_tail_lines($log_file, $n, $exclude = null) {
    if (!file_exists($log_file)) return '';
    $fh = fopen($log_file, 'rb');
    if (!$fh) return '';

    $pos      = filesize($log_file);
    $leftover = '';
    $lines    = [];
    $keep     = function ($line) use ($exclude) {
        return $exclude === null || strpos($line, $exclude) === false;
    };

    while ($pos > 0 && count($lines) < $n) {
        $read = min(UNSPIN_CHUNK_BYTES, $pos);
        $pos -= $read;
        fseek($fh, $pos);
        $data     = fread($fh, $read) . $leftover;
        $chunk    = explode("\n", $data);
        $leftover = array_shift($chunk);
        for ($i = count($chunk) - 1; $i >= 0 && count($lines) < $n; $i--) {
            if ($chunk[$i] !== '' && $keep($chunk[$i])) $lines[] = $chunk[$i];
        }
    }
    if (count($lines) < $n && $leftover !== '' && $keep($leftover)) $lines[] = $leftover;
    fclose($fh);

    return implode("\n", array_reverse($lines)) . "\n";
}

// Paths are wrapped in double quotes by the daemon (qpath() in src/unspind.cpp) so
// they can contain spaces/parens - match the quoted string, not \S*.
function log_scan_match_disk($line, $scan_paths) {
    foreach ($scan_paths as $disk) {
        $re = '#"(' . preg_quote($disk, '#') . '(?:\\\\.|[^"\\\\])*)"#';
        if (preg_match($re, $line, $m)) {
            return [$disk, str_replace('\\"', '"', $m[1])];
        }
    }
    return null;
}

// Classify one log line into an access/skip entry for its disk, or null if irrelevant.
function log_scan_classify($line, $scan_paths) {
    $match = log_scan_match_disk($line, $scan_paths);
    if (!$match) return null;
    [$disk, $path] = $match;
    $timestamp = substr($line, 0, 19);

    if (preg_match('/ reads=(\d+) promoted=(yes|no)$/', $line, $m)) {
        return [$disk, [
            'type'      => 'access',
            'path'      => $path,
            'reads'     => (int)$m[1],
            'promoted'  => $m[2] === 'yes',
            'timestamp' => $timestamp,
        ]];
    }
    if (strpos($line, '[skip share] ') !== false) {
        return [$disk, [
            'type'      => 'skip',
            'path'      => $path,
            'timestamp' => $timestamp,
        ]];
    }
    return null;
}

// Keep at most UNSPIN_ACCESS_CAP DISTINCT-path access entries and
// UNSPIN_SKIP_CAP distinct-path skip entries per disk, preserving order
// (most-recent-first). A file read/skipped repeatedly must only occupy one
// slot (the most recent occurrence)
function log_scan_apply_caps($lists) {
    $out = [];
    foreach ($lists as $disk => $entries) {
        $access = 0; $skip = 0; $kept = []; $seen = [];
        foreach ($entries as $e) {
            $key = $e['type'] . '|' . $e['path'];
            if (isset($seen[$key])) continue;
            if ($e['type'] === 'access') {
                if ($access >= UNSPIN_ACCESS_CAP) continue;
                $access++;
            } else {
                if ($skip >= UNSPIN_SKIP_CAP) continue;
                $skip++;
            }
            $seen[$key] = true;
            $kept[] = $e;
        }
        $out[$disk] = $kept;
    }
    return $out;
}

// Backward chunked scan from EOF - never loads the whole file into memory.
// Stops once every disk has UNSPIN_ACCESS_CAP access entries, or start-of-file.
function log_scan_full($log_file, $scan_paths) {
    $lists = [];
    foreach ($scan_paths as $disk) $lists[$disk] = [];

    if (!file_exists($log_file)) return [$lists, 0];
    $fh = fopen($log_file, 'rb');
    if (!$fh) return [$lists, 0];

    $filesize = filesize($log_file);
    $pos      = $filesize;
    $leftover = '';
    // Tracks distinct paths seen per disk
    $access_seen = array_fill_keys($scan_paths, []);
    $skip_seen   = array_fill_keys($scan_paths, []);

    $satisfied = function () use ($scan_paths, &$access_seen) {
        foreach ($scan_paths as $disk)
            if (count($access_seen[$disk]) < UNSPIN_ACCESS_CAP) return false;
        return true;
    };

    $consume = function ($line) use ($scan_paths, &$lists, &$access_seen, &$skip_seen) {
        if ($line === '') return;
        $c = log_scan_classify($line, $scan_paths);
        if (!$c) return;
        [$disk, $entry] = $c;
        if ($entry['type'] === 'access') {
            if (isset($access_seen[$disk][$entry['path']])) return;
            if (count($access_seen[$disk]) >= UNSPIN_ACCESS_CAP) return;
            $access_seen[$disk][$entry['path']] = true;
        } else {
            if (isset($skip_seen[$disk][$entry['path']])) return;
            if (count($skip_seen[$disk]) >= UNSPIN_SKIP_CAP) return;
            $skip_seen[$disk][$entry['path']] = true;
        }
        $lists[$disk][] = $entry;
    };

    while ($pos > 0 && !$satisfied()) {
        $read = min(UNSPIN_CHUNK_BYTES, $pos);
        $pos -= $read;
        fseek($fh, $pos);
        $data     = fread($fh, $read) . $leftover;
        $lines    = explode("\n", $data);
        $leftover = array_shift($lines);
        for ($i = count($lines) - 1; $i >= 0; $i--) {
            $consume($lines[$i]);
            if ($satisfied()) break;
        }
    }
    if (!$satisfied() && $leftover !== '') $consume($leftover);
    fclose($fh);

    return [log_scan_apply_caps($lists), $filesize];
}

// Forward read of only the bytes appended since $from_offset, merged into
// $current_lists. Falls back to a full rescan if the log was trimmed
// (current size smaller than the offset we were given).
function log_scan_incremental($log_file, $from_offset, $scan_paths, $current_lists) {
    if (!file_exists($log_file)) return [log_scan_apply_caps($current_lists), 0];
    $filesize = filesize($log_file);
    // offset 0 means no real checkpoint yet (e.g. first poll after switching Log
    // Level to Debug) - reading forward from byte 0 would fread() the whole file
    // into memory at once. Use the chunked backward scan instead.
    if ($from_offset <= 0 || $filesize < $from_offset) return log_scan_full($log_file, $scan_paths);
    if ($filesize === $from_offset) return [$current_lists, $filesize];

    $fh = fopen($log_file, 'rb');
    if (!$fh) return [log_scan_apply_caps($current_lists), $from_offset];
    fseek($fh, $from_offset);
    $data = fread($fh, $filesize - $from_offset);
    fclose($fh);

    // Only keep complete lines - a trailing fragment without a newline
    // means a write was in-flight; pick it up on the next poll instead.
    $end = strrpos($data, "\n");
    if ($end === false) return [$current_lists, $from_offset];
    $new_offset = $from_offset + $end + 1;
    $lines = explode("\n", substr($data, 0, $end));

    // $lines is oldest-to-newest (file order); collect newest-first so it
    // can be merged ahead of the existing (already newest-first) lists.
    $new_entries = [];
    foreach (array_reverse($lines) as $line) {
        if ($line === '') continue;
        $c = log_scan_classify($line, $scan_paths);
        if (!$c) continue;
        [$disk, $entry] = $c;
        if (!isset($new_entries[$disk])) $new_entries[$disk] = [];
        $new_entries[$disk][] = $entry;
    }

    $lists = $current_lists;
    foreach ($new_entries as $disk => $entries) {
        $lists[$disk] = array_merge($entries, $lists[$disk] ?? []);
    }

    return [log_scan_apply_caps($lists), $new_offset];
}
