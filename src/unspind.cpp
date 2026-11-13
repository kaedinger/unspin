// unspind.cpp - Unspin hotfile tiering daemon for Unraid
// Monitors file accesses via fanotify and promotes hot files to the cache.
//
// Compile: g++ -O2 -Wall -o unspind unspind.cpp
// Requires: Linux kernel 2.6.37+ (fanotify), CAP_SYS_ADMIN (run as root)

#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/fanotify.h>
#include <sys/file.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t     COPY_CHUNK_BYTES     = 1u << 20;           // 1 MiB per sendfile call
static constexpr off_t      COPY_DROP_BYTES      = 32 << 20;           // copied data leaves RAM every 32 MiB
static constexpr int        CLEANUP_INTERVAL_SEC = 3600;               // 1 h between access-map cleanups
static constexpr int        PERIODIC_CHECK_INTERVAL_SEC = 60;          // new dataset mounts, pool access times, stale pending promotions
static constexpr int        POLL_TIMEOUT_MS      = 5000;               // fanotify poll wait
static constexpr int        WAKE_POLL_MS         = 250;                // poll wait while a disk wake-up may be under way
static constexpr size_t     FAN_BUF_BYTES        = 65536;              // fanotify read buffer
static constexpr int        MOVER_CACHE_SEC      = 5;                  // how long to cache mover-active result
static constexpr int        PAUSE_DIR_CACHE_SEC  = 2;                  // how long to cache the pause-lock-directory scan
static constexpr int        PENDING_PROMOTE_STALE_SEC = 10 * 60;       // force a stuck deferred promotion through after this long with zero events
static constexpr int        HELD_RETRY_SEC            = 5;             // a file found open elsewhere is checked again at most this often
static constexpr uint64_t   FAN_WATCH_MASK       = FAN_ACCESS | FAN_OPEN | FAN_CLOSE_NOWRITE
                                                 | FAN_CLOSE_WRITE | FAN_MODIFY;
static constexpr int64_t    KB                   = 1024;
static constexpr int64_t    MB                   = 1024 * KB;
static constexpr int64_t    GB                   = 1024 * MB;
static constexpr int64_t    TB                   = 1024 * GB;
static constexpr int64_t    SMALL_F_THRESHOLD    = 10 * MB;
static constexpr double     DEFAULT_MAX_FILL_PERCENT = 80.0;
static constexpr double     LOG_LOCATION_FULL_PERCENT = 95.0;          // trim log immediately if its filesystem is this full
static constexpr const char RULE1[]              = "rule 1 - small";
static constexpr const char RULE2[]              = "rule 2 - big, reads";
static constexpr const char RULE3[]              = "rule 3 - big, opens";
static constexpr const char DEFAULT_CFG_FILE[]   = "/usr/local/emhttp/plugins/unspin/unspin.cfg.default";
static constexpr const char TRIM_CHARS[]         = " \t\r\n";
// Unraid's per-disk / per-pool mount root. All disks and pools live under this prefix.
static const     std::string MNT_PREFIX          = "/mnt/";
// Share mount point - fanotify is ambiguous/unreliable here, so rejected in SCAN_PATHS validation.
static const     std::string SHARE_MNT           = "/mnt/user";
// Directory of named pause locks - rc.unspin pause/unpause just touch/rm a file here.
// The daemon polls this directory itself (see pause_dir_active()) rather than trusting
// rc.unspin to signal the exact 0->1 / 1->0 transition, which isn't atomic across
// concurrent rc.unspin invocations (e.g. two cron'd backup scripts pausing back to back).
static const     std::string PAUSE_DIR           = "/var/run/unspind.pause.d";

// ---------------------------------------------------------------------------
// Config & globals
// ---------------------------------------------------------------------------

static std::string _cfg_file = "/boot/config/plugins/unspin/unspin.cfg";
static volatile sig_atomic_t _quit   = 0;
static volatile sig_atomic_t _reload = 0;
static volatile sig_atomic_t _paused = 0;

struct Config {
    std::vector<std::string> scan_paths                = {"/mnt/disk1","/mnt/disk2"};
    int64_t                  small_file_threshold      = SMALL_F_THRESHOLD;
    int                      small_min_accesses        = 1;
    int                      large_short_min_accesses  = 100;
    int                      large_short_window_mins   = 5;
    int                      large_long_min_accesses   = 3;
    int                      large_long_window_hours   = 24;
    double                   default_max_fill_percent  = DEFAULT_MAX_FILL_PERCENT;
    bool                     dry_run                   = false;
    bool                     debug                     = false;
    bool                     rule1_enabled             = true;
    bool                     rule1_fallthrough         = true;  // when rule1 is off, evaluate small files via rules 2+3
    bool                     rule2_enabled             = true;
    bool                     rule3_enabled             = true;
    int                      rule3_min_reads           = 6;     // thumbnail filter: skip opens with 1..N-1 reads (0=disabled)
    bool                     pause_on_rsync            = true;  // pause event counting while any rsync process is running
    bool                     enable_pool_atime         = false; // turn access times on for target pools (Mover Tuning ATIME)
    bool                     track_wakeups             = true;  // log and count which process wakes a sleeping disk
    std::string              log_file                  = "/var/log/unspin.log";
    long                     log_max_size_mb           = 64;    // trim log once it exceeds this size
    long                     log_trim_size_mb          = 5;     // trim log down to this size
    bool                     log_excluded_shares       = false; // debug-log [skip share] events (off by default - prevent log flooding)
    int                      mount_wait_timeout_mins   = 45;    // timeout scan paths to mount (0 = don't wait)
    int                      mount_retry_interval_secs = 30;    // seconds between mount re-checks while waiting
    std::vector<std::string> exclude_patterns;
};

static Config _cfg;

// Mount points already fanotify-marked one level under a scan path (see
// mark_submounts() below). Rebuilt from scratch each time init_fanotify()
// gets a fresh fd; grown incrementally by the periodic rescan in main().
static std::unordered_set<std::string> _marked_submounts;
static time_t _last_periodic_check = 0;

enum class EvType { Read, Open, CloseNoWrite, CloseWrite, Modify };

struct PromoteDecision {
    bool        promote = false;
    std::string rule;
    std::string reason;
};

struct AccessRecord {
    std::deque<time_t> read_timestamps; // FAN_ACCESS timestamps within longest window
    std::deque<time_t> open_timestamps; // FAN_OPEN timestamps within longest window
    int                total_reads  = 0;
    time_t             last_event   = 0;
    int                open_reads      = -1;  // reads since last FAN_OPEN, -1 if no open pending
    int                open_count      = 0;   // number of currently open file descriptors
    bool               pending_promote = false;
    PromoteDecision    pending_dec;
    bool               promoted        = false;
    bool               has_write_open  = false; // FAN_MODIFY seen while file is open
    bool               held_elsewhere  = false; // a promotion found it open (open_count can drift, GH #10); waits for a close
    time_t             held_at         = 0;     // when that was found
    std::string        held_by;                 // that holder, logged once at INFO
    time_t             asleep_logged   = 0;     // sleep period (spundown_at) of the last "not waking it" INFO line
};

struct ShareInfo {
    std::string use_cache;   // "yes" | "prefer" | "no" | "only"
    std::string cache_pool;  // primary cache pool name from shareCachePool
    bool        excluded = false; // user unticked in UI (EXCLUDED_SHARES)
};

struct PoolInfo {
    double max_fill_percent = DEFAULT_MAX_FILL_PERCENT;
};

static std::unordered_map<std::string, AccessRecord> _access_map;
static std::unordered_map<std::string, ShareInfo>    _shares;  // share name -> info
static std::unordered_map<std::string, PoolInfo>     _pools;   // pool name  -> info
static time_t      _last_cleanup     = 0;
static time_t      _last_log_size_check = 0;
static std::unordered_map<std::string, std::string> _last_pool_full_msg; // per-pool suppression
static std::string _transfer_cached;          // "Mover", "rsync" or "" (none running)
static time_t      _transfers_checked = 0;
static bool        _pause_dir_cached  = false;
static time_t      _pause_dir_checked = 0;
static uid_t       _nobody_uid       = 99;   // Unraid defaults; updated by init_nobody()
static gid_t       _nobody_gid       = 100;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static std::ofstream _log_stream;
static constexpr int LOG_SIZE_CHECK_INTERVAL_SEC = 60;

static void trim_log(); // defined below; checks/trims the log by size
static double disk_fill_percent(const std::string& path); // defined below in Disk utilities

// Runs at most once per LOG_SIZE_CHECK_INTERVAL_SEC. Called both from vlog()
// (so normal logging keeps the log trimmed) and from the main loop (so it
// still runs on schedule while paused, when little or nothing gets logged).
static void check_log_size() {
    auto now = time(nullptr);
    if (now - _last_log_size_check < LOG_SIZE_CHECK_INTERVAL_SEC) return;
    _last_log_size_check = now;
    trim_log();
}

static bool   _log_active     = false; // open_log() was called, so the log file is wanted
static time_t _log_reopen_at  = 0;     // last reopen attempt after a failed write
static bool   _log_full_handled = false; // trimmed/warned once while the log location is full

// A write that ran out of space can leave a partial last line.
static bool log_ends_mid_line() {
    std::ifstream in(_cfg.log_file, std::ios::binary | std::ios::ate);
    char c;
    return in && in.tellg() > 0 && in.seekg(-1, std::ios::end) && in.get(c) && c != '\n';
}

// Opens the log for appending; the next line starts on a fresh line.
static void open_log() {
    _log_active = true;
    _log_stream.open(_cfg.log_file, std::ios::app);
    if (_log_stream.is_open() && log_ends_mid_line()) _log_stream << '\n';
}

static void vlog(const char* level, const std::string& msg) {
    auto now = time(nullptr);
    // After a failed write (e.g. the log location was full) the stream stays
    // in a failed state and drops everything; reopen it, at most once a second.
    // close() writes out the line that failed, if there is room now, so end a
    // partial line before that.
    if (_log_active && (!_log_stream.is_open() || !_log_stream.good()) && now != _log_reopen_at) {
        _log_reopen_at = now;
        if (log_ends_mid_line()) { std::ofstream nl(_cfg.log_file, std::ios::app); nl << '\n'; }
        _log_stream.close();
        _log_stream.clear();
        open_log();
    }
    char ts[32];
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    std::ostream& out = _log_stream.is_open() ? _log_stream : std::cout;
    out << ts << " [" << level << "] " << msg << "\n";
    out.flush();

    check_log_size();
}

static void log_info (const std::string& m) { vlog("INFO ", m); }
static void log_warn (const std::string& m) { vlog("WARN ", m); }
static void log_err  (const std::string& m) { vlog("ERROR", m); }
static void log_debug(const std::string& m) { if (_cfg.debug) vlog("DEBUG", m); }

// Trailing tag for the "recently accessed" panel
static std::string access_tag(int total_reads, bool promoted) {
    return " reads=" + std::to_string(total_reads) + " promoted=" + (promoted ? "yes" : "no");
}

// Quote for paths containing spaces. This is a contract with the log parser
// in Unspin.php to display recently accessed files!
static std::string qpath(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 2);
    out += '"';
    for (char c : path) { if (c == '"') out += '\\'; out += c; }
    out += '"';
    return out;
}

static std::string human_size(int64_t bytes);

// The biggest file in a directory and its direct subdirectories, as
// "name (size)", to point at whatever filled the log location.
static std::string biggest_file_in(const std::string& dir) {
    std::string best;
    off_t best_size = -1;
    std::vector<std::string> dirs{dir};
    for (size_t i = 0; i < dirs.size(); ++i) {
        DIR* d = opendir(dirs[i].c_str());
        if (!d) continue;
        while (auto* e = readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            auto full = dirs[i] + "/" + name;
            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode) && i == 0) dirs.push_back(full);
            else if (S_ISREG(st.st_mode) && st.st_size > best_size) { best_size = st.st_size; best = full; }
        }
        closedir(d);
    }
    return best.empty() ? "" : best + " (" + human_size(best_size) + ")";
}

static void rewrite_log_tail(long trim_bytes, bool location_full);

static void trim_log() {
    const auto& path = _cfg.log_file;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    const long max_bytes = _cfg.log_max_size_mb * 1024L * 1024L;
    const bool over_size  = (long)st.st_size > max_bytes;
    const double fill_percent = disk_fill_percent(path);
    const bool location_full = fill_percent >= LOG_LOCATION_FULL_PERCENT;
    if (!location_full) _log_full_handled = false;
    const long trim_bytes = _cfg.log_trim_size_mb * 1024L * 1024L;

    // A full location gets one trim (if our log is over the trim size) and one
    // warning, then nothing until it drops below the limit again: other logs
    // filled it and take whatever we free, so trimming again only churns.
    const bool newly_full = location_full && !_log_full_handled;
    if (newly_full) _log_full_handled = true;
    if (over_size || (newly_full && (long)st.st_size > trim_bytes))
        rewrite_log_tail(trim_bytes, location_full);

    if (newly_full) {
        auto dir = path.substr(0, path.rfind('/'));
        if (dir.empty()) dir = "/";
        auto biggest = biggest_file_in(dir);
        log_warn("Log location " + dir + " is " + std::to_string((int)fill_percent) +
                 "% full, filled by other programs" +
                 (biggest.empty() ? "" : " - biggest file: " + biggest) +
                 ". Unspin's log stops once it is completely full and resumes when space is freed there.");
    }
}

// Keeps whole lines from the end of the log, up to trim_bytes.
static void rewrite_log_tail(long trim_bytes, bool location_full) {
    const auto& path = _cfg.log_file;
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        if (!in.is_open()) return;
        std::string line;
        while (std::getline(in, line)) lines.push_back(std::move(line));
    }

    long kept_bytes = 0;
    auto start = lines.size();
    while (start > 0) {
        long line_bytes = (long)lines[start - 1].size() + 1; // +1 for newline
        if (kept_bytes + line_bytes > trim_bytes) break;
        kept_bytes += line_bytes;
        --start;
    }

    _log_stream.close();
    {
        std::ofstream out(path, std::ios::trunc);
        for (auto i = start; i < lines.size(); ++i)
            out << lines[i] << '\n';
    }
    open_log();
    log_info("Log trimmed to " + std::to_string(_cfg.log_trim_size_mb) + " MB" +
              (location_full ? " (log location was nearly full)" : "") + ".");
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    auto l = s.find_first_not_of(TRIM_CHARS);
    auto r = s.find_last_not_of(TRIM_CHARS);
    return (l == std::string::npos) ? "" : s.substr(l, r - l + 1);
}

static int64_t parse_size(const std::string& s) {
    if (s.empty()) return SMALL_F_THRESHOLD;

    size_t i = 0;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;

    auto num = std::stod(s.substr(0, i));
    auto suf = trim(s.substr(i));
    for (char& c : suf) c = (char)toupper((unsigned char)c);

    if (suf == "KB" || suf == "K") return (int64_t)(num * KB);
    if (suf == "MB" || suf == "M") return (int64_t)(num * MB);
    if (suf == "GB" || suf == "G") return (int64_t)(num * GB);
    if (suf == "TB" || suf == "T") return (int64_t)(num * TB);
    return (int64_t)num;
}

static std::string human_size(int64_t bytes) {
    char buf[32];
    if      (bytes >= TB) snprintf(buf, sizeof(buf), "%.1f TB", (double)bytes / TB);
    else if (bytes >= GB) snprintf(buf, sizeof(buf), "%.1f GB", (double)bytes / GB);
    else if (bytes >= MB) snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / MB);
    else                  snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / KB);
    return buf;
}

static void init_nobody() {
    struct passwd* pw = getpwnam("nobody");
    if (pw) {
        _nobody_uid = pw->pw_uid;
        _nobody_gid = pw->pw_gid;
    } else {
        log_err("getpwnam(\"nobody\") failed - falling back to defaults");
    }
    log_info("Using nobody uid=" + std::to_string(_nobody_uid) + " gid=" + std::to_string(_nobody_gid));
}

static void mkdir_p(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            auto sub = path.substr(0, i);
            if (sub.empty()) continue;
            if (mkdir(sub.c_str(), 0777) == 0) {
                if (chown(sub.c_str(), _nobody_uid, _nobody_gid) != 0)
                    log_err("chown(" + sub + "): " + strerror(errno));
            } else if (errno != EEXIST) {
                log_err("mkdir(" + sub + "): " + strerror(errno));
            }
        }
    }
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) result.push_back(tok);
    }
    return result;
}

// Extract share name from a path: /mnt/<pool>/<share>/... -> <share>
static std::string path_share_name(const std::string& path) {
    if (path.rfind(MNT_PREFIX, 0) != 0) return "";
    auto pool_end = path.find('/', MNT_PREFIX.size());
    if (pool_end == std::string::npos) return "";
    auto share_end = path.find('/', pool_end + 1);
    return path.substr(pool_end + 1,
        share_end == std::string::npos ? std::string::npos : share_end - pool_end - 1);
}

// defined in the config section
static void parse_cfg_into(const std::string& path,
                           std::unordered_map<std::string, std::string>& raw);

// Load /boot/config/shares/*.cfg into _shares
static void load_share_settings() {
    _shares.clear();
    const std::string dir = "/boot/config/shares";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        auto fname = std::string(ent->d_name);
        if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".cfg")
            continue;

        auto share = fname.substr(0, fname.size() - 4);
        std::unordered_map<std::string, std::string> raw;
        parse_cfg_into(dir + "/" + fname, raw);
        if (raw.empty()) continue;

        ShareInfo info;
        if (auto it = raw.find("shareUseCache");  it != raw.end()) info.use_cache  = it->second;
        if (auto it = raw.find("shareCachePool"); it != raw.end()) info.cache_pool = it->second;
        _shares[share] = info;
    }
    closedir(d);
}

// Rebuild the set of treatable pools after share config or user exclusions change.
// Pools without an explicit MAX_FILL_PERCENT_<pool> inherit default_max_fill_percent.
static void rebuild_pools(const std::unordered_map<std::string, double>& per_pool_limits) {
    _pools.clear();
    for (const auto& [sname, info] : _shares) {
        if (info.excluded) continue;
        if (info.use_cache != "yes" && info.use_cache != "prefer") continue;
        if (info.cache_pool.empty()) continue;
        if (_pools.count(info.cache_pool)) continue;
        PoolInfo p;
        auto it = per_pool_limits.find(info.cache_pool);
        p.max_fill_percent = (it != per_pool_limits.end())
                           ? it->second
                           : _cfg.default_max_fill_percent;
        _pools[info.cache_pool] = p;
    }
}

// True if the share's file events should be evaluated for promotion.
static bool share_promotable(const std::string& share) {
    auto it = _shares.find(share);
    if (it == _shares.end()) return false;
    const auto& info = it->second;
    if (info.excluded) return false;
    if (info.use_cache != "yes" && info.use_cache != "prefer") return false;
    if (info.cache_pool.empty()) return false;
    return true;
}

static std::string share_pool_path(const std::string& share) {
    auto it = _shares.find(share);
    if (it == _shares.end() || it->second.cache_pool.empty()) return "";
    return MNT_PREFIX + it->second.cache_pool;
}

// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

static void sig_handler(int sig) {
    if      (sig == SIGTERM || sig == SIGINT) _quit   = 1;
    else if (sig == SIGHUP)                   _reload = 1;
    else if (sig == SIGUSR1)                  _paused = 1;
    else if (sig == SIGUSR2)                  _paused = 0;
    else if (sig == SIGRTMIN)                 _paused = !_paused;
}

// ---------------------------------------------------------------------------
// Crash report
// ---------------------------------------------------------------------------
// A daemon that dies should say why. rc.unspin sends stdout/stderr to
// /dev/null, so an uncaught exception's message or a fatal signal would leave
// no trace. The signal handler may only use async-signal-safe calls, so it
// formats its log line by hand and writes it with write().

// Exists while the daemon runs; left behind by a run that didn't stop cleanly.
static const std::string RUNNING_MARKER = "/var/run/unspind.running";

static char _crash_log[PATH_MAX] = "";   // copy of _cfg.log_file for the signal handler
static long _tz_offset = 0;              // seconds east of UTC, refreshed periodically
static char _altstack[65536];            // so a stack overflow can still be reported

static void crash_prepare() {
    snprintf(_crash_log, sizeof(_crash_log), "%s", _cfg.log_file.c_str());
    auto now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    _tz_offset = tm_buf.tm_gmtoff;
}

static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }

static char* put_num(char* p, long v, int width = 0) {
    char tmp[24];
    int n = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n < width) tmp[n++] = '0';
    if (v < 0) *p++ = '-';
    while (n) *p++ = tmp[--n];
    return p;
}

// Local time as "YYYY-MM-DD HH:MM:SS" without localtime() (not signal-safe).
static char* put_time(char* p) {
    long t    = (long)time(nullptr) + _tz_offset;
    long secs = t % 86400;
    long z    = t / 86400 + 719468;               // days -> civil date (H. Hinnant)
    long era  = z / 146097;
    long doe  = z - era * 146097;
    long yoe  = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy  = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp   = (5 * doy + 2) / 153;
    long day  = doy - (153 * mp + 2) / 5 + 1;
    long mon  = mp < 10 ? mp + 3 : mp - 9;
    long year = yoe + era * 400 + (mon <= 2 ? 1 : 0);
    p = put_num(p, year, 4); *p++ = '-'; p = put_num(p, mon, 2); *p++ = '-'; p = put_num(p, day, 2);
    *p++ = ' ';
    p = put_num(p, secs / 3600, 2); *p++ = ':'; p = put_num(p, secs / 60 % 60, 2); *p++ = ':';
    return put_num(p, secs % 60, 2);
}

static const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV:   return "SIGSEGV";   case SIGBUS:    return "SIGBUS";
        case SIGFPE:    return "SIGFPE";    case SIGILL:    return "SIGILL";
        case SIGABRT:   return "SIGABRT";   case SIGTRAP:   return "SIGTRAP";
        case SIGSYS:    return "SIGSYS";    case SIGQUIT:   return "SIGQUIT";
        case SIGALRM:   return "SIGALRM";   case SIGVTALRM: return "SIGVTALRM";
        case SIGPROF:   return "SIGPROF";   case SIGXCPU:   return "SIGXCPU";
        case SIGXFSZ:   return "SIGXFSZ";   case SIGPWR:    return "SIGPWR";
        case SIGSTKFLT: return "SIGSTKFLT";
        default:        return sig > SIGRTMIN && sig <= SIGRTMAX ? "a real-time signal" : "a signal";
    }
}

static void fatal_signal(int sig, siginfo_t* si, void*) {
    char buf[512];
    char* p = put_time(buf);
    p = put_str(p, " [ERROR] unspind killed by ");
    p = put_str(p, signal_name(sig));
    p = put_str(p, " (signal ");
    p = put_num(p, sig);
    *p++ = ')';
    // si_code <= 0: sent by a process (kill, tgkill, sigqueue), not raised by a fault.
    if (si && si->si_code <= 0) {
        p = put_str(p, ", sent by pid ");
        p = put_num(p, si->si_pid);
        char path[48];
        char* q = put_str(path, "/proc/");
        q = put_num(q, si->si_pid);
        q = put_str(q, "/comm");
        *q = '\0';
        int cfd = open(path, O_RDONLY | O_CLOEXEC);
        if (cfd >= 0) {
            char comm[32];
            auto n = read(cfd, comm, sizeof(comm) - 1);
            close(cfd);
            if (n > 0) {
                while (n > 0 && comm[n - 1] == '\n') --n;
                comm[n] = '\0';
                p = put_str(p, " (");
                p = put_str(p, comm);
                *p++ = ')';
            }
        }
    }
    *p++ = '\n';
    int fd = open(_crash_log, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd >= 0) {
        auto w = write(fd, buf, p - buf);
        (void)w;
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_terminate() {
    std::string what = "std::terminate() without an exception";
    if (auto e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) { what = std::string("exception: ") + ex.what(); }
        catch (...)                      { what = "exception of unknown type"; }
    }
    log_err("unspind crashed: uncaught " + what);
    signal(SIGABRT, SIG_DFL);   // abort() below; fatal_signal() would log a second line
    abort();
}

static void install_crash_handlers() {
    std::set_terminate(on_terminate);
    signal(SIGPIPE, SIG_IGN);   // a write to a closed pipe returns EPIPE instead of killing us

    stack_t ss = {};
    ss.ss_sp   = _altstack;
    ss.ss_size = sizeof(_altstack);
    sigaltstack(&ss, nullptr);

    struct sigaction sa = {};
    sa.sa_sigaction = fatal_signal;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS, SIGQUIT,
                    SIGALRM, SIGVTALRM, SIGPROF, SIGXCPU, SIGXFSZ, SIGPWR, SIGSTKFLT})
        sigaction(sig, &sa, nullptr);
    for (int sig = SIGRTMIN + 1; sig <= SIGRTMAX; ++sig)   // SIGRTMIN itself toggles pause
        sigaction(sig, &sa, nullptr);

    if (access(RUNNING_MARKER.c_str(), F_OK) == 0)
        log_warn("The previous unspind run ended without a clean stop. A crash report, if it "
                 "could be written, is in the lines before; without one it was killed with "
                 "SIGKILL or the system lost power.");
    std::ofstream(RUNNING_MARKER).close();
    chmod(RUNNING_MARKER.c_str(), 0644);   // umask(0) in main() would leave it world-writable
}

// ---------------------------------------------------------------------------
// Config loader
// ---------------------------------------------------------------------------

// be graceful with hand edit files...
static void parse_cfg_into(const std::string& path,
                            std::unordered_map<std::string, std::string>& raw) {
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') 
            continue;

        auto pos = line.find('=');
        if (pos == std::string::npos) 
            continue;

        auto k = trim(line.substr(0, pos));
        auto v = trim(line.substr(pos + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        raw[k] = v;
    }
}

// Problems found by load_config(), logged by log_config(): the log isn't open
// while the config loads, at startup and on a reload.
static std::vector<std::string> _cfg_warnings;

static void load_config() {
    _cfg_warnings.clear();
    // Last-resort fallbacks - used only if unspin.cfg.default is missing.
    // These do not usually need to be kept in sync, only if you're bored
    std::unordered_map<std::string, std::string> raw = {
        {"SCAN_PATHS",               SHARE_MNT},
        {"SMALL_FILE_THRESHOLD",     "10 MB"},
        {"SMALL_MIN_ACCESSES",       "1"},
        {"LARGE_SHORT_MIN_ACCESSES", "100"},
        {"LARGE_SHORT_WINDOW_MINS",  "5"},
        {"LARGE_LONG_MIN_ACCESSES",  "2"},
        {"LARGE_LONG_WINDOW_HOURS",  "26"},
        {"DRY_RUN",                  "yes"},
        {"LOG_LEVEL",                "info"},
        {"RULE1_ENABLED",            "yes"},
        {"RULE1_FALLTHROUGH",        "yes"},
        {"RULE2_ENABLED",            "yes"},
        {"RULE3_ENABLED",            "yes"},
        {"RULE3_MIN_READS",          "6"},
        {"PAUSE_ON_RSYNC",           "yes"},
        {"ENABLE_POOL_ATIME",        "no"},
        {"TRACK_WAKEUPS",            "yes"},
        {"EXCLUDE_PATTERNS",         "/nevercachethis,/orthis"},
        {"LOG_FILE",                 "/var/log/unspin.log"},
        {"LOG_MAX_SIZE_MB",          "64"},
        {"LOG_TRIM_SIZE_MB",         "5"},
        {"LOG_EXCLUDED_SHARES",      "no"},
        {"MOUNT_WAIT_TIMEOUT_MINS",  "45"},
        {"MOUNT_RETRY_INTERVAL_SECS","30"},
    };

    parse_cfg_into(DEFAULT_CFG_FILE, raw); // overlay shipped defaults
    parse_cfg_into(_cfg_file, raw);        // overlay user config

    auto get = [&](const std::string& k) -> const std::string& {
        static const std::string empty;
        auto it = raw.find(k);
        return (it != raw.end()) ? it->second : empty;
    };
    // A value that isn't a number - a typo, or the file read by a reload while the
    // settings page is still writing it - keeps the current setting instead of
    // stopping the daemon.
    auto bad = [&](const char* k, const std::string& cur) {
        _cfg_warnings.push_back(std::string("Ignoring invalid setting ") + k + "=\"" + get(k) + "\", keeping " + cur);
    };
    auto num = [&](const char* k, auto cur) -> decltype(cur) {
        try { return (decltype(cur))std::stol(get(k)); }
        catch (...) { bad(k, std::to_string(cur)); return cur; }
    };

    auto raw_scans = split_csv(get("SCAN_PATHS"));

    // Default fill % for pools without a per-pool override.
    // Legacy MAX_HOT_FILL_PERCENT, if present, provides the default (migration).
    _cfg.default_max_fill_percent = DEFAULT_MAX_FILL_PERCENT;
    if (auto it = raw.find("MAX_HOT_FILL_PERCENT"); it != raw.end() && !it->second.empty()) {
        try { _cfg.default_max_fill_percent = std::stod(it->second); }
        catch (...) { bad("MAX_HOT_FILL_PERCENT", std::to_string((int)DEFAULT_MAX_FILL_PERCENT)); }
    }

    // Collect per-pool fill limits: MAX_FILL_PERCENT_<pool>=<pct>
    std::unordered_map<std::string, double> per_pool_limits;
    for (const auto& [k, v] : raw) {
        const std::string prefix = "MAX_FILL_PERCENT_";
        if (k.rfind(prefix, 0) != 0 || k.size() <= prefix.size() || v.empty()) continue;
        try { per_pool_limits[k.substr(prefix.size())] = std::stod(v); }
        catch (...) { _cfg_warnings.push_back("Ignoring malformed fill limit: " + k + "=" + v); }
    }

    try { _cfg.small_file_threshold = parse_size(get("SMALL_FILE_THRESHOLD")); }
    catch (...) { bad("SMALL_FILE_THRESHOLD", human_size(_cfg.small_file_threshold)); }
    _cfg.small_min_accesses       = num("SMALL_MIN_ACCESSES",       _cfg.small_min_accesses);
    _cfg.large_short_min_accesses = num("LARGE_SHORT_MIN_ACCESSES", _cfg.large_short_min_accesses);
    _cfg.large_short_window_mins  = num("LARGE_SHORT_WINDOW_MINS",  _cfg.large_short_window_mins);
    _cfg.large_long_min_accesses  = num("LARGE_LONG_MIN_ACCESSES",  _cfg.large_long_min_accesses);
    _cfg.large_long_window_hours  = num("LARGE_LONG_WINDOW_HOURS",  _cfg.large_long_window_hours);
    _cfg.dry_run                  = (get("DRY_RUN")            == "yes");
    _cfg.debug                    = (get("LOG_LEVEL")          == "debug");
    _cfg.rule1_enabled            = (get("RULE1_ENABLED")      != "no");
    _cfg.rule1_fallthrough        = (get("RULE1_FALLTHROUGH")  == "yes");
    _cfg.rule2_enabled            = (get("RULE2_ENABLED")      != "no");
    _cfg.rule3_enabled            = (get("RULE3_ENABLED")      != "no");
    _cfg.rule3_min_reads          = std::max(0, num("RULE3_MIN_READS", _cfg.rule3_min_reads));
    _cfg.pause_on_rsync           = (get("PAUSE_ON_RSYNC")     != "no");
    _cfg.enable_pool_atime        = (get("ENABLE_POOL_ATIME")  == "yes");
    _cfg.track_wakeups            = (get("TRACK_WAKEUPS")      != "no");
    _cfg.log_file                 = get("LOG_FILE");
    _cfg.log_max_size_mb          = num("LOG_MAX_SIZE_MB",  _cfg.log_max_size_mb);
    _cfg.log_trim_size_mb         = num("LOG_TRIM_SIZE_MB", _cfg.log_trim_size_mb);
    _cfg.log_excluded_shares      = (get("LOG_EXCLUDED_SHARES") == "yes");
    _cfg.mount_wait_timeout_mins  = std::max(0, num("MOUNT_WAIT_TIMEOUT_MINS",   _cfg.mount_wait_timeout_mins));
    _cfg.mount_retry_interval_secs= std::max(1, num("MOUNT_RETRY_INTERVAL_SECS", _cfg.mount_retry_interval_secs));
    _cfg.exclude_patterns         = split_csv(get("EXCLUDE_PATTERNS"));

    load_share_settings();

    // Apply user-excluded shares (EXCLUDED_SHARES) onto loaded share records.
    auto excluded_csv = split_csv(get("EXCLUDED_SHARES"));
    std::unordered_set<std::string> exset(excluded_csv.begin(), excluded_csv.end());
    for (auto& [sname, info] : _shares)
        info.excluded = exset.count(sname) > 0;

    rebuild_pools(per_pool_limits);

    // Filtered here (not earlier) because it needs _pools, which rebuild_pools()
    // just populated - reject the share mount and any known cache pool, both of
    // which fanotify/promotion logic assume scan_paths never contains.
    _cfg.scan_paths.clear();
    for (const auto& sp : raw_scans) {
        if (sp.rfind(SHARE_MNT, 0) == 0) {
            _cfg_warnings.push_back("Ignoring scan path '" + sp + "': " + SHARE_MNT + " is the share mount - "
                     "use array disk mount points (e.g. " + MNT_PREFIX + "disk1) instead");
            continue;
        }
        if (sp.rfind(MNT_PREFIX, 0) == 0) {
            auto end = sp.find('/', MNT_PREFIX.size());
            auto top = sp.substr(MNT_PREFIX.size(), end == std::string::npos ? std::string::npos : end - MNT_PREFIX.size());
            if (_pools.count(top)) {
                _cfg_warnings.push_back("Ignoring scan path '" + sp + "': " + MNT_PREFIX + top + " is a cache pool, not an array disk - "
                         "use array disk mount points (e.g. " + MNT_PREFIX + "disk1) instead");
                continue;
            }
        }
        _cfg.scan_paths.push_back(sp);
    }
}

static void log_config() {
    log_info("Config: " + _cfg_file);
    for (const auto& w : _cfg_warnings) log_warn(w);
    for (const auto& sp : _cfg.scan_paths)
        log_info("Scan path: " + sp);
    for (const auto& [pool, info] : _pools)
        log_info("Pool: " + MNT_PREFIX + pool + " max fill " +
                 std::to_string((int)info.max_fill_percent) + "%");
    {
        std::string excl;
        for (const auto& p : _cfg.exclude_patterns) {
            if (!excl.empty()) excl += ", ";
            excl += p;
        }
        log_info("Exclude patterns: " + (excl.empty() ? std::string("(none)") : excl));
    }
    log_info((_cfg.rule1_enabled ? "" : "DISABLED: ") +
             std::string("Small file threshold: ") + human_size(_cfg.small_file_threshold) +
             ", min reads: " + std::to_string(_cfg.small_min_accesses) +
             (!_cfg.rule1_enabled ? (_cfg.rule1_fallthrough ? " (fallthrough on)" : " (fallthrough off)") : ""));
    log_info((_cfg.rule2_enabled ? "" : "DISABLED: ") +
             std::string("Large short window: ") + std::to_string(_cfg.large_short_min_accesses) +
             " reads / " + std::to_string(_cfg.large_short_window_mins) + " min");
    log_info((_cfg.rule3_enabled ? "" : "DISABLED: ") +
             std::string("Large long window:  ") + std::to_string(_cfg.large_long_min_accesses) +
             " opens / " + std::to_string(_cfg.large_long_window_hours) + " h, min reads filter: " +
             (_cfg.rule3_min_reads == 0 ? std::string("off") : std::to_string(_cfg.rule3_min_reads)));
    if (_cfg.dry_run) log_info("DRY RUN mode - no files will be moved");
    log_info(std::string("Pool access times: ") + (_cfg.enable_pool_atime ? "enabled" : "disabled"));
    log_info(std::string("Disk wake-up tracking: ") + (_cfg.track_wakeups ? "enabled" : "disabled"));
    log_info(_cfg.mount_wait_timeout_mins > 0
             ? "Mount wait: up to " + std::to_string(_cfg.mount_wait_timeout_mins) +
               " min, retry every " + std::to_string(_cfg.mount_retry_interval_secs) + " s"
             : std::string("Mount wait: disabled"));

    // Summarise share status: promotable vs skipped (with reason).
    std::vector<std::string> promo, excluded, not_use, no_pool;
    for (const auto& [sname, info] : _shares) {
        if (info.use_cache != "yes" && info.use_cache != "prefer") { not_use.push_back(sname); continue; }
        if (info.cache_pool.empty()) { no_pool.push_back(sname); continue; }
        if (info.excluded)           { excluded.push_back(sname); continue; }
        promo.push_back(sname + "->" + info.cache_pool);
    }
    auto join = [](const std::vector<std::string>& v) {
        std::string s; for (const auto& x : v) { if (!s.empty()) s += ", "; s += x; } return s;
    };
    if (!promo.empty())    log_info("Treating shares: "            + join(promo));
    if (!excluded.empty()) log_info("User-excluded shares: "       + join(excluded));
    if (!not_use.empty())  log_info("Skipping (use_cache!=yes/prefer): " + join(not_use));
    if (!no_pool.empty())  log_info("Skipping (no cache pool set): " + join(no_pool));
}

// ---------------------------------------------------------------------------
// Disk utilities
// ---------------------------------------------------------------------------

static constexpr unsigned long ZFS_SUPER_MAGIC = 0x2fc12fc1;

struct MountEntry {
    std::string src;     // device / dataset name
    std::string mnt;
    std::string fstype;
};

// The mount that contains path, i.e. the longest mountpoint in /proc/self/mounts
// that is a prefix of path.
static bool mount_entry(const std::string& path, MountEntry& out) {
    std::ifstream f("/proc/self/mounts");
    std::string line;
    bool found = false;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        MountEntry e;
        if (!(ss >> e.src >> e.mnt >> e.fstype)) continue;
        bool under = path == e.mnt || e.mnt == "/" ||
                     (path.compare(0, e.mnt.size(), e.mnt) == 0 && path[e.mnt.size()] == '/');
        if (under && (!found || e.mnt.size() >= out.mnt.size())) { out = e; found = true; }
    }
    return found;
}

static std::string mount_source(const std::string& path) {
    MountEntry e;
    return mount_entry(path, e) ? e.src : "";
}

// statvfs on a ZFS dataset only counts the dataset's own data (not child
// datasets or snapshots), so a pool whose shares are child datasets looks
// nearly empty. Ask zfs instead: "used" of the dataset includes its children.
static bool zfs_fill_percent(const std::string& path, double& pct) {
    auto ds = mount_source(path);
    if (ds.empty() || ds.find('\'') != std::string::npos) return false;
    auto cmd = "zfs get -Hp -o value used,available '" + ds + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    unsigned long long used = 0, avail = 0;
    int n = fscanf(p, "%llu %llu", &used, &avail);
    pclose(p);
    if (n != 2 || used + avail == 0) return false;
    pct = 100.0 * (double)used / (double)(used + avail);
    return true;
}

static double disk_fill_percent(const std::string& path) {
    struct statfs sf;
    double pct;
    if (statfs(path.c_str(), &sf) == 0 && (unsigned long)sf.f_type == ZFS_SUPER_MAGIC &&
        zfs_fill_percent(path, pct))
        return pct;

    // Same calculation as df: reserved blocks count as neither used nor available.
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) != 0  ||  sv.f_blocks == 0)
        return 100.0;
    auto used = (double)(sv.f_blocks - sv.f_bfree);
    auto total = used + (double)sv.f_bavail;
    return total > 0 ? 100.0 * used / total : 100.0;
}

// A whole file, or as much of it as could be read ("" if it can't be opened).
// Plain read(), not an ifstream: a /proc file whose process ends while it's
// being read fails with ESRCH, and libstdc++ throws on that instead of just
// stopping - which ended the daemon.
static std::string read_file(const std::string& path) {
    std::string s;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return s;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) s.append(buf, (size_t)n);
    close(fd);
    return s;
}

// A process's command line with the NUL separators turned into spaces ("" if gone).
static std::string proc_cmdline(const std::string& pid) {
    auto s = read_file("/proc/" + pid + "/cmdline");
    std::replace(s.begin(), s.end(), '\0', ' ');
    return s;
}

// True if a process's name or the first word of its command line (basename) is
// `name`. The command line matters: Mover Tuning (2026.10.30+) re-runs its
// age_mover as `exec -a mover /bin/bash ...`, so its process name is "bash" and
// only argv[0] says "mover".
static bool process_running(const std::string& name) {
    DIR* d = opendir("/proc");
    if (!d) return false;
    auto found = false;
    struct dirent* ent;
    while (!found && (ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        std::string pid = ent->d_name, comm;
        std::ifstream c("/proc/" + pid + "/comm");
        std::getline(c, comm);
        if (comm == name) { found = true; break; }
        auto cmd  = proc_cmdline(pid);
        auto arg0 = cmd.substr(0, cmd.find(' '));
        auto base = arg0.substr(arg0.rfind('/') + 1);  // npos + 1 = 0: no slash
        found = (base == name);
    }
    closedir(d);
    return found;
}

// The mover: /var/run/mover.pid (written by Unraid's mover and by Mover Tuning)
// pointing to a live process with "mover" in its command line - the check guards
// against a stale pid file whose PID got reused - or else a process called mover.
static bool mover_running() {
    std::ifstream f("/var/run/mover.pid");
    long pid = 0;
    if (f >> pid && pid > 0 &&
        proc_cmdline(std::to_string(pid)).find("mover") != std::string::npos)
        return true;
    return process_running("mover");
}

// Live check - mover always counted, rsync only if PAUSE_ON_RSYNC is set.
// Returns what's running ("Mover" wins over "rsync"), "" for nothing.
static std::string transfers_running() {
    if (mover_running())
        return "Mover";
    if (_cfg.pause_on_rsync && process_running("rsync"))
        return "rsync";
    return "";
}

// Cached transfers check - refreshes at most every MOVER_CACHE_SEC seconds.
// Log when monitoring pauses/resumes
static bool transfers_active() {
    auto now = time(nullptr);
    if (now - _transfers_checked < MOVER_CACHE_SEC)
        return !_transfer_cached.empty();
    _transfers_checked = now;

    auto prev = _transfer_cached;
    _transfer_cached = transfers_running();
    if (_transfer_cached != prev) {
        if (prev.empty())
            log_info(_transfer_cached + " started - pausing monitoring");
        else if (_transfer_cached.empty())
            log_info(prev + " finished - resuming monitoring");
        else
            log_info(prev + " finished, " + _transfer_cached + " running - monitoring stays paused");
    }
    return !_transfer_cached.empty();
}

// Pause reasons only the daemon knows (a running Mover or rsync, a pause by
// signal), one per line, for the settings page and rc.unspin status - pause
// locks they read from PAUSE_DIR themselves. Rewritten only when the reasons
// change; removed when there are none and on exit.
static const std::string PAUSE_STATE = "/var/run/unspind.pause-reasons";
static void write_pause_state() {
    static std::string last;
    std::string s;
    if (transfers_active()) s += _transfer_cached + "\n";
    if (_paused)            s += "signal\n";
    if (s == last) return;
    last = s;
    if (s.empty()) { unlink(PAUSE_STATE.c_str()); return; }
    auto tmp = PAUSE_STATE + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << s;
    }
    chmod(tmp.c_str(), 0644);
    rename(tmp.c_str(), PAUSE_STATE.c_str());
}

// True if any named pause lock exists in PAUSE_DIR. rc.unspin pause/unpause just
// touch/rm a file here; the daemon decides its own paused state by looking at the
// directory itself instead of trusting a signal from rc.unspin, since detecting the
// exact 0->1 / 1->0 transition from a separate process is inherently racy when two
// pause/unpause calls land close together (see PAUSE_DIR comment above).
// A lock set with `pause <name> --ttl` holds its TTL in seconds as file content;
// its mtime is the last refresh. One not refreshed within its TTL (the job that
// set it died or its VM was stopped) is removed here. Empty = never expires.
static bool pause_locks_present() {
    DIR* d = opendir(PAUSE_DIR.c_str());
    if (!d) return false;  // directory doesn't exist yet = no locks held
    auto found = false;
    auto now   = time(nullptr);
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        auto path = PAUSE_DIR + "/" + ent->d_name;
        long ttl  = 0;
        std::ifstream f(path);
        if (f) f >> ttl;
        struct stat st;
        if (ttl > 0 && stat(path.c_str(), &st) == 0 && now - st.st_mtime > ttl) {
            if (unlink(path.c_str()) == 0)
                log_info(std::string("Pause lock expired - ") + ent->d_name
                         + " (not refreshed within " + std::to_string(ttl) + "s)");
            continue;
        }
        found = true;
    }
    closedir(d);
    return found;
}

// Cached pause-dir check - refreshes at most every PAUSE_DIR_CACHE_SEC seconds.
static bool pause_dir_active() {
    auto now = time(nullptr);
    if (now - _pause_dir_checked < PAUSE_DIR_CACHE_SEC)
        return _pause_dir_cached;
    _pause_dir_checked = now;
    _pause_dir_cached = pause_locks_present();
    return _pause_dir_cached;
}

// Combined pause state: a direct SIGUSR1/SIGUSR2/SIGRTMIN signal (manual, documented
// on the CLI banner) OR at least one named lock in PAUSE_DIR (rc.unspin pause/unpause).
static bool is_paused() {
    auto locks = pause_dir_active();  // first, so expired locks are removed even while signal-paused
    return _paused || locks;
}

// ---------------------------------------------------------------------------
// File movement
// ---------------------------------------------------------------------------

// Map /mnt/<disk>/<share>/<rel> to /mnt/<share.cache_pool>/<share>/<rel>.
// Returns "" if the share is unknown or not promotable; callers must handle this.
static std::string resolve_destination(const std::string& src) {
    if (src.rfind(MNT_PREFIX, 0) != 0) return "";
    auto pool_end = src.find('/', MNT_PREFIX.size());
    if (pool_end == std::string::npos) return "";
    auto share_end = src.find('/', pool_end + 1);
    if (share_end == std::string::npos) return "";
    auto share = src.substr(pool_end + 1, share_end - pool_end - 1);

    auto pool_path = share_pool_path(share);
    if (pool_path.empty()) return "";
    return pool_path + src.substr(pool_end);
}

// A promotion moves a file while other programs may use it: a user may move,
// delete or save it during the copy. So the move never shows a half-done or
// stale result, the file never goes missing from the user share, and
// whatever the user did wins:
//   1. Open somewhere: no copy now, the promotion waits for the file's close.
//   2. Copy into a nameless file on the pool (O_TMPFILE). Nobody can see it,
//      and the kernel drops it if Unspin dies.
//   3. Check: unchanged since the copy started, and still open nowhere.
//   4. Give the copy its name on the pool. shfs looks a file up on every
//      open and the pool comes first, so from now on the user share shows
//      the copy (measured: the very next read gets it, no read fails).
//   5. Take the array file: rename it to a hidden name next to it. From then
//      on nobody can reach it, so nothing can change under the checks.
//   6. Check again: still the file that was copied, unchanged, open nowhere.
//   7. Delete the hidden original.
// Changed or opened between 3 and 5: the array file is put back first, then
// the copy is removed, so the share never misses the file either; a copy
// somebody already opened is kept, with an error naming both. Moved or
// deleted on the array before step 5: the copy is removed, the user's action
// wins. The pending step is noted in HANDOVER_FILE, so a restart after a
// crash in that moment finishes it or puts the file back.

static const std::string HANDOVER_FILE = "/var/run/unspind.handover";
static constexpr unsigned RENAME_NOREPLACE_FLAG = 1; // RENAME_NOREPLACE, renameat2(2)

enum class Handover { done, failed, gone, changed, in_use };

static int rename_noreplace(const std::string& from, const std::string& to) {
    return (int)syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE_FLAG);
}

// Same file with the same content. ctime is left out after our own rename,
// which sets it.
static bool unchanged(const struct stat& a, const struct stat& b, bool with_ctime) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           (!with_ctime || (a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec));
}

// ".unspin.NAME" next to the file, or ".unspin.INODE" if NAME is too long.
static std::string hidden_name(const std::string& src, ino_t ino) {
    auto slash = src.rfind('/');
    auto base = src.substr(slash + 1);
    if (base.size() + 8 > NAME_MAX) base = std::to_string(ino);
    return src.substr(0, slash + 1) + ".unspin." + base;
}

// The paths plus size and mtime of the original, which the copy shares.
static void note_handover(const std::string& hidden, const std::string& src, const std::string& dst,
                          const struct stat& orig) {
    std::ofstream f(HANDOVER_FILE, std::ios::trunc | std::ios::binary);
    f << hidden << '\0' << src << '\0' << dst << '\0' << orig.st_size << '\0'
      << orig.st_mtim.tv_sec << '\0' << orig.st_mtim.tv_nsec << '\0';
}

static bool put_back(const std::string& hidden, const std::string& src) {
    if (rename_noreplace(hidden, src) == 0) return true;
    log_err("Cannot put " + qpath(src) + " back (" + strerror(errno) + ") - it is kept as " + qpath(hidden));
    return false;
}

// Removes the folders made for dst on the pool, up to the deepest one that
// existed before (only empty ones go).
static void remove_new_dirs(const std::string& dst, const std::string& existed) {
    auto dir = dst.substr(0, dst.rfind('/'));
    while (dir.size() > existed.size() && rmdir(dir.c_str()) == 0)
        dir = dir.substr(0, dir.rfind('/'));
}

// The process that has the file open or mapped, other than Unspin, as
// "comm(pid)", or "". Matches the file name at the end of the fd links (plain
// text, no file system access: a stat() through a FUSE file could wake a
// disk) and confirms the inode; works for containers that see another path.
static std::string file_holder(const std::string& path, dev_t dev, ino_t ino) {
    auto suffix = path.substr(path.rfind('/'));
    char dev_s[32];
    snprintf(dev_s, sizeof dev_s, "%02x:%02x", major(dev), minor(dev));
    auto ends_with = [](const std::string& s, const std::string& end) {
        return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    };
    auto self = std::to_string(getpid());
    DIR* proc = opendir("/proc");
    if (!proc) return "";
    std::string holder;
    while (auto* e = readdir(proc)) {
        if (!isdigit((unsigned char)e->d_name[0]) || self == e->d_name) continue;
        auto base = std::string("/proc/") + e->d_name;
        auto found = false;
        if (DIR* fds = opendir((base + "/fd").c_str())) {
            while (auto* f = readdir(fds)) {
                if (f->d_name[0] == '.') continue;
                char link[PATH_MAX];
                auto n = readlink((base + "/fd/" + f->d_name).c_str(), link, sizeof link - 1);
                if (n <= 0) continue;
                link[n] = '\0';
                if (!ends_with(link, suffix)) continue;
                std::ifstream info(base + "/fdinfo/" + f->d_name);
                std::string line;
                while (std::getline(info, line))
                    if (line.rfind("ino:", 0) == 0) { found = strtoull(line.c_str() + 4, nullptr, 10) == ino; break; }
                if (found) break;
            }
            closedir(fds);
        }
        if (!found) {
            std::ifstream maps(base + "/maps");
            std::string line;
            while (!found && std::getline(maps, line)) {
                if (!ends_with(line, suffix)) continue;
                char mdev[32];
                unsigned long long mino = 0;
                found = sscanf(line.c_str(), "%*s %*s %*s %31s %llu", mdev, &mino) == 2 &&
                        mino == ino && strcmp(mdev, dev_s) == 0;
            }
        }
        if (found) {
            std::ifstream c(base + "/comm");
            std::string comm;
            std::getline(c, comm);
            holder = comm + "(" + e->d_name + ")";
            break;
        }
    }
    closedir(proc);
    return holder;
}

// Removes the copy from the pool again, unless somebody already opened or
// changed it. False if it had to stay.
static bool drop_copy(const std::string& dst, const struct stat& copy) {
    struct stat d;
    if (lstat(dst.c_str(), &d) != 0 || d.st_dev != copy.st_dev || d.st_ino != copy.st_ino)
        return true;  // already gone, or no longer ours
    if (!unchanged(copy, d, false) || !file_holder(dst, d.st_dev, d.st_ino).empty())
        return false;
    if (unlink(dst.c_str()) == 0) return true;
    log_err("unlink(" + dst + "): " + strerror(errno));
    return false;
}

// Moves src on the array to dst on its pool, as described above. why gets the
// error for failed, the holder for in_use.
static Handover move_to_cache(const std::string& src, const std::string& dst, std::string& why) {
    int src_fd = open(src.c_str(), O_RDONLY | O_NOATIME | O_CLOEXEC);
    if (src_fd < 0 && errno == EPERM) src_fd = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        if (errno == ENOENT) return Handover::gone;
        why = "open(" + src + "): " + strerror(errno);
        return Handover::failed;
    }
    int tmp_fd = -1;
    auto end = [&](Handover h, std::string w = "") {
        if (tmp_fd >= 0) close(tmp_fd);
        close(src_fd);
        why = std::move(w);
        return h;
    };

    struct stat before;
    if (fstat(src_fd, &before) < 0) return end(Handover::failed, "fstat(" + src + "): " + strerror(errno));
    if (access(dst.c_str(), F_OK) == 0) return end(Handover::failed, "destination already exists: " + dst);

    // The nameless file goes into the deepest existing folder of dst: same file
    // system as dst (a ZFS share can be its own dataset). The missing folders
    // are created right before the copy gets its name, and removed again if
    // the promotion steps back.
    auto pool_root = dst.substr(0, dst.find('/', MNT_PREFIX.size()));
    auto tmp_dir = dst.substr(0, dst.rfind('/'));
    while (tmp_dir.size() > pool_root.size() && access(tmp_dir.c_str(), F_OK) != 0)
        tmp_dir = tmp_dir.substr(0, tmp_dir.rfind('/'));
    mode_t mode = (before.st_mode & 0222) ? 0666 : 0444;
    tmp_fd = open(tmp_dir.c_str(), O_TMPFILE | O_WRONLY | O_CLOEXEC, mode);
    if (tmp_fd < 0)
        return end(Handover::failed, "Cannot create a nameless file in " + tmp_dir +
                                     " for the copy (O_TMPFILE): " + strerror(errno));

    // The copy's pages are written out and dropped from RAM as it goes, so a
    // promotion doesn't push other cached data out (above all the array disks'
    // directory blocks, whose loss means a spin-up on the next listing). The
    // original's pages go with it when it's removed.
    off_t offset = 0, dropped = 0;
    auto remaining = before.st_size;
    while (remaining > 0) {
        auto n = sendfile(tmp_fd, src_fd, &offset, (size_t)std::min(remaining, (off_t)COPY_CHUNK_BYTES));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return end(Handover::failed, "sendfile(" + src + "): " + strerror(errno));
        if (n == 0) return end(Handover::changed);  // shrank during the copy
        remaining -= n;
        if (offset - dropped >= COPY_DROP_BYTES) {
            sync_file_range(tmp_fd, dropped, offset - dropped,
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
            posix_fadvise(tmp_fd, dropped, offset - dropped, POSIX_FADV_DONTNEED);
            dropped = offset;
        }
    }

    // Unraid-standard permissions and ownership. Keep the original mtime;
    // atime = now: the promotion is itself an access, and the array copy's
    // atime is stale (Unraid mounts disks noatime), which would make Mover
    // Tuning's ATIME filter demote the file on its next run.
    fchmod(tmp_fd, mode);
    if (fchown(tmp_fd, _nobody_uid, _nobody_gid) != 0) log_err("chown(" + dst + "): " + strerror(errno));
    struct timespec times[2] = { {0, UTIME_NOW}, before.st_mtim };
    futimens(tmp_fd, times);
    // On the pool for good before the original goes: else a power cut could lose both.
    if (fsync(tmp_fd) != 0) return end(Handover::failed, "fsync(" + dst + "): " + strerror(errno));
    posix_fadvise(tmp_fd, 0, 0, POSIX_FADV_DONTNEED);

    struct stat copy, now;
    if (fstat(tmp_fd, &copy) != 0) return end(Handover::failed, "fstat(copy of " + src + "): " + strerror(errno));
    if (fstat(src_fd, &now) != 0 || !unchanged(before, now, true)) {
        struct stat at;  // a rename sets ctime, too: moved or deleted, not changed?
        auto moved = lstat(src.c_str(), &at) != 0 || at.st_dev != before.st_dev || at.st_ino != before.st_ino;
        return end(moved ? Handover::gone : Handover::changed);
    }
    if (auto holder = file_holder(src, before.st_dev, before.st_ino); !holder.empty())
        return end(Handover::in_use, holder);  // opened during the copy

    auto hidden = hidden_name(src, before.st_ino);
    note_handover(hidden, src, dst, before);
    auto done_with = [&](Handover h, std::string w = "") {
        unlink(HANDOVER_FILE.c_str());
        return end(h, std::move(w));
    };
    // Both copies exist from here until the original is taken; the user share
    // shows the pool copy.
    auto both_kept = [&](const std::string& orig) {
        log_err("Promotion of " + qpath(src) + " stepped back, but the pool copy was already in use - "
                "both kept: " + qpath(orig) + " and " + qpath(dst));
    };

    mkdir_p(dst.substr(0, dst.rfind('/')));
    if (linkat(tmp_fd, "", AT_FDCWD, dst.c_str(), AT_EMPTY_PATH) != 0) {
        auto err = errno;
        remove_new_dirs(dst, tmp_dir);
        return done_with(Handover::failed, "linkat(" + dst + "): " + strerror(err));
    }

    if (rename_noreplace(src, hidden) != 0) {
        auto err = errno;
        if (drop_copy(dst, copy)) remove_new_dirs(dst, tmp_dir);
        else if (err == ENOENT)
            log_err(qpath(src) + " was moved or deleted on the array during its promotion, but the pool copy "
                    "was already in use - it is kept: " + qpath(dst));
        else both_kept(src);
        if (err == ENOENT) return done_with(Handover::gone);  // moved or deleted on the array meanwhile
        return done_with(Handover::failed, "rename(" + src + "): " + strerror(err));
    }

    struct stat taken;
    auto same = lstat(hidden.c_str(), &taken) == 0 && taken.st_dev == before.st_dev &&
                taken.st_ino == before.st_ino && fstat(src_fd, &now) == 0 && unchanged(before, now, false);
    auto holder = same ? file_holder(hidden, before.st_dev, before.st_ino) : std::string();
    if (!same || !holder.empty()) {
        // Original first, so the share never misses the file.
        if (put_back(hidden, src)) {
            if (!drop_copy(dst, copy)) both_kept(src);
            else remove_new_dirs(dst, tmp_dir);
        }
        return done_with(same ? Handover::in_use : Handover::changed, holder);
    }

    // Deleted or moved through the share in the moment both existed: that
    // reached the pool copy and was meant for this file, so the original goes
    // either way.
    if (unlink(hidden.c_str()) != 0)
        log_err("unlink(" + hidden + "): " + strerror(errno) +
                " - the copy at " + dst + " is complete, this leftover can be deleted");
    return done_with(Handover::done);
}

// After a crash in the middle of a hand-over. Without a pool copy the array
// file is put back. With both, the one still matching the original's size
// and mtime goes: the other one holds a change somebody made meanwhile.
static void recover_handover() {
    std::ifstream f(HANDOVER_FILE, std::ios::binary);
    if (!f) return;
    std::string field[6];
    for (auto& s : field) std::getline(f, s, '\0');
    f.close();
    unlink(HANDOVER_FILE.c_str());
    const auto& hidden = field[0];
    const auto& src    = field[1];
    const auto& dst    = field[2];
    long long size = -1, sec = 0, nsec = 0;
    try { size = std::stoll(field[3]); sec = std::stoll(field[4]); nsec = std::stoll(field[5]); }
    catch (...) {}
    if (hidden.empty() || src.empty() || dst.empty() || size < 0) {
        log_warn("Unreadable note of an interrupted promotion, nothing changed: " + HANDOVER_FILE);
        return;
    }
    auto original = [&](const struct stat& s) {
        return s.st_size == size && s.st_mtim.tv_sec == sec && s.st_mtim.tv_nsec == nsec;
    };

    struct stat o, d;
    auto hidden_there = lstat(hidden.c_str(), &o) == 0;
    auto orig = hidden_there ? hidden : src;
    if (!hidden_there && lstat(src.c_str(), &o) != 0) return;  // finished, or the user moved it
    if (lstat(dst.c_str(), &d) != 0) {
        if (hidden_there) {
            log_warn("Undoing a promotion interrupted by a crash: " + qpath(src) + " stays on the array");
            put_back(hidden, src);
        }
        return;
    }
    if (original(o)) {
        log_warn("Finishing a promotion interrupted by a crash: " + qpath(src) + " is on the pool");
        if (unlink(orig.c_str()) != 0) log_err("unlink(" + orig + "): " + strerror(errno));
        return;
    }
    auto back = !hidden_there || put_back(hidden, src);
    if (back && original(d)) {
        log_warn("Undoing a promotion interrupted by a crash: " + qpath(src) + " was changed on the array, it stays there");
        if (unlink(dst.c_str()) != 0) log_err("unlink(" + dst + "): " + strerror(errno));
    } else {
        log_err("A promotion interrupted by a crash left two changed copies, both kept: " +
                qpath(back ? src : hidden) + " and " + qpath(dst));
    }
}

// ---------------------------------------------------------------------------
// Disk state
// ---------------------------------------------------------------------------
// Unspin must never wake a sleeping disk. The spin state of each array disk
// comes from emhttpd's disks.ini. That only shows a spin-up once it's done
// (measured: ~0.3 s after an ~11 s spin-up), so the block-layer counters in
// /proc/diskstats tell whether a disk marked as spun down has seen I/O since:
// if it has, it's spinning up or already up. A disk put to sleep outside
// Unraid (hdparm -y) never shows up in disks.ini, but Unraid's own polling
// wakes such a disk again within seconds anyway (measured: 22 s).

static const std::string DISKS_INI = "/var/local/emhttp/disks.ini";
static const std::string DISKSTATS = "/proc/diskstats";

struct IoCounters {
    uint64_t reads = 0, writes = 0, in_flight = 0;
};

// A file access on a sleeping disk (see "Disk wake-ups" below). Only a suspect:
// a read served from RAM doesn't wake the disk.
struct Suspect {
    int64_t     at_ms = 0;      // first event
    int64_t     last_ms = 0;    // latest event
    bool        from_shfs = false;    // the event came from shfs (one suspect per file, not per process)
    pid_t       pid   = 0;
    std::string label;          // what gets counted: "container plex", "SMB", "Mover", "rsync"
    std::string name;           // short form for the summary line: "plex", "Mover", "rsync"
    std::string detail;         // the program inside a container or under the mover, if different
    std::string access;         // "reading", "writing", "opening"
    std::string path;
    bool        smb   = false;  // user and client are looked up only if it's the culprit
    pid_t       smb_pid = 0;    // the smbd process to look up
    bool        via_shfs = false;     // shfs (/mnt/user) accessed the disk; the requester is looked up
    int64_t     shfs_tried_ms = 0;    // last lookup attempt
    int         blocked_rank = 0;     // found waiting during the spin-up: 3 disk path, 2 user share path, 1 shfs
};

struct DiskState {
    std::string name;                  // "disk4"
    std::string device;                // "sdb"
    std::string mount;                 // "/mnt/disk4"
    bool        spundown     = false;  // as disks.ini reports it
    time_t      spundown_at  = 0;      // when Unspin saw it spin down (identifies the sleep period)
    IoCounters  baseline;              // counters at spin-down
    bool        io_seen      = false;  // I/O since spin-down: spinning up or up
    int64_t     io_at_ms     = 0;      // when that I/O was first seen: the spin-up start
    int64_t     woke_at_ms   = 0;      // disks.ini showed it awake; recorded after WAKE_GRACE_MS
    std::vector<Suspect> suspects;     // file accesses while asleep or waking
    std::vector<Suspect> blocked;      // processes waiting for the disk during its spin-up
    int64_t     blocked_scan_ms = 0;   // last scan for them
    std::string blocked_logged;        // last scan's result, logged only when it changes
    bool        metadata_write = false; // XFS wrote metadata to it during the spin-up
};

static std::vector<DiskState> _disks;
// Parity disks: no mountpoint, so not in _disks. A parity disk only spins up
// for a write, which tells a buffered write from a read when nobody waited.
static std::vector<DiskState> _parity;
static struct timespec        _disks_ini_mtime = {0, 0};

static int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void record_wakeup(DiskState& d);   // defined in "Disk wake-ups" below
static void scan_blocked(DiskState& d);    // defined in "Disk wake-ups" below

static std::unordered_map<std::string, IoCounters> read_diskstats() {
    std::unordered_map<std::string, IoCounters> out;
    std::ifstream f(DISKSTATS);
    std::string line;
    while (std::getline(f, line)) {
        // major minor name reads merged sectors ms writes merged sectors ms in_flight ...
        std::istringstream ss(line);
        unsigned major, minor;
        std::string name;
        uint64_t v[8];
        IoCounters c;
        if (!(ss >> major >> minor >> name)) continue;
        if (!(ss >> c.reads >> v[0] >> v[1] >> v[2] >> c.writes >> v[3] >> v[4] >> v[5] >> c.in_flight)) continue;
        out[name] = c;
    }
    return out;
}

// Re-reads disks.ini when it changed. Keeps the per-disk state across reloads,
// so a spin-down baseline survives emhttpd rewriting the file.
static void load_disks() {
    std::ifstream f(DISKS_INI);
    if (!f) return;
    std::vector<DiskState> fresh, fresh_parity;
    std::unordered_map<std::string, std::string> kv;
    auto flush = [&]() {
        auto data   = kv["type"] == "Data" && !kv["fsMountpoint"].empty();
        auto parity = kv["type"] == "Parity";
        if ((data || parity) && !kv["device"].empty()) {
            DiskState d;
            d.name     = kv["name"];
            d.device   = kv["device"];
            d.mount    = data ? kv["fsMountpoint"] : "";
            d.spundown = kv["spundown"] == "1";
            (data ? fresh : fresh_parity).push_back(d);
        }
        kv.clear();
    };
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] == '[') { flush(); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto v = line.substr(eq + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        kv[line.substr(0, eq)] = v;
    }
    flush();

    std::unordered_map<std::string, IoCounters> io;
    auto now = time(nullptr);
    for (auto& p : fresh_parity) {   // only the spin-up start matters
        auto old = std::find_if(_parity.begin(), _parity.end(),
                                [&](const DiskState& o) { return o.name == p.name && o.device == p.device; });
        auto was_spundown = old != _parity.end() && old->spundown;
        if (old != _parity.end()) {
            p.baseline = old->baseline;
            p.io_seen  = old->io_seen;
            p.io_at_ms = old->io_at_ms;
        }
        if (p.spundown && !was_spundown) {
            if (io.empty()) io = read_diskstats();
            p.baseline = io[p.device];
            p.io_seen  = false;
            p.io_at_ms = 0;
        }
    }
    _parity.swap(fresh_parity);
    for (auto& d : fresh) {
        auto old = std::find_if(_disks.begin(), _disks.end(),
                                [&](const DiskState& o) { return o.name == d.name; });
        auto same = old != _disks.end() && old->device == d.device;
        if (same) {   // keep everything that doesn't come from disks.ini
            d.spundown_at = old->spundown_at;
            d.baseline    = old->baseline;
            d.io_seen     = old->io_seen;
            d.io_at_ms    = old->io_at_ms;
            d.woke_at_ms  = old->woke_at_ms;
            d.suspects    = std::move(old->suspects);
            d.blocked     = std::move(old->blocked);
            d.blocked_scan_ms = old->blocked_scan_ms;
            d.blocked_logged  = old->blocked_logged;
            d.metadata_write  = old->metadata_write;
        }
        auto was_spundown = same && old->spundown;
        if (d.spundown && !was_spundown) {
            if (d.woke_at_ms) record_wakeup(d);   // asleep again within the grace period
            if (io.empty()) io = read_diskstats();
            d.spundown_at = now;
            d.baseline    = io[d.device];
            d.io_seen     = false;
            d.io_at_ms    = 0;
            d.suspects.clear();
            d.blocked.clear();
            d.blocked_logged.clear();
            d.metadata_write = false;
            log_debug(d.name + " spun down");
        } else if (!d.spundown && was_spundown) {
            log_debug(d.name + " spun up");
            // The I/O check runs on every main-loop pass; catch I/O it hasn't seen yet.
            if (!d.io_seen) {
                if (io.empty()) io = read_diskstats();
                const auto& c = io[d.device];
                if (c.reads != d.baseline.reads || c.writes != d.baseline.writes) {
                    d.io_seen  = true;
                    d.io_at_ms = now_ms();
                    if (_cfg.track_wakeups) scan_blocked(d);   // whoever woke it may still wait
                }
            }
            // Recorded a moment later: when a path lookup itself needed the disk,
            // the file event only arrives after the spin-up.
            d.woke_at_ms = now_ms();
        }
    }
    _disks.swap(fresh);
}

static constexpr int64_t WAKE_GRACE_MS      = 2000;    // events still accepted after disks.ini shows the disk awake
static constexpr int64_t SUSPECT_MAX_AGE_MS = 90000;   // a buffered write reaches the disk within ~35 s
static constexpr int64_t READ_LEAD_MS       = 5000;    // a read or open wakes the disk within this time
static constexpr int64_t WRITE_LEAD_MS      = 60000;   // a buffered write reaches the disk within this time
static constexpr size_t  SUSPECTS_MAX       = 8;

static void resolve_shfs_suspects(DiskState& d, bool force);   // defined in "Disk wake-ups" below

// Called on every main-loop pass and before every promotion: picks up a
// changed disks.ini, and notices I/O on disks marked as spun down.
static void update_disk_state() {
    auto now = now_ms();
    // I/O on disks marked as spun down. Checked before disks.ini is reloaded:
    // a disk woken moments after its spin-down may show as awake there already.
    auto waiting = [](const DiskState& d) { return d.spundown && !d.io_seen; };
    if (std::any_of(_disks.begin(), _disks.end(), waiting) ||
        std::any_of(_parity.begin(), _parity.end(), waiting)) {
        auto io = read_diskstats();
        auto started = [&](DiskState& d) {
            if (!waiting(d)) return false;
            auto it = io.find(d.device);
            if (it == io.end()) return false;
            const auto& c = it->second;
            if (c.reads == d.baseline.reads && c.writes == d.baseline.writes && c.in_flight == 0) return false;
            d.io_seen  = true;
            d.io_at_ms = now;
            log_debug(d.name + " is spinning up");
            return true;
        };
        for (auto& p : _parity) started(p);
        for (auto& d : _disks)
            if (started(d) && _cfg.track_wakeups) scan_blocked(d);   // the cause waits for the disk right now
    }
    struct stat st;
    if (stat(DISKS_INI.c_str(), &st) == 0 &&
        (st.st_mtim.tv_sec != _disks_ini_mtime.tv_sec || st.st_mtim.tv_nsec != _disks_ini_mtime.tv_nsec)) {
        _disks_ini_mtime = st.st_mtim;
        load_disks();
    }
    for (auto& d : _disks) {
        if (d.spundown || d.woke_at_ms) resolve_shfs_suspects(d, false);
        if (_cfg.track_wakeups && d.spundown && d.io_seen && now - d.blocked_scan_ms >= 1000) scan_blocked(d);
        if (d.woke_at_ms && now - d.woke_at_ms >= WAKE_GRACE_MS) record_wakeup(d);
        if (d.spundown)   // cached reads on a sleeping disk that never led anywhere
            d.suspects.erase(std::remove_if(d.suspects.begin(), d.suspects.end(),
                                 [&](const Suspect& s) { return now - s.last_ms > SUSPECT_MAX_AGE_MS; }),
                             d.suspects.end());
    }
}

static constexpr int64_t PARITY_WINDOW_MS = 3000;    // parity starts spinning up with the data disk it writes for
static constexpr int64_t PARITY_LEAD_MS   = 15000;   // or up to one spin-up earlier, for a write to another disk in the same flush

// A parity disk started spinning up together with this disk, or shortly
// before it: changes are being written out.
static bool parity_woke_with(const DiskState& d) {
    return std::any_of(_parity.begin(), _parity.end(), [&](const DiskState& p) {
        auto lead = d.io_at_ms - p.io_at_ms;   // positive: parity started first
        return p.io_at_ms && lead >= -PARITY_WINDOW_MS && lead <= PARITY_LEAD_MS;
    });
}

// While a wake-up may be under way, the main loop polls fast so the spin-up
// start (io_at_ms) is pinned down closely enough to tell suspects apart.
static constexpr time_t AFTER_SPINDOWN_WATCH_S = 15;   // a disk woken right after its spin-down is back up within seconds

static bool wake_watch_active() {
    auto now = time(nullptr);
    return std::any_of(_disks.begin(), _disks.end(), [&](const DiskState& d) {
        return d.woke_at_ms ||
               (d.spundown && (d.io_seen || !d.suspects.empty() || now - d.spundown_at < AFTER_SPINDOWN_WATCH_S));
    });
}

// The array disk a path lives on (longest mountpoint prefix, so nested ZFS
// dataset mounts count too), or nullptr.
static DiskState* disk_for_path(const std::string& path) {
    DiskState* best = nullptr;
    for (auto& d : _disks) {
        if (path.compare(0, d.mount.size(), d.mount) != 0) continue;
        if (path.size() > d.mount.size() && path[d.mount.size()] != '/') continue;
        if (!best || d.mount.size() > best->mount.size()) best = &d;
    }
    return best;
}

// The disk a path lives on, if that disk is asleep; nullptr otherwise.
static const DiskState* sleeping_disk(const std::string& path) {
    update_disk_state();
    auto d = disk_for_path(path);
    return (d && d->spundown && !d->io_seen) ? d : nullptr;
}

// Writes a file via a temp file and rename, so a reader never sees half of it.
static bool write_file_atomic(const std::string& path, const std::string& content) {
    auto tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << content;
        f.close();
        if (!f) {
            log_err("Cannot write " + tmp);
            return false;
        }
    }
    chmod(tmp.c_str(), 0644);  // umask(0) in main() would leave it world-writable
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        log_err("rename(" + tmp + "): " + strerror(errno));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Disk wake-ups (TRACK_WAKEUPS)
// ---------------------------------------------------------------------------
// Which process, container, VM or SMB client woke a sleeping disk. Every file
// event on a sleeping disk makes its process a suspect, described right away
// while the process still exists. A suspect is only blamed once the disk
// really spins up (disks.ini), because a read served from RAM wakes nothing:
//   1. the latest suspect before the spin-up started (the first I/O in
//      /proc/diskstats) - a read with a cached path, or a buffered write that
//      reached the disk later;
//   2. otherwise the first one after - a read whose path lookup itself needed
//      the disk only gets its file event after the spin-up;
//   3. otherwise I/O without any file opened: a directory listing or metadata;
//   4. otherwise no I/O at all: a command sent to the disk (spin-up, SMART).

static const std::string WAKE_LIVE  = "/var/run/unspind.wakeups";
static const std::string WAKE_FILE  = "/boot/config/plugins/unspin/wakeups";
// Created by the settings page's reset button (which also deletes both files).
static const std::string WAKE_RESET = "/var/run/unspind.wakeups-reset";
static constexpr size_t  WAKE_RECENT_PER_DISK = 20;   // per disk, so a busy disk can't push a quiet one out

struct WakeEntry {
    time_t      at = 0;
    std::string disk, label, name, access, path, also;
};

static std::map<std::tuple<std::string, std::string, std::string>, int64_t> _wake_counts; // (date, disk, label)
static std::deque<WakeEntry> _wake_recent;
static bool   _wake_flash_dirty = false;
static time_t _wake_flash_last  = 0;
static std::unordered_map<std::string, std::string> _container_names;   // container id -> name

static std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

// argv[0] of a process, without its directory: Mover Tuning runs as
// `exec -a mover bash`, so its process name says "bash" but argv[0] "mover".
// Read up to the first NUL, so names with spaces ("Plex Media Scanner") stay whole.
static std::string proc_arg0(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string a;
    std::getline(f, a, '\0');
    return a.substr(a.rfind('/') + 1);   // npos + 1 = 0: no slash
}

static pid_t proc_ppid(pid_t pid) {
    auto s = read_first_line("/proc/" + std::to_string(pid) + "/stat");
    auto p = s.rfind(')');                // comm may contain spaces and parentheses
    if (p == std::string::npos) return 0;
    std::istringstream ss(s.substr(p + 1));
    char state;
    long ppid = 0;
    ss >> state >> ppid;
    return (pid_t)ppid;
}

// The Docker container a process runs in ("0::/docker/<64 hex>" in its cgroup),
// by the name Docker stores in the container's config.v2.json.
static std::string container_name(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line, id;
    while (id.empty() && std::getline(f, line)) {
        auto p = line.find("docker");
        if (p == std::string::npos) continue;
        p += 6;
        if (p < line.size() && (line[p] == '/' || line[p] == '-')) ++p;
        if (line.size() - p >= 64 &&
            std::all_of(line.begin() + p, line.begin() + p + 64, [](char c) { return isxdigit((unsigned char)c); }))
            id = line.substr(p, 64);
    }
    if (id.empty()) return "";
    auto it = _container_names.find(id);
    if (it != _container_names.end()) return it->second;
    auto json = read_file("/var/lib/docker/containers/" + id + "/config.v2.json");
    std::string name = id.substr(0, 12);
    auto n = json.find("\"Name\":\"/");     // container names start with a slash
    if (n != std::string::npos) {
        n += 9;
        auto e = json.find('"', n);
        if (e != std::string::npos && e > n) name = json.substr(n, e - n);
    }
    return _container_names[id] = name;
}

// The script a shell or interpreter runs ("/bin/bash .../cache_dirs -p 1" gives
// "cache_dirs"), or "" for anything else, including "bash -c ...". User Scripts
// runs every script as .../NAME/script, so there the folder gives the name.
static std::string script_name(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::vector<std::string> args;
    std::string a;
    while (args.size() < 8 && std::getline(f, a, '\0')) args.push_back(a);
    if (args.size() < 2) return "";
    auto base = [](const std::string& p) { return p.substr(p.rfind('/') + 1); };
    auto interp = base(args[0]);
    static const std::unordered_set<std::string> SHELLS = {"bash", "sh", "dash", "zsh", "php", "perl"};
    if (!SHELLS.count(interp) && interp.rfind("python", 0) != 0) return "";
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-c") return "";
        if (args[i].empty() || args[i][0] == '-') continue;
        auto n = base(args[i]);
        if (n == "script") n = base(args[i].substr(0, args[i].rfind('/')));
        return n;
    }
    return "";
}

static Suspect describe_process(pid_t pid) {
    Suspect s;
    s.pid = pid;
    if (pid == getpid()) { s.label = s.name = "Unspin"; return s; }
    auto ps   = std::to_string(pid);
    auto comm = read_first_line("/proc/" + ps + "/comm");
    if (comm.empty()) {
        s.label  = "unknown (process exited)";
        s.name   = "exited";
        s.detail = "pid " + ps;
        return s;
    }
    auto prog = proc_arg0(pid);
    if (prog.empty()) prog = comm;   // kernel threads have no command line
    // The mover, or a program it started (its rsync).
    pid_t p = pid;
    for (int i = 0; i < 8 && p > 1; ++i, p = proc_ppid(p)) {
        auto a = p == pid ? prog : proc_arg0(p);
        if (a == "mover" || a == "age_mover") {
            s.label = s.name = "Mover";
            if (prog != a) s.detail = prog;
            return s;
        }
    }
    if (auto c = container_name(pid); !c.empty()) {
        s.label  = "container " + c;
        s.name   = c;
        s.detail = prog;
        return s;
    }
    if (comm.rfind("qemu-system", 0) == 0) {
        auto cmd = proc_cmdline(ps);
        auto g = cmd.find("guest=");
        if (g != std::string::npos) {
            auto vm = cmd.substr(g + 6, cmd.find_first_of(", ", g + 6) - (g + 6));
            s.label = "VM " + vm;
            s.name  = vm;
            return s;
        }
    }
    // Samba names the process serving a client "smbd[10.10.10.3]". comm is cut at
    // 15 characters, so the address is used (until smbstatus names the user) only
    // when it's complete.
    if (comm == "smbd" || comm.rfind("smbd[", 0) == 0) {
        s.label = s.name = "SMB";
        auto close = comm.find(']');
        if (close != std::string::npos && close > 5) s.label += " " + comm.substr(5, close - 5);
        s.smb = true;
        s.smb_pid = pid;
        return s;
    }
    if (comm == "shfs") {
        s.label = "user share (requester not found)";
        s.name  = "shfs";
        s.via_shfs = s.from_shfs = true;
        return s;
    }
    if (comm == "nfsd") { s.label = "NFS (client unknown)"; s.name = "NFS"; return s; }
    // A script, or a program a script started: cache_dirs runs find under timeout.
    if (auto sc = script_name(pid); !sc.empty()) { s.label = s.name = sc; return s; }
    p = proc_ppid(pid);
    for (int i = 0; i < 3 && p > 1; ++i, p = proc_ppid(p))
        if (auto sc = script_name(p); !sc.empty()) { s.label = s.name = sc; s.detail = prog; return s; }
    s.label = s.name = prog;
    return s;
}

// SMB user and client of an smbd process, from smbstatus - run only for an
// actual culprit, not for every event.
static void resolve_smb(Suspect& s) {
    s.smb = false;
    FILE* p = popen("smbstatus -p 2>/dev/null", "r");
    if (!p) return;
    char buf[512];
    auto want = std::to_string(s.smb_pid);
    while (fgets(buf, sizeof(buf), p)) {
        std::istringstream ss(buf);
        std::string pid, user, group, machine;
        if (!(ss >> pid >> user >> group >> machine) || pid != want) continue;
        s.label = "SMB " + user + "@" + machine;
        s.name  = user;
    }
    pclose(p);
}

// Containers, VMs and SMB clients mostly use /mnt/user, so the process that
// touches the disk is shfs. The real requester holds the same file open as
// /mnt/user/... (or /mnt/user0/...): during a spin-up it waits ~11 s in its
// read, so a scan of open files finds it. Tried at most once a second per
// suspect while the disk sleeps or wakes, and once more before recording.
static void resolve_shfs(Suspect& s, const DiskState& d) {
    s.shfs_tried_ms = now_ms();
    auto rel = s.path.substr(d.mount.size());
    const std::string targets[] = {"/mnt/user" + rel, "/mnt/user0" + rel};
    // Inside a container the kernel shows the container's own path for an open
    // file (/downloads/... for /mnt/user/qbittorrent/...): match the last two
    // path parts, the folder and the file name.
    auto cut  = s.path.rfind('/', s.path.rfind('/') - 1);
    auto tail = s.path.substr(cut == std::string::npos ? 0 : cut);
    auto ends_with_tail = [&](const char* l, size_t n) {
        return n >= tail.size() && memcmp(l + n - tail.size(), tail.data(), tail.size()) == 0;
    };
    DIR* proc = opendir("/proc");
    if (!proc) return;
    pid_t found = 0;
    struct dirent* pe;
    char link[PATH_MAX];
    while (!found && (pe = readdir(proc)) != nullptr) {
        if (pe->d_name[0] < '0' || pe->d_name[0] > '9') continue;
        auto pid = (pid_t)atol(pe->d_name);
        if (pid == s.pid || pid == getpid()) continue;
        auto fd_dir = std::string("/proc/") + pe->d_name + "/fd";
        DIR* fds = opendir(fd_dir.c_str());
        if (!fds) continue;
        struct dirent* fe;
        while ((fe = readdir(fds)) != nullptr) {
            if (fe->d_name[0] == '.') continue;
            auto n = readlink((fd_dir + "/" + fe->d_name).c_str(), link, sizeof(link) - 1);
            if (n <= 0) continue;
            link[n] = '\0';
            if (targets[0] == link || targets[1] == link || ends_with_tail(link, (size_t)n)) { found = pid; break; }
        }
        closedir(fds);
    }
    closedir(proc);
    if (!found) return;
    auto r     = describe_process(found);
    s.label    = r.label;
    s.name     = r.name;
    s.detail   = r.detail;
    s.smb      = r.smb;
    s.smb_pid  = r.smb_pid;
    s.via_shfs = false;
}

// Each lookup scans every open file on the system, so it only runs while it can
// matter: during a spin-up, or while a read could still turn out to be the cause.
static void resolve_shfs_suspects(DiskState& d, bool force) {
    auto now = now_ms();
    for (auto& s : d.suspects) {
        if (!s.via_shfs) continue;
        auto useful = d.io_seen || now - s.last_ms <= READ_LEAD_MS + 1000;
        if (force || (useful && now - s.shfs_tried_ms >= 1000)) resolve_shfs(s, d);
    }
}

// Whether any thread of the process is in uninterruptible sleep ("D"), told
// apart for kernel threads. Reads only /proc/<pid>/stat files: a process's
// command line lives in its memory, and reading it can wait as long as the
// process itself.
enum class Blocked { no, process, kernel_thread };

static Blocked blocked_state(const std::string& proc_dir) {
    auto s = read_first_line(proc_dir + "/stat");
    auto p = s.rfind(')');
    if (p == std::string::npos) return Blocked::no;
    std::istringstream ss(s.substr(p + 1));
    char state;
    long skip;
    unsigned long flags = 0;
    ss >> state >> skip >> skip >> skip >> skip >> skip >> flags;
    if (flags & 0x00200000)   // PF_KTHREAD: a kernel thread
        return state == 'D' ? Blocked::kernel_thread : Blocked::no;
    if (state == 'D') return Blocked::process;
    DIR* tasks = opendir((proc_dir + "/task").c_str());
    if (!tasks) return Blocked::no;
    bool found = false;
    struct dirent* te;
    while (!found && (te = readdir(tasks)) != nullptr) {
        if (te->d_name[0] == '.') continue;
        auto t = read_first_line(proc_dir + "/task/" + te->d_name + "/stat");
        auto q = t.rfind(')');
        found = q != std::string::npos && q + 2 < t.size() && t[q + 2] == 'D';
    }
    closedir(tasks);
    return found ? Blocked::process : Blocked::no;
}

// What a waiting process waits for, from the kernel stacks of its waiting
// threads. op: the innermost file operation ("looking up a name"). kind:
// "disk" when it waits inside a disk's filesystem, "fuse" when it waits for a
// FUSE filesystem like shfs, so it's the process that asked shfs; empty when
// it waits for something else.
struct BlockedOp { std::string op, kind; };

static BlockedOp blocked_op(const std::string& proc_dir) {
    static const std::pair<const char*, const char*> OPS[] = {
        {"iterate_dir", "listing a directory"},       {"xfs_readdir", "listing a directory"},
        {"__lookup_slow", "looking up a name"},       {"lookup_slow", "looking up a name"},
        {"lookup_open", "looking up a name"},         {"lookup_one_qstr_excl", "looking up a name"},
        {"xfs_dir_lookup", "looking up a name"},      {"fuse_lookup", "looking up a name"},
        {"vfs_getxattr", "reading extended attributes"}, {"__vfs_getxattr", "reading extended attributes"},
        {"vfs_getattr", "reading attributes"},        {"vfs_getattr_nosec", "reading attributes"},
        {"vfs_statx", "reading attributes"},
        {"vfs_read", "reading"},                      {"filemap_read", "reading"},
        {"vfs_write", "writing"},                     {"vfs_fsync_range", "writing"},
    };
    static const char* DISK_FS[] = {"xfs_", "zfs_", "zpl_", "btrfs_", "ext4_"};
    BlockedOp out;
    DIR* tasks = opendir((proc_dir + "/task").c_str());
    if (!tasks) return out;
    struct dirent* te;
    while (out.kind.empty() && (te = readdir(tasks)) != nullptr) {
        if (te->d_name[0] == '.') continue;
        auto t = proc_dir + "/task/" + te->d_name;
        auto st = read_first_line(t + "/stat");
        auto q = st.rfind(')');
        if (q == std::string::npos || q + 2 >= st.size() || st[q + 2] != 'D') continue;
        // "[<0>] xfs_dir_lookup+0x5e/0xb0", innermost call first
        std::ifstream f(t + "/stack");
        std::string line, op, kind;
        while (std::getline(f, line)) {
            auto a = line.find("] ");
            if (a == std::string::npos) continue;
            auto fn = line.substr(a + 2, line.find('+', a) - (a + 2));
            if (kind.empty()) {
                // fuse_dev_*: a FUSE server (shfs) waiting for its next request
                if (fn == "request_wait_answer" || (fn.rfind("fuse_", 0) == 0 && fn.rfind("fuse_dev_", 0) != 0))
                    kind = "fuse";
                for (auto* p : DISK_FS) if (fn.rfind(p, 0) == 0) kind = "disk";
            }
            if (op.empty())
                for (const auto& [name, what] : OPS) if (fn == name) { op = what; break; }
        }
        if (!kind.empty()) out = {op.empty() ? "accessing" : op, kind};
    }
    closedir(tasks);
    return out;
}

// A kernel thread working for this disk's array device: its name ends with
// "/md2p1" (or "/md2" before Unraid 6.12) for disk2.
static bool is_disk_kthread(const std::string& comm, const DiskState& d) {
    if (d.name.rfind("disk", 0) != 0) return false;
    auto md = "/md" + d.name.substr(4);
    auto ends = [&](const std::string& s) {
        return comm.size() >= s.size() && comm.compare(comm.size() - s.size(), s.size(), s) == 0;
    };
    return ends(md + "p1") || ends(md);
}

// A directory listing or a file check on a sleeping disk produces no file
// event, but the process behind it waits in uninterruptible sleep until the
// disk has spun up (~11 s). Scanned once a second during the spin-up: waiting
// processes with a directory or file of this disk or of a user share open, or
// as their working directory, explain a wake-up that has no file event. So
// does, as a last resort, a process with no such path that waits inside a
// disk's filesystem (shfs looking up a name) or for shfs (its requester),
// unless it has a path on another disk or pool open: then it waits for that one.
static void scan_blocked(DiskState& d) {
    d.blocked_scan_ms = now_ms();
    DIR* proc = opendir("/proc");
    if (!proc) return;
    std::string seen, others;   // others: waiting too, but not for this disk
    char link[PATH_MAX];
    struct dirent* pe;
    while ((pe = readdir(proc)) != nullptr) {
        if (pe->d_name[0] < '0' || pe->d_name[0] > '9') continue;
        auto pid = (pid_t)atol(pe->d_name);
        auto dir = std::string("/proc/") + pe->d_name;
        if (pid == getpid()) continue;
        auto blocked = blocked_state(dir);
        if (blocked == Blocked::no) continue;
        if (blocked == Blocked::kernel_thread) {
            // XFS writing metadata changes (its journal, or the changes in place)
            // to exactly this disk: "kworker/6:2+xfs-sync/md2p1", "xfsaild/md2p1".
            auto comm = read_first_line(dir + "/comm");
            if (comm.find("xfs") != std::string::npos && is_disk_kthread(comm, d)) {
                d.metadata_write = true;
                seen += " " + comm + "(" + pe->d_name + ") writing metadata";
            }
            continue;
        }
        int rank = 0;
        bool elsewhere = false;   // a path on another disk or pool
        std::string where;
        auto consider = [&](const std::string& lp) {
            auto n = readlink(lp.c_str(), link, sizeof(link) - 1);
            if (n <= 0) return;
            std::string l(link, (size_t)n);
            int r = 0;
            if (l == d.mount || l.compare(0, d.mount.size() + 1, d.mount + "/") == 0)  r = 3;
            else if (l.rfind("/mnt/user/", 0) == 0 || l.rfind("/mnt/user0/", 0) == 0) r = 2;
            else if (l.rfind("/mnt/", 0) == 0) elsewhere = true;
            if (r > rank) { rank = r; where = l; }
        };
        if (DIR* fds = opendir((dir + "/fd").c_str())) {
            struct dirent* fe;
            while (rank < 3 && (fe = readdir(fds)) != nullptr)
                if (fe->d_name[0] != '.') consider(dir + "/fd/" + fe->d_name);
            closedir(fds);
        }
        if (rank < 3) consider(dir + "/cwd");
        auto op   = blocked_op(dir);
        auto comm = read_first_line(dir + "/comm");
        if (comm == "shfs" && op.kind == "fuse") op = {};   // shfs serving, not asking
        // shfs serves every disk and pool, so it always has some of them open
        if (!rank && (!elsewhere || comm == "shfs")) rank = op.kind == "fuse" ? 2 : op.kind == "disk" ? 1 : 0;
        if (!rank) {
            others += " " + comm + "(" + pe->d_name + ")" + (op.kind.empty() ? "" : " " + op.op);
            continue;
        }
        seen += " " + comm + "(" + pe->d_name + ")" + (op.kind.empty() ? "" : " " + op.op) +
                (where.empty() ? "" : " " + qpath(where));
        if (std::any_of(d.blocked.begin(), d.blocked.end(),
                                 [&](const Suspect& b) { return b.pid == pid; }))
            continue;
        auto s   = describe_process(pid);
        s.at_ms  = s.last_ms = d.blocked_scan_ms;
        s.access = op.kind.empty() ? "accessing" : op.op;
        s.path   = where;
        if (s.via_shfs && !where.empty()) {   // shfs holds the disk path; its requester the user share path
            resolve_shfs(s, d);
            rank = s.via_shfs ? 1 : 2;
        }
        s.blocked_rank = rank;
        if (d.blocked.size() < SUSPECTS_MAX) d.blocked.push_back(std::move(s));
    }
    closedir(proc);
    if (seen.empty()) seen = " nothing of this disk";
    if (seen != d.blocked_logged)
        log_debug(d.name + " spinning up, waiting:" + seen + (others.empty() ? "" : " - also waiting:" + others));
    d.blocked_logged = seen;
}

// Called for every file event, before any filtering: a wake-up is about the
// disk, so excluded shares count too, and it keeps running while paused.
static void note_access(const std::string& path, pid_t pid, uint64_t mask) {
    if (!_cfg.track_wakeups) return;
    auto d = disk_for_path(path);
    if (!d || !(d->spundown || d->woke_at_ms)) return;   // awake: the cheap, common case
    // Unspin promotes only from a disk that is spinning up or awake: its own
    // accesses then can't be the cause. Before that they'd be a bug, so they count.
    if (pid == getpid() && (d->io_seen || !d->spundown)) return;
    const char* access = (mask & (FAN_MODIFY | FAN_CLOSE_WRITE)) ? "writing"
                       : (mask & FAN_ACCESS)                     ? "reading"
                       :                                           "opening";
    // One suspect per process (per file for shfs, which serves everyone) and
    // burst of activity; a later read or write of the same file says more than
    // the open before it. After a pause it's a new access, maybe by someone else.
    auto now = now_ms();
    for (auto it = d->suspects.begin(); it != d->suspects.end(); ++it) {
        auto& s = *it;
        if (s.pid != pid || (s.from_shfs && s.path != path)) continue;
        if (now - s.last_ms > READ_LEAD_MS) { d->suspects.erase(it); break; }
        s.last_ms = now;
        if (s.path == path && (s.access == "opening" || (s.access == "reading" && access[0] == 'w')))
            s.access = access;
        // shfs reads once the requester's open is done: now it holds the file.
        if (s.via_shfs && now - s.shfs_tried_ms >= 250) resolve_shfs(s, *d);
        return;
    }
    auto s    = describe_process(pid);
    s.at_ms   = s.last_ms = now;
    s.access  = access;
    s.path    = path;
    if (s.via_shfs) resolve_shfs(s, *d);
    if (d->suspects.size() >= SUSPECTS_MAX) d->suspects.erase(d->suspects.begin());
    d->suspects.push_back(std::move(s));
}

static std::string wake_field(std::string s) {
    std::replace(s.begin(), s.end(), '\t', ' ');
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

static std::string wake_date(time_t t) {
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", &tm_buf);
    return date;
}

// One "C date disk label count" line per day, disk and culprit, and one
// "W epoch disk label name access path also" line per recent wake-up.
static bool write_wakeups(const std::string& path) {
    std::string out;
    for (const auto& [key, n] : _wake_counts)
        out += "C\t" + std::get<0>(key) + '\t' + std::get<1>(key) + '\t' + std::get<2>(key) + '\t' +
               std::to_string(n) + '\n';
    for (const auto& w : _wake_recent)
        out += "W\t" + std::to_string(w.at) + '\t' + w.disk + '\t' + w.label + '\t' + w.name + '\t' +
               w.access + '\t' + w.path + '\t' + w.also + '\n';
    return write_file_atomic(path, out);
}

static void check_wakeups_reset() {
    if (unlink(WAKE_RESET.c_str()) != 0) return;
    _wake_counts.clear();
    _wake_recent.clear();
    _wake_flash_dirty = false;
    unlink(WAKE_LIVE.c_str());
    unlink(WAKE_FILE.c_str());
    log_info("Disk wake-up statistics reset");
}

static void load_wakeups() {
    check_wakeups_reset();
    _wake_flash_last = time(nullptr);
    // As with the promotion statistics: the RAM copy is never older.
    auto live = access(WAKE_LIVE.c_str(), F_OK) == 0;
    _wake_flash_dirty = live;
    std::ifstream f(live ? WAKE_LIVE : WAKE_FILE);
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> v;
        std::istringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) v.push_back(field);
        if (line.size() && line.back() == '\t') v.push_back("");   // empty last field
        try {
            if (v.size() == 5 && v[0] == "C")
                _wake_counts[{v[1], v[2], v[3]}] = std::stoll(v[4]);
            else if (v.size() == 8 && v[0] == "W")
                _wake_recent.push_back({(time_t)std::stoll(v[1]), v[2], v[3], v[4], v[5], v[6], v[7]});
        } catch (...) {}   // a damaged line is skipped, not fatal
    }
}

// force: write the flash copy now (on exit) instead of waiting for the interval.
static void flush_wakeups(bool force = false) {
    check_wakeups_reset();
    auto now = time(nullptr);
    if (_wake_flash_dirty && (force || now - _wake_flash_last >= 60 * 60)) {
        _wake_flash_last = now;
        if (write_wakeups(WAKE_FILE)) _wake_flash_dirty = false;
    }
}

static void record_wakeup(DiskState& d) {
    d.woke_at_ms = 0;
    if (_cfg.track_wakeups) resolve_shfs_suspects(d, true);
    auto suspects = std::move(d.suspects);
    auto blocked  = std::move(d.blocked);
    auto metadata_write = d.metadata_write;
    d.suspects.clear();
    d.blocked.clear();
    d.metadata_write = false;
    if (!_cfg.track_wakeups) return;

    // A read or open wakes the disk within moments; only a buffered write
    // reaches it much later. An older read was served from RAM.
    auto lead_ok = [&](const Suspect& s) {
        auto lead = d.io_at_ms - s.last_ms;   // negative: still active when the I/O began
        return lead <= (s.access == "writing" ? WRITE_LEAD_MS : READ_LEAD_MS);
    };
    Suspect* pick = nullptr;
    if (d.io_seen && d.io_at_ms) {
        for (auto& s : suspects)                    // 1. latest before the spin-up started
            if (s.at_ms <= d.io_at_ms + 1000 && lead_ok(s) && (!pick || s.last_ms >= pick->last_ms))
                pick = &s;
        if (!pick)
            for (auto& s : suspects)                // 2. first after
                if (s.at_ms > d.io_at_ms + 1000) { pick = &s; break; }
        if (!pick)                                  // 3. waiting for the disk, without a file event
            for (auto& s : blocked)
                if (!pick || s.blocked_rank > pick->blocked_rank) pick = &s;
    }

    WakeEntry w;
    w.at   = time(nullptr);
    w.disk = d.name;
    std::string msg = "Wake-up: " + d.name;
    if (pick) {
        if (pick->smb) resolve_smb(*pick);
        w.label  = pick->label;
        w.name   = pick->name;
        w.access = pick->access;
        w.path   = pick->path;
        msg += " by " + pick->label + (pick->detail.empty() ? "" : " (" + pick->detail + ")") +
               " " + pick->access + (pick->path.empty() ? "" : " " + qpath(pick->path));
    } else if (d.io_seen && (metadata_write || parity_woke_with(d))) {
        // Nobody waited, and XFS wrote metadata or parity woke too: a change
        // kept in RAM was written out later, one without a file write event
        // (times, attributes, a new, renamed or deleted file or folder).
        w.label = "metadata change";
        w.name  = "metadata";
        msg += std::string(" - metadata change written later (times, attributes, create, rename or delete; ") +
               (metadata_write ? "XFS wrote it" : "parity woke too") + ")";
    } else if (d.io_seen) {
        w.label = "no file opened";
        w.name  = "dir listing";
        msg += " - no file opened (directory listing or metadata)";
    } else {
        w.label = "no I/O seen";
        w.name  = "no I/O";
        msg += " - no I/O seen (spin-up command, SMART or similar)";
    }
    std::vector<std::string> others;
    for (const auto* list : {&suspects, &blocked})
        for (const auto& s : *list)
            if (&s != pick && (!pick || s.label != pick->label) &&
                std::find(others.begin(), others.end(), s.label) == others.end())
                others.push_back(s.label);
    for (const auto& o : others) w.also += (w.also.empty() ? "" : ", ") + o;
    if (!w.also.empty()) msg += " (also active: " + w.also + ")";

    if (pick && pick->pid == getpid()) log_warn(msg + " - Unspin should never wake a disk, please report this");
    else                               log_info(msg);

    for (auto* f : {&w.label, &w.name, &w.access, &w.path, &w.also}) *f = wake_field(*f);
    check_wakeups_reset();
    _wake_counts[{wake_date(w.at), w.disk, w.label}]++;
    _wake_recent.push_back(std::move(w));
    auto n = std::count_if(_wake_recent.begin(), _wake_recent.end(),
                           [&](const WakeEntry& e) { return e.disk == d.name; });
    for (auto it = _wake_recent.begin(); n > (long)WAKE_RECENT_PER_DISK && it != _wake_recent.end();)
        if (it->disk == d.name) { it = _wake_recent.erase(it); --n; } else ++it;
    _wake_flash_dirty = true;
    write_wakeups(WAKE_LIVE);   // a wake-up is rare: the page shows it right away
}

// ---------------------------------------------------------------------------
// Promotion statistics
// ---------------------------------------------------------------------------

// One line per day and pool with promotions: "YYYY-MM-DD\tpool\tfiles\tbytes".
// The settings page reads STATS_LIVE (RAM), written at most once per
// PERIODIC_CHECK_INTERVAL_SEC. STATS_FILE on the flash survives reboots; to
// spare the flash drive it's written at most once per STATS_FLASH_INTERVAL_SEC
// and on exit (Unraid sends SIGTERM before unmounting the flash at shutdown).
static const std::string STATS_LIVE  = "/var/run/unspind.stats";
static const std::string STATS_FILE  = "/boot/config/plugins/unspin/stats";
// Created by the settings page's reset button (which also deletes both files):
// drop the in-memory statistics before the next write would bring them back.
static const std::string STATS_RESET = "/var/run/unspind.stats-reset";
static constexpr int     STATS_FLASH_INTERVAL_SEC = 60 * 60;

struct DayStats { int64_t files = 0; int64_t bytes = 0; };
static std::map<std::pair<std::string, std::string>, DayStats> _stats;   // (date, pool) -> totals
static bool   _stats_live_dirty  = false;
static bool   _stats_flash_dirty = false;
static time_t _stats_flash_last  = 0;

static void check_stats_reset() {
    if (unlink(STATS_RESET.c_str()) != 0) return;
    _stats.clear();
    _stats_live_dirty = _stats_flash_dirty = false;
    unlink(STATS_LIVE.c_str());
    unlink(STATS_FILE.c_str());
    log_info("Promotion statistics reset");
}

static void load_stats() {
    check_stats_reset();
    _stats_flash_last = time(nullptr);
    // The RAM copy is never older than the flash one: it's only missing after
    // a reboot, and newer when the daemon died without writing to the flash.
    auto live = access(STATS_LIVE.c_str(), F_OK) == 0;
    _stats_flash_dirty = live;
    std::ifstream f(live ? STATS_LIVE : STATS_FILE);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string date, pool;
        DayStats d;
        if (ss >> date >> pool >> d.files >> d.bytes) _stats[{date, pool}] = d;
    }
}

static void record_promotion(const std::string& pool, int64_t size) {
    check_stats_reset();
    auto now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", &tm_buf);
    auto& d = _stats[{date, pool}];
    d.files++;
    d.bytes += size;
    _stats_live_dirty = _stats_flash_dirty = true;
}

static bool write_stats(const std::string& path) {
    std::string out;
    for (const auto& [key, d] : _stats)
        out += key.first + '\t' + key.second + '\t' + std::to_string(d.files) + '\t' +
               std::to_string(d.bytes) + '\n';
    return write_file_atomic(path, out);
}

// force: write the flash copy now (on exit) instead of waiting for the interval.
static void flush_stats(bool force = false) {
    check_stats_reset();
    if (_stats_live_dirty && write_stats(STATS_LIVE))
        _stats_live_dirty = false;
    auto now = time(nullptr);
    if (_stats_flash_dirty && (force || now - _stats_flash_last >= STATS_FLASH_INTERVAL_SEC)) {
        _stats_flash_last = now;
        if (write_stats(STATS_FILE)) _stats_flash_dirty = false;
    }
}

// holder gets "comm(pid)" for in_use, which the caller logs.
static Handover promote_file(const std::string& src, int64_t size, const std::string& pool,
                             const PromoteDecision& dec, int total_reads, std::string& holder) {
    auto dst = resolve_destination(src);
    auto tag = access_tag(total_reads, true);

    if (_cfg.dry_run) {
        log_info("[" + dec.rule + "] [DRY RUN] Would promote " +
                 qpath(src) + " -> " + dst + " (" + human_size(size) + ") - " + dec.reason + tag);
        return Handover::done;
    }

    // Open somewhere: no copy now (move_to_cache checks again after the copy).
    struct stat st;
    if (lstat(src.c_str(), &st) == 0) {
        holder = file_holder(src, st.st_dev, st.st_ino);
        if (!holder.empty()) return Handover::in_use;
    }

    log_info("[" + dec.rule + "] Promoting " + qpath(src) + " -> " + dst +
             " (" + human_size(size) + ") - " + dec.reason + tag);

    std::string why;
    auto not_yet = access_tag(total_reads, false);
    auto result = move_to_cache(src, dst, why);
    switch (result) {
    case Handover::done:
        record_promotion(pool, size);
        break;
    case Handover::gone:
        log_info("[" + dec.rule + "] Moved or deleted during the promotion, no copy kept: " + qpath(src) + not_yet);
        break;
    case Handover::changed:
        log_info("[" + dec.rule + "] Changed during the promotion, left on the array for a later access: " +
                 qpath(src) + not_yet);
        break;
    case Handover::in_use:
        holder = why;
        break;
    case Handover::failed:
        log_err(why);
        break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Promotion rules
// ---------------------------------------------------------------------------

static PromoteDecision should_promote(const AccessRecord& rec, int64_t size) {
    PromoteDecision d;

    if (size <= _cfg.small_file_threshold) { // it's a small one!
        if (_cfg.rule1_enabled) {
            // Rule 1 - small files: promote after SMALL_MIN_ACCESSES total reads
            if (rec.total_reads >= _cfg.small_min_accesses) {
                d.promote = true;
                d.rule    = RULE1;
                d.reason  = std::to_string(rec.total_reads) + "/" +
                            std::to_string(_cfg.small_min_accesses) +
                            " reads, size " + human_size(size) +
                            " <= " + human_size(_cfg.small_file_threshold) + " threshold";
            } else {
                d.reason = std::to_string(rec.total_reads) +
                           "/" + std::to_string(_cfg.small_min_accesses) + " reads";
            }
            if (!_cfg.rule1_fallthrough) return d;
            if (d.promote) return d; // already promoting, no need to check further
            // fallthrough enabled and rule1 not met - evaluate large-file rules below
        } else {
            // rule1 disabled - check fallthrough
            if (!_cfg.rule1_fallthrough) {
                d.reason = "rule 1 - small: disabled, fallthrough off";
                return d;
            }
            // fallthrough enabled - evaluate large-file rules below
        }
    }
    // so we're packing a big one or we fell through

    // Rules 2 & 3 - sliding windows
    auto now          = time(nullptr);
    auto short_cutoff = now - (time_t)_cfg.large_short_window_mins * 60;
    auto long_cutoff  = now - (time_t)_cfg.large_long_window_hours * 3600;

    auto short_count = 0, long_open_count = 0;
    for (const time_t& ts : rec.read_timestamps)
        if (ts >= short_cutoff) ++short_count;
    for (const time_t& ts : rec.open_timestamps)
        if (ts >= long_cutoff)  ++long_open_count;

    // Rule 2 - short burst (e.g. 100 reads in 5 min = streaming video)
    if (_cfg.rule2_enabled && short_count >= _cfg.large_short_min_accesses) {
        d.promote = true;
        d.rule    = RULE2;
        d.reason  = std::to_string(short_count) + "/" +
                    std::to_string(_cfg.large_short_min_accesses) +
                    " reads in " + std::to_string(_cfg.large_short_window_mins) + " min";
        return d;
    }
    // Rule 3 - periodic opens (e.g. 3 opens in 24 h = regularly consulted PDF)
    if (_cfg.rule3_enabled && long_open_count >= _cfg.large_long_min_accesses) {
        d.promote = true;
        d.rule    = RULE3;
        d.reason  = std::to_string(long_open_count) + "/" +
                    std::to_string(_cfg.large_long_min_accesses) +
                    " opens in " + std::to_string(_cfg.large_long_window_hours) + " h";
        return d;
    }

    // Not promoting - build reason showing current counts vs thresholds
    std::string r;
    r += _cfg.rule2_enabled
       ? std::to_string(short_count) + "/" + std::to_string(_cfg.large_short_min_accesses) +
         " reads/" + std::to_string(_cfg.large_short_window_mins) + " min"
       : "r2 off";
    r += "; ";
    r += _cfg.rule3_enabled
       ? std::to_string(long_open_count) + "/" + std::to_string(_cfg.large_long_min_accesses) +
         " opens/" + std::to_string(_cfg.large_long_window_hours) + " h"
       : "r3 off";
    d.reason = r;
    return d;
}

// Attempt an immediate promotion (transfer / fill checks included). Shared by
// handle_event() (right after a rule fires) and promote_stale_pending() (recovering a
// promotion that's been stuck pending on a file whose open_count never made
// it back to zero - see PENDING_PROMOTE_STALE_SEC).
static void try_promote(const std::string& path, int64_t size, const std::string& sname,
                         AccessRecord& rec, const PromoteDecision& dec) {
    if (rec.promoted) return;
    rec.pending_promote = false;  // this attempt is the deferred one, too
    if (auto running = transfers_running(); !running.empty()) {
        log_info("[" + dec.rule + "] " + running + " active, deferring: " + path);
        return;
    }
    // The access came from RAM while the disk sleeps. Removing the array copy
    // would wake it (and parity); the rule fires again on a later access.
    if (auto disk = sleeping_disk(path)) {
        auto msg = "[" + dec.rule + "] " + disk->name +
                   " is spun down, not waking it - promoting on a later access: " + qpath(path);
        if (rec.asleep_logged != disk->spundown_at) { log_info(msg); rec.asleep_logged = disk->spundown_at; }
        else                                        { log_debug(msg); }
        return;
    }

    const auto& pool_name = _shares.at(sname).cache_pool;
    auto pit = _pools.find(pool_name);
    auto limit = (pit != _pools.end()) ? pit->second.max_fill_percent
                                       : _cfg.default_max_fill_percent;
    auto pool_path = MNT_PREFIX + pool_name;
    auto fill = disk_fill_percent(pool_path);
    if (fill >= limit) {
        auto msg = "[" + dec.rule + "] Pool " + pool_path + " " +
                   std::to_string((int)fill) + "% full, skipping: " + path;
        auto& last = _last_pool_full_msg[pool_path];
        if (msg != last) { log_info(msg); last = msg; }
        else             { log_debug(msg); }
        return;
    }
    std::string holder;
    auto result = promote_file(path, size, pool_name, dec, rec.total_reads, holder);
    if (result == Handover::in_use) {
        // Wait for a close instead of trying again on every read.
        auto msg = "[" + dec.rule + "] Open in " + holder + ", promoting once it is closed: " +
                   qpath(path) + access_tag(rec.total_reads, false);
        if (holder != rec.held_by) { log_info(msg); rec.held_by = holder; }
        else                       { log_debug(msg); }
        rec.held_elsewhere  = true;
        rec.held_at         = time(nullptr);
        rec.pending_promote = true;
        rec.pending_dec     = dec;
        return;
    }
    rec.held_by.clear();
    if (result == Handover::done) rec.promoted = true;
}

// ---------------------------------------------------------------------------
// Access event handler
// ---------------------------------------------------------------------------

static void handle_event(const std::string& path, EvType ev) {
    if (path.empty()) return;

    // Must be in a watches scan path
    auto in_scope = false;
    for (const auto& sp : _cfg.scan_paths)
        if (path.rfind(sp, 0) == 0) { in_scope = true; break; }
    if (!in_scope) return;

    // Unspin's own hidden name during a hand-over (see hidden_name()).
    if (path.compare(path.rfind('/') + 1, 8, ".unspin.") == 0) return;

    // Already on a known cache pool mount? Config validation rejects cache-pool
    // paths in SCAN_PATHS (see load_config()), but that only catches pools known
    // at config-load time; a pool added/renamed later, or a config file edited
    // while the daemon isn't running, can still slip a pool path through. Without
    // this guard, files already on cache would flow into the promotion logic below.
    if (path.rfind(MNT_PREFIX, 0) == 0) {
        auto end = path.find('/', MNT_PREFIX.size());
        if (end != std::string::npos) {
            auto top = path.substr(MNT_PREFIX.size(), end - MNT_PREFIX.size());
            if (_pools.count(top)) return;
        }
    }

    // Excluded patterns
    for (const auto& ex : _cfg.exclude_patterns)
        if (!ex.empty() && path.find(ex) != std::string::npos) return;

    // Share must be promotable (use_cache=yes|prefer, has pool, not user-excluded)
    auto sname = path_share_name(path);
    if (sname.empty() || !share_promotable(sname)) {
        if (_cfg.log_excluded_shares) log_debug("[skip share] " + qpath(path));
        return;
    }

    auto skip_counting = is_paused() || transfers_active();

    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        // On close of a deleted/missing file: clean up open tracking.
        if (ev == EvType::CloseNoWrite || ev == EvType::CloseWrite) {
            auto it = _access_map.find(path);
            if (it != _access_map.end()) {
                auto& r = it->second;
                if (r.open_count > 0) r.open_count--;
                r.open_reads = -1;
            }
        }
        return;
    }
    if (!S_ISREG(st.st_mode)) return;
    auto size = (int64_t)st.st_size;

    // FAN_MODIFY: flag that a write-open is active; don't create map entry if file isn't tracked yet.
    if (ev == EvType::Modify) {
        auto it = _access_map.find(path);
        if (it != _access_map.end() && it->second.open_count > 0)
            it->second.has_write_open = true;
        return;
    }

    auto& rec = _access_map[path];

    if (rec.promoted) {
        // Verify the hot copy still exists - it may have been deleted and the file put back.
        auto dst = resolve_destination(path);
        if (access(dst.c_str(), F_OK) == 0) {
            if (ev == EvType::Read || ev == EvType::Open)
                log_debug("[already hot] " + qpath(path) + access_tag(rec.total_reads, true));
            return;
        }
        // Hot copy is gone
        log_debug("[promoted -> reset] hot copy gone, re-evaluating: " + path);
        rec.promoted        = false;
        rec.pending_promote = false;
    }

    auto now = time(nullptr);
    rec.last_event = now;

    // Prune old timestamps, evaluate rules, then either promote or defer until file is closed.
    auto evaluate = [&]() {
        auto prune = now - (time_t)_cfg.large_long_window_hours * 3600;
        while (!rec.read_timestamps.empty() && rec.read_timestamps.front() < prune)
            rec.read_timestamps.pop_front();
        while (!rec.open_timestamps.empty() && rec.open_timestamps.front() < prune)
            rec.open_timestamps.pop_front();

        auto dec = should_promote(rec, size);
        if (!dec.promote) return;

        // Defer promotion while any handle is open. Unlinking the source while a reader
        // (SMB/NFS/local) holds an fd causes the client to lose the file — shfs won't
        // redirect to the cache copy until the next open.
        if (rec.open_count > 0 || rec.held_elsewhere) {
            auto first = !rec.pending_promote;
            rec.pending_promote = true;
            rec.pending_dec = dec;
            if (first)
                log_debug("[" + dec.rule + "] Deferred (file open): " +
                          path + " (" + human_size(size) + ") - " + dec.reason);
            return;
        }
        try_promote(path, size, sname, rec, dec);
    };

    if (ev == EvType::Read) {
        if (skip_counting) return;
        rec.total_reads++;
        rec.read_timestamps.push_back(now);
        if (rec.open_reads >= 0) rec.open_reads++;
        auto dec = should_promote(rec, size);
        log_debug("Read #" + std::to_string(rec.total_reads) + " " + qpath(path) +
                  " (" + human_size(size) + ") - " + dec.reason +
                  access_tag(rec.total_reads, false));
        evaluate();

    } else if (ev == EvType::Open) {
        rec.open_count++;
        if (skip_counting) return;
        rec.open_reads = 0;
        if (_cfg.rule3_min_reads == 0) {
            // Filter disabled - count immediately
            rec.open_timestamps.push_back(now);
            auto dec = should_promote(rec, size);
            log_debug("Open " + qpath(path) + " (" + human_size(size) + ") - " + dec.reason +
                      access_tag(rec.total_reads, false));
            evaluate();
        } else {
            log_debug("Open (pending, min reads: " + std::to_string(_cfg.rule3_min_reads) +
                      ") " + qpath(path) + " (" + human_size(size) + ")" +
                      access_tag(rec.total_reads, false));
        }

    } else { // CloseNoWrite or CloseWrite
        // Decrement open count; clamp at 0 (file may have been opened before daemon start) (ask me how I know...).
        if (rec.open_count > 0) rec.open_count--;
        // Clear if done.
        if (rec.open_count == 0) rec.has_write_open = false;
        // A close may be the holder's: try again, at most every HELD_RETRY_SEC
        // (promote_stale_pending() catches a close in between).
        if (rec.held_elsewhere && now - rec.held_at >= HELD_RETRY_SEC) rec.held_elsewhere = false;

        if (skip_counting) {
            rec.open_reads = -1;
            return;
        }

        if (ev == EvType::CloseNoWrite) {
            // Rule 3 "thumbnail" filter: evaluate the completed read-only open session.
            // This is done especially for the Windows explorer (but mybe other file managers, too):
            // they always keep reading and opening files just to make... something sure ;-)
            if (rec.open_reads >= 0) {
                auto reads = rec.open_reads;
                rec.open_reads = -1;

                auto count_open = (_cfg.rule3_min_reads == 0)
                               || (reads == 0)
                               || (reads >= _cfg.rule3_min_reads);
                if (!count_open) {
                    log_debug("Close (" + std::to_string(reads) + " reads < " +
                              std::to_string(_cfg.rule3_min_reads) + " min) skipped: " + path);
                } else {
                    rec.open_timestamps.push_back(now);
                    auto reads_str = (reads == 0) ? "mmap" : std::to_string(reads) + " reads";
                    auto dec = should_promote(rec, size);
                    log_debug("Close (" + reads_str + ") " + path +
                              " (" + human_size(size) + ") - " + dec.reason);
                    evaluate();
                }
            } else {
                // A previous close already ended the open session (several handles
                // open at once). Logged so open_count drift is visible (GH #10).
                log_debug("Close (outside open session) " + path + " (" + human_size(size) +
                          ") open_count=" + std::to_string(rec.open_count));
            }

            // Execute any promotion deferred while the file was open.
            if (!rec.promoted && rec.pending_promote && rec.open_count == 0 && !rec.held_elsewhere) {
                PromoteDecision dec = rec.pending_dec;
                rec.pending_promote = false;
                try_promote(path, size, sname, rec, dec);
            }
        } else {
            // CloseWrite: file was modified - only update open tracking, no promotion.
            rec.open_reads = -1;
        }
    }
}

// Lazily clean up the access map from old entries
static void maybe_cleanup() {
    auto now = time(nullptr);
    if (now - _last_cleanup < CLEANUP_INTERVAL_SEC) return;
    _last_cleanup = now;

    auto cutoff = now - (time_t)_cfg.large_long_window_hours * 3600;
    auto before = _access_map.size();

    for (auto it = _access_map.begin(); it != _access_map.end(); ) {
        // Remove promoted entries and entries that haven't been accessed within the window
        if (it->second.promoted || it->second.last_event < cutoff) {
            it = _access_map.erase(it);
            continue;
        }
        ++it;
    }

    log_info("Access map cleanup: " + std::to_string(before) + " -> " +
             std::to_string(_access_map.size()) + " entries");
}

// A deferred promotion can get stuck forever if open_count drifts out of sync
// with reality - e.g. fanotify pairing more Opens than Closes for the same path
// (seen with repeated Explorer icon-extraction opens over SMB, and with the
// kernel merging identical queued close events), so open_count never returns
// to 0 and the close-triggered promotion never runs. If the path has gone
// completely quiet for a while, nothing is realistically still reading it, so
// push the promotion through anyway instead of waiting forever.
// Called every PERIODIC_CHECK_INTERVAL_SEC (GH #10).
static void promote_stale_pending() {
    if (is_paused()) return;
    auto now = time(nullptr);
    for (auto& [path, rec] : _access_map) {
        if (!rec.pending_promote) continue;
        // Found open elsewhere: its close may have come between two retries.
        auto held = rec.held_elsewhere && rec.open_count == 0 && now - rec.held_at >= HELD_RETRY_SEC;
        if (!held && now - rec.last_event < PENDING_PROMOTE_STALE_SEC) continue;
        // Even the lstat() below could wake a sleeping disk once the inode
        // left the cache. Dropped like a transfer deferral: the next access
        // with the disk awake re-arms it.
        if (auto disk = sleeping_disk(path)) {
            rec.pending_promote = false;
            log_debug("[" + rec.pending_dec.rule + "] Stale pending promotion dropped, " +
                      disk->name + " is spun down: " + path);
            continue;
        }
        struct stat st;
        auto sname = path_share_name(path);
        if (lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            !sname.empty() && share_promotable(sname)) {
            PromoteDecision dec = rec.pending_dec;
            rec.pending_promote = false;
            rec.held_elsewhere  = false;
            if (!held)
                log_info("[" + dec.rule + "] Stale pending promotion (" +
                         std::to_string(PENDING_PROMOTE_STALE_SEC / 60) +
                         " min quiet, open_count=" + std::to_string(rec.open_count) +
                         "), promoting anyway: " + path);
            try_promote(path, (int64_t)st.st_size, sname, rec, dec);
        } else {
            rec.pending_promote = false; // gone, no longer a regular file, or no longer promotable
        }
    }
}

// ---------------------------------------------------------------------------
// fanotify
// ---------------------------------------------------------------------------

// A scan path is "ready" only when it is a genuine mount point, not merely an
// existing directory on rootfs. Marking a non-mounted dir with FAN_MARK_MOUNT
// would watch the wrong (root) filesystem, so compare its st_dev to its parent's.
static bool is_mounted(const std::string& path) {
    struct stat st, parent;
    if (stat(path.c_str(), &st) != 0)               return false;  // ENOENT = not there yet
    if (stat((path + "/..").c_str(), &parent) != 0) return false;
    return st.st_dev != parent.st_dev;                             // differs => mount point
}

// Wrap a string in single quotes for safe use in a shell command line.
static std::string sh_squote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

// Raise an entry in Unraid's notification system. importance: normal|warning|alert.
// No-op when the notify helper is absent (e.g. dev box / not Unraid).
static void notify_unraid(const char* importance,
                          const std::string& subject,
                          const std::string& description) {
    static const char* NOTIFY = "/usr/local/emhttp/webGui/scripts/notify";
    if (access(NOTIFY, X_OK) != 0) return;
    std::string cmd = std::string(NOTIFY)
        + " -e 'Unspin' -i " + sh_squote(importance)
        + " -s " + sh_squote(subject)
        + " -d " + sh_squote(description)
        + " > /dev/null 2>&1";
    if (system(cmd.c_str()) != 0) log_warn("notify failed: " + subject);
}

// FAN_MARK_MOUNT watches the entire mount that contains this path.
// Events are then filtered by path prefix in handle_event().
static bool mark_path(int fd, const std::string& path) {
    return fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
                         FAN_WATCH_MASK, AT_FDCWD, path.c_str()) == 0;
}

// Datasets Unraid auto-creates for a ZFS array disk's top-level shares mount as
// separate filesystems nested one level under the disk mount (GH #8). FAN_MARK_MOUNT
// does not cross into a nested mount, so each dataset needs its own explicit mark.
// Only one level deep: Unraid only auto-creates datasets for top-level shares, never
// nested subfolders. The mounts come from /proc/self/mountinfo, not from listing
// the disk: that listing runs every minute and could wake a sleeping disk.
static void mark_submounts(int fd, const std::string& base,
                            std::unordered_set<std::string>& known) {
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        // id parent major:minor root mountpoint ... - the mount point is field 5,
        // with spaces, tabs, newlines and backslashes octal-escaped (\040 etc.).
        std::istringstream ss(line);
        std::string skip, raw;
        if (!(ss >> skip >> skip >> skip >> skip >> raw)) continue;
        std::string child;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size() && isdigit((unsigned char)raw[i + 1])) {
                child += (char)std::stoi(raw.substr(i + 1, 3), nullptr, 8);
                i += 3;
            } else {
                child += raw[i];
            }
        }
        if (child.size() <= base.size() + 1 || child.compare(0, base.size(), base) != 0 ||
            child[base.size()] != '/' || child.find('/', base.size() + 1) != std::string::npos)
            continue;
        if (known.count(child)) continue;
        if (mark_path(fd, child)) {
            log_info("Watching nested mount at: " + child);
            known.insert(child);
        }
    }
}

// Block up to `secs` seconds, returning early when a signal is delivered.
// poll() is never auto-restarted by SA_RESTART on Linux (the reason the main
// loop's EINTR handling works), so SIGTERM/SIGINT make it return at once; the
// caller then re-checks _quit. One syscall, instant wakeup.
static void interruptible_sleep(int secs) {
    poll(nullptr, 0, secs * 1000);
}

static int init_fanotify() {
    auto fd = fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK,
                           O_RDONLY | O_LARGEFILE);
    if (fd < 0) {
        log_err("fanotify_init: " + std::string(strerror(errno)));
        return -1;
    }

    // A fresh fd carries no marks, so any previously-tracked submounts are gone too.
    _marked_submounts.clear();

    // First pass: mark every path that is already mounted; collect the rest.
    std::vector<std::string> pending;
    for (const auto& path : _cfg.scan_paths) {
        if (is_mounted(path) && mark_path(fd, path)) {
            log_info("Watching mount at: " + path);
            mark_submounts(fd, path, _marked_submounts);
        } else {
            pending.push_back(path);
        }
    }

    auto join_csv = [](const std::vector<std::string>& v) {
        std::string s; for (const auto& x : v) { if (!s.empty()) s += ", "; s += x; } return s;
    };

    if (pending.empty())
        return fd;

    // Slow boots (arrays/pools can take 20-30 min) mean a scan path may not be
    // mounted yet. Wait for it rather than silently watching nothing...
    // Whatcha doing? - Nothing... - You watching me? - I swear I dont...
    if (_cfg.mount_wait_timeout_mins > 0) {
        auto deadline = time(nullptr) + (time_t)_cfg.mount_wait_timeout_mins * 60;
        log_warn("Waiting for unmounted scan paths: " + join_csv(pending));
        notify_unraid("warning", "Unspin waiting for storage",
                      "Waiting up to " + std::to_string(_cfg.mount_wait_timeout_mins) +
                      " min for: " + join_csv(pending) +
                      ". Tiering is paused until these mount.");

        while (!pending.empty() && !_quit && time(nullptr) < deadline) {
            interruptible_sleep(_cfg.mount_retry_interval_secs);
            for (auto it = pending.begin(); it != pending.end(); ) {
                if (is_mounted(*it) && mark_path(fd, *it)) {
                    log_info("Watching mount at: " + *it + " (after wait)");
                    mark_submounts(fd, *it, _marked_submounts);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (pending.empty() && !_quit) {
            notify_unraid("normal", "Unspin storage ready",
                          "All scan paths mounted; tiering is active.");
        } else if (!pending.empty()) {
            notify_unraid("alert", "Unspin mount wait timed out",
                          "These scan paths did not mount within " +
                          std::to_string(_cfg.mount_wait_timeout_mins) + " min: " +
                          join_csv(pending) + ". Restart Unspin once the array is up.");
            for (const auto& path : pending)
                log_err("Timed out waiting for scan path: " + path + " - not watched");
        }
    } else {
        // Mount wait disabled: legacy warn-and-skip behaviour.
        for (const auto& path : pending)
            log_warn("fanotify_mark(" + path + "): not mounted - skipping (mount wait disabled)");
    }

    return fd;
}

static void process_events(int fan_fd) {
    static const pid_t _self_pid = getpid();
    alignas(fanotify_event_metadata) char buf[FAN_BUF_BYTES];
    auto n = read(fan_fd, buf, sizeof(buf));
    if (n <= 0) return;

    auto meta = reinterpret_cast<const fanotify_event_metadata*>(buf);

    while (FAN_EVENT_OK(meta, n)) {
        if ((meta->mask & FAN_WATCH_MASK) && meta->fd >= 0) {
            char proc_path[64];
            char file_path[PATH_MAX];
            snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", meta->fd);
            auto len = readlink(proc_path, file_path, sizeof(file_path) - 1);
            if (len > 0) {
                file_path[len] = '\0';
                // The kernel merges queued events of the same file and process
                // into one, so a quick open-read-close can arrive as a single
                // event with several bits set. Handle each bit, in the order
                // they happen; handling only one lost the close, the open count
                // never went back to 0 and a deferred promotion never ran.
                static constexpr std::pair<uint64_t, EvType> ORDER[] = {
                    {FAN_OPEN,          EvType::Open},
                    {FAN_ACCESS,        EvType::Read},
                    {FAN_MODIFY,        EvType::Modify},
                    {FAN_CLOSE_WRITE,   EvType::CloseWrite},
                    {FAN_CLOSE_NOWRITE, EvType::CloseNoWrite},
                };
                note_access(file_path, meta->pid, meta->mask);
                // Unspin's own reads (a promotion's copy) are no access by anyone.
                if (meta->pid != _self_pid)
                    for (const auto& [bit, ev] : ORDER)
                        if (meta->mask & bit) handle_event(file_path, ev);
            }
        }
        if (meta->fd >= 0) 
            close(meta->fd);
        meta = FAN_EVENT_NEXT(meta, n);
    }

    maybe_cleanup();
}

// ---------------------------------------------------------------------------
// Pool access times (ENABLE_POOL_ATIME, GH #9)
// ---------------------------------------------------------------------------
// Unraid mounts every pool noatime, so Mover Tuning's ATIME filter sees frozen
// access times. With the setting on, the pools Unspin promotes into get access
// times back: btrfs/xfs via a relatime remount (lost at every array start, so
// the periodic check re-applies it), ZFS via the atime/relatime properties
// (persist in the pool). Only Unspin's own changes are recorded, so switching
// the setting off never touches a pool the user set up themselves.

// btrfs/xfs pools remounted relatime by Unspin, one pool per line. tmpfs is
// fine: the remount itself doesn't survive a reboot either.
static const std::string ATIME_APPLIED_FILE = "/var/run/unspind.atime-applied";
// ZFS properties changed by Unspin, tab-separated, on the flash because the
// change persists across reboots:
//   set     <pool> <dataset> <property> <original value> <original source>
//   pending <pool> <mount id>      (revert at the next array start)
static const std::string ATIME_ZFS_STATE    = "/boot/config/plugins/unspin/atime-zfs.state";
// Created by the settings page when the setting is switched off with "now".
static const std::string ATIME_REVERT_NOW   = "/var/run/unspind.atime-revert-now";

static std::unordered_set<std::string> _atime_warned;          // pools warned about (reset on SIGHUP)
static std::unordered_set<std::string> _atime_seen_unmounted;  // ZFS pools seen unmounted while a revert is pending

static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) lines.push_back(line);
    return lines;
}

static void write_lines(const std::string& path, const std::vector<std::string>& lines) {
    if (lines.empty()) { unlink(path.c_str()); return; }
    std::ofstream f(path, std::ios::trunc);
    for (const auto& l : lines) f << l << "\n";
}

static std::vector<std::string> split_tabs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string field;
    while (std::getline(ss, field, '\t')) out.push_back(field);
    return out;
}

static bool run_quiet(const std::string& cmd) {
    return system((cmd + " > /dev/null 2>&1").c_str()) == 0;
}

static void atime_warn(const std::string& pool, const std::string& msg) {
    if (_atime_warned.insert(pool).second) log_warn(msg);
}

// Mount id (first field of /proc/self/mountinfo) of the mount at mnt. Changes
// when the pool is remounted, i.e. at every array start.
static std::string mount_id(const std::string& mnt) {
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string id, parent, devno, root, point;
        if (ss >> id >> parent >> devno >> root >> point && point == mnt) return id;
    }
    return "";
}

static void enable_atime_remount(const std::string& pool, const std::string& mnt) {
    struct statvfs sv;
    if (statvfs(mnt.c_str(), &sv) != 0 || !(sv.f_flag & ST_NOATIME)) return;  // relatime/strictatime already
    // "atime" is required: libmount keeps the existing noatime on a remount, so
    // "remount,relatime" alone succeeds but changes nothing.
    if (!run_quiet("mount -o remount,atime,relatime " + sh_squote(mnt)) ||
        statvfs(mnt.c_str(), &sv) != 0 || (sv.f_flag & ST_NOATIME)) {
        atime_warn(pool, "Could not remount " + mnt + " with relatime - access times stay off");
        return;
    }
    auto applied = read_lines(ATIME_APPLIED_FILE);
    if (std::find(applied.begin(), applied.end(), pool) == applied.end()) {
        applied.push_back(pool);
        write_lines(ATIME_APPLIED_FILE, applied);
    }
    log_info("Enabled access times on pool " + mnt + " (remounted relatime)");
}

struct ZfsProp { std::string ds, prop, value, source; };

static bool zfs_atime_props(const std::string& root, std::vector<ZfsProp>& out) {
    auto cmd = "zfs get -H -r -t filesystem -o name,property,value,source atime,relatime " +
               sh_squote(root) + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) {
        auto f = split_tabs(trim(buf));
        if (f.size() == 4) out.push_back({f[0], f[1], f[2], f[3]});
    }
    return pclose(p) == 0;
}

static void enable_atime_zfs(const std::string& pool, const std::string& root) {
    auto state = read_lines(ATIME_ZFS_STATE);
    // One property per pass, then re-read: setting it on a parent switches
    // children that inherit it too, so they don't get their own local value.
    for (int pass = 0; pass < 256; ++pass) {
        std::vector<ZfsProp> props;
        if (!zfs_atime_props(root, props)) {
            atime_warn(pool, "Could not read atime properties of ZFS dataset " + root);
            return;
        }
        auto it = std::find_if(props.begin(), props.end(),
                               [](const ZfsProp& z) { return z.value != "on"; });
        if (it == props.end()) return;

        // Record before changing, so a crash in between can't lose the original.
        auto key = "set\t" + pool + "\t" + it->ds + "\t" + it->prop + "\t";
        bool recorded = std::any_of(state.begin(), state.end(),
                                    [&](const std::string& l) { return l.rfind(key, 0) == 0; });
        if (!recorded) {
            state.push_back(key + it->value + "\t" + it->source);
            write_lines(ATIME_ZFS_STATE, state);
        }
        if (!run_quiet("zfs set " + it->prop + "=on " + sh_squote(it->ds))) {
            if (!recorded) { state.pop_back(); write_lines(ATIME_ZFS_STATE, state); }
            atime_warn(pool, "Could not set " + it->prop + "=on on ZFS dataset " + it->ds +
                             " - access times stay off");
            return;
        }
        log_info("Enabled access times on ZFS dataset " + it->ds + " (" + it->prop +
                 "=on, was " + it->value + ")");
    }
}

static void revert_atime_remounts() {
    for (const auto& pool : read_lines(ATIME_APPLIED_FILE)) {
        auto mnt = MNT_PREFIX + pool;
        if (!is_mounted(mnt)) continue;
        if (run_quiet("mount -o remount,noatime " + sh_squote(mnt)))
            log_info("Turned access times off on pool " + mnt + " (remounted noatime)");
        else
            log_warn("Could not remount " + mnt + " with noatime");
    }
    unlink(ATIME_APPLIED_FILE.c_str());
}

// Restore the recorded ZFS properties of one pool (or all, if pool is empty),
// newest change first.
static void revert_atime_zfs(const std::string& pool) {
    auto state = read_lines(ATIME_ZFS_STATE);
    std::vector<std::string> keep;
    for (auto it = state.rbegin(); it != state.rend(); ++it) {
        auto f = split_tabs(*it);
        if (f.size() < 3 || (!pool.empty() && f[1] != pool)) { keep.insert(keep.begin(), *it); continue; }
        if (f[0] != "set" || f.size() != 6) continue;   // drops "pending" too
        const auto& ds = f[2]; const auto& prop = f[3]; const auto& val = f[4]; const auto& src = f[5];
        auto cmd = src == "local" ? "zfs set " + prop + "=" + sh_squote(val) + " " + sh_squote(ds)
                                  : "zfs inherit " + prop + " " + sh_squote(ds);
        if (run_quiet(cmd))
            log_info("Turned access times off on ZFS dataset " + ds + " (" + prop + " back to " +
                     (src == "local" ? val : "inherited") + ")");
        else
            log_warn("Could not restore " + prop + " on ZFS dataset " + ds + ": " + cmd);
    }
    write_lines(ATIME_ZFS_STATE, keep);
}

// Called at startup, on SIGHUP and every PERIODIC_CHECK_INTERVAL_SEC.
static void check_pool_atime() {
    if (_cfg.enable_pool_atime) {
        unlink(ATIME_REVERT_NOW.c_str());
        // Switched back on before a pending revert happened: keep the changes.
        auto state = read_lines(ATIME_ZFS_STATE);
        auto n = state.size();
        state.erase(std::remove_if(state.begin(), state.end(),
                                   [](const std::string& l) { return l.rfind("pending\t", 0) == 0; }),
                    state.end());
        if (state.size() != n) write_lines(ATIME_ZFS_STATE, state);

        for (const auto& [pool, info] : _pools) {
            auto mnt = MNT_PREFIX + pool;
            MountEntry me;
            if (!is_mounted(mnt) || !mount_entry(mnt, me) || me.mnt != mnt) continue;
            if (me.fstype == "zfs") enable_atime_zfs(pool, me.src);
            else                    enable_atime_remount(pool, mnt);
        }
        return;
    }

    if (access(ATIME_REVERT_NOW.c_str(), F_OK) == 0) {
        revert_atime_remounts();
        revert_atime_zfs("");
        unlink(ATIME_REVERT_NOW.c_str());
        return;
    }

    // Off, at the next array start: Unraid remounts btrfs/xfs pools noatime itself.
    unlink(ATIME_APPLIED_FILE.c_str());

    // ZFS keeps its properties, so revert once the pool has been remounted.
    auto state = read_lines(ATIME_ZFS_STATE);
    if (state.empty()) return;
    std::unordered_map<std::string, std::string> pending;   // pool -> mount id
    std::vector<std::string> pools;
    for (const auto& l : state) {
        auto f = split_tabs(l);
        if (f.size() < 3) continue;
        if (f[0] == "pending") pending[f[1]] = f[2];
        else if (std::find(pools.begin(), pools.end(), f[1]) == pools.end()) pools.push_back(f[1]);
    }
    for (const auto& pool : pools) {
        auto mnt = MNT_PREFIX + pool;
        if (!is_mounted(mnt)) { _atime_seen_unmounted.insert(pool); continue; }
        auto id = mount_id(mnt);
        auto pit = pending.find(pool);
        if (pit == pending.end()) {
            state.push_back("pending\t" + pool + "\t" + id);
            write_lines(ATIME_ZFS_STATE, state);
            log_info("Access times on ZFS pool " + mnt + " will be turned off at the next array start");
        } else if (pit->second != id || _atime_seen_unmounted.count(pool)) {
            revert_atime_zfs(pool);
            _atime_seen_unmounted.erase(pool);
            state = read_lines(ATIME_ZFS_STATE);
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static const std::string LOCK_FILE = "/var/run/unspind.lock";

// Only one unspind may run: two would promote the same files at the same time.
// The kernel releases the lock however its holder ends, SIGKILL included.
static bool take_instance_lock() {
    int fd = open(LOCK_FILE.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        log_warn("Cannot open " + LOCK_FILE + ": " + strerror(errno) + " - not checking for a second unspind");
        return true;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK) {
            log_warn("Cannot lock " + LOCK_FILE + ": " + strerror(errno) + " - not checking for a second unspind");
            close(fd);
            return true;
        }
        auto other = read_first_line(LOCK_FILE);
        auto msg   = "unspind is already running" + (other.empty() ? "" : " (PID " + other + ")") +
                     " - this second instance exits";
        log_warn(msg);
        std::cerr << msg << "\n";
        close(fd);
        return false;
    }
    // The descriptor stays open: the lock lasts as long as this process.
    auto pid = std::to_string(getpid()) + "\n";
    if (ftruncate(fd, 0) != 0 || write(fd, pid.data(), pid.size()) != (ssize_t)pid.size())
        log_debug("Cannot write the PID to " + LOCK_FILE);
    return true;
}

int main(int argc, char* argv[]) {
    // Honor explicit modes on mkdir/open without inherited-umask masking;
    // directory creation relies on this to produce 0777 rather than 0755.
    umask(0);

    auto override_dry_run = false;
    auto override_debug   = false;

    for (auto i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            _cfg_file = argv[++i];
        else if (strcmp(argv[i], "--dry-run") == 0)
            override_dry_run = true;
        else if (strcmp(argv[i], "--debug") == 0)
            override_debug = true;
        else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: unspind [--config FILE] [--dry-run] [--debug]\n"
                      << "  Signals: SIGTERM/SIGINT to stop, SIGHUP to reload config,\n"
                      << "           SIGUSR1 to pause, SIGUSR2 to unpause,\n"
                      << "           SIGRTMIN to toggle pause\n";
            return 0;
        } else {
            std::cerr << "unspind: unknown option " << argv[i] << " (see --help)\n";
            return 1;
        }
    }

    load_config();
    if (override_dry_run) _cfg.dry_run = true;
    if (override_debug)   _cfg.debug   = true;

    open_log();
    if (!_log_stream.is_open())
        std::cerr << "Warning: cannot open log file " << _cfg.log_file << "\n";
    if (!take_instance_lock()) return 1;

    signal(SIGTERM,  sig_handler);
    signal(SIGINT,   sig_handler);
    signal(SIGHUP,   sig_handler);
    signal(SIGUSR1,  sig_handler);
    signal(SIGUSR2,  sig_handler);
    signal(SIGRTMIN, sig_handler);

    log_info("=== unspind starting ===");
    crash_prepare();
    install_crash_handlers();
    log_config();
    recover_handover();
    init_nobody();
    load_stats();
    load_wakeups();

    int fan_fd = init_fanotify();
    if (fan_fd < 0) {
        log_err("fanotify init failed - unspind running as root?");
        return 1;
    }
    check_pool_atime();

    _last_cleanup = time(nullptr);

    struct pollfd pfd = { fan_fd, POLLIN, 0 };
    auto was_paused = false;
    log_info("Monitoring for file accesses...");

    while (!_quit) {
        auto paused_now = is_paused();
        if (paused_now != was_paused) {
            was_paused = paused_now;
            log_info(paused_now ? "--- Paused ---" : "--- Resumed ---");
        }
        write_pause_state();

        // Runs on its own schedule (not just when something gets logged), so
        // the log still gets trimmed while paused and otherwise quiet.
        check_log_size();
        update_disk_state();

        if (_reload) {
            _reload = 0;
            log_info("SIGHUP - reloading config");
            close(fan_fd);
            _log_stream.close();
            load_config();
            if (override_dry_run) _cfg.dry_run = true;
            if (override_debug)   _cfg.debug   = true;
            open_log();
            crash_prepare();
            log_config();
            fan_fd = init_fanotify();
            if (fan_fd < 0) {
                log_err("fanotify re-init failed. Exiting.");
                return 1;
            }
            pfd.fd = fan_fd;
            _atime_warned.clear();
            check_pool_atime();
        }

        auto ret = poll(&pfd, 1, wake_watch_active() ? WAKE_POLL_MS : POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_err("poll: " + std::string(strerror(errno)));
            break;
        }
        if (ret == 0) {
            // Poll timeout - refresh transfer state so "finished" is logged promptly
            transfers_active();
        } else if (pfd.revents & POLLIN) {
            process_events(fan_fd);
        }

        // Checked on every pass, not only on poll timeouts: a busy server may
        // never go POLL_TIMEOUT_MS without a file event.
        time_t now = time(nullptr);
        if (now - _last_periodic_check >= PERIODIC_CHECK_INTERVAL_SEC) {
            _last_periodic_check = now;
            promote_stale_pending();
            // Pick up dataset mounts created on the fly (e.g. a ZFS share used for
            // the first time) without waiting for a restart or SIGHUP (GH #8).
            for (const auto& path : _cfg.scan_paths)
                if (is_mounted(path)) mark_submounts(fan_fd, path, _marked_submounts);
            // Every array start remounts the pools noatime (and is how a
            // pending ZFS revert gets noticed).
            check_pool_atime();
            flush_stats();
            flush_wakeups();
            crash_prepare();   // keeps the crash report's time right across DST changes
        }
    }

    flush_stats(true);
    flush_wakeups(true);
    unlink(RUNNING_MARKER.c_str());
    unlink(PAUSE_STATE.c_str());
    close(fan_fd);
    log_info("unspind stopped.");
    return 0;
}
