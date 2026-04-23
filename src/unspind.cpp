// unspind.cpp - Unspin tiering daemon for Unraid
// Monitors file accesses via fanotify and promotes hot files to the cache tier.
//
// Compile: g++ -O2 -Wall -o unspind unspind.cpp
// Requires: Linux kernel 2.6.37+ (fanotify), CAP_SYS_ADMIN (run as root)

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/fanotify.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t     COPY_CHUNK_BYTES     = 1u << 20;           // 1 MiB per sendfile call
static constexpr int        CLEANUP_INTERVAL_SEC = 3600;               // 1 h between access-map cleanups
static constexpr int        POLL_TIMEOUT_MS      = 5000;               // fanotify poll wait
static constexpr size_t     FAN_BUF_BYTES        = 65536;              // fanotify read buffer
static constexpr int        MOVER_CACHE_SEC      = 5;                  // how long to cache mover-active result
static constexpr uint64_t   FAN_WATCH_MASK       = FAN_ACCESS | FAN_OPEN | FAN_CLOSE_NOWRITE
                                                 | FAN_CLOSE_WRITE | FAN_MODIFY;
static constexpr int64_t    KB                   = 1024;
static constexpr int64_t    MB                   = 1024 * KB;
static constexpr int64_t    GB                   = 1024 * MB;
static constexpr int64_t    TB                   = 1024 * GB;
static constexpr int64_t    SMALL_F_THRESHOLD    = 10 * MB;
static constexpr const char RULE1[]              = "rule 1 - small";
static constexpr const char RULE2[]              = "rule 2 - big, reads";
static constexpr const char RULE3[]              = "rule 3 - big, opens";
static constexpr const char DEFAULT_CFG_FILE[]   = "/usr/local/emhttp/plugins/unspin/unspin.cfg.default";
static constexpr const char TRIM_CHARS[]         = " \t\r\n";

// ---------------------------------------------------------------------------
// Config & globals
// ---------------------------------------------------------------------------

static std::string _cfg_file = "/boot/config/plugins/unspin/unspin.cfg";
static volatile sig_atomic_t _quit   = 0;
static volatile sig_atomic_t _reload = 0;
static volatile sig_atomic_t _paused = 0;

struct Config {
    std::string              hot_path                  = "/mnt/cache";
    std::vector<std::string> scan_paths                = {"/mnt/disk1","/mnt/disk2"};
    int64_t                  small_file_threshold      = SMALL_F_THRESHOLD;
    int                      small_min_accesses        = 1;
    int                      large_short_min_accesses  = 100;
    int                      large_short_window_mins   = 5;
    int                      large_long_min_accesses   = 3;
    int                      large_long_window_hours   = 24;
    double                   max_hot_fill_percent      = 80.0;
    bool                     dry_run                   = false;
    bool                     debug                     = false;
    bool                     rule1_enabled             = true;
    bool                     rule1_fallthrough         = true;  // when rule1 is off, evaluate small files via rules 2+3
    bool                     rule2_enabled             = true;
    bool                     rule3_enabled             = true;
    int                      rule3_min_reads           = 6;    // thumbnail filter: skip opens with 1..N-1 reads (0=disabled)
    bool                     pause_on_rsync            = true; // pause event counting while any rsync process is running
    std::string              log_file                  = "/var/log/unspin.log";
    int                      log_max_lines             = 3000;
    std::vector<std::string> exclude_patterns;
};

static Config _cfg;

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
};

static std::unordered_map<std::string, AccessRecord> _access_map;
static std::unordered_set<std::string>               _no_cache_shares;
static time_t      _last_cleanup     = 0;
static std::string _last_hot_full_msg;
static bool        _transfers_cached  = false;
static time_t      _transfers_checked = 0;
static uid_t       _nobody_uid       = 99;   // Unraid defaults; updated by init_nobody()
static gid_t       _nobody_gid       = 100;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static std::ofstream _log_stream;

static void vlog(const char* level, const std::string& msg) {
    auto now = time(nullptr);
    char ts[32];
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    std::ostream& out = _log_stream.is_open() ? _log_stream : std::cout;
    out << ts << " [" << level << "] " << msg << "\n";
    out.flush();
}

static void log_info (const std::string& m) { vlog("INFO ", m); }
static void log_warn (const std::string& m) { vlog("WARN ", m); }
static void log_err  (const std::string& m) { vlog("ERROR", m); }
static void log_debug(const std::string& m) { if (_cfg.debug) vlog("DEBUG", m); }

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
    }
    log_info("nobody uid=" + std::to_string(_nobody_uid) + " gid=" + std::to_string(_nobody_gid));
}

static void mkdir_p(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            auto sub = path.substr(0, i);
            if (sub.empty()) continue;
            if (mkdir(sub.c_str(), 0777) == 0) {
                chown(sub.c_str(), _nobody_uid, _nobody_gid);
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
    const std::string mnt = "/mnt/";
    if (path.rfind(mnt, 0) != 0) return "";
    auto pool_end = path.find('/', mnt.size());
    if (pool_end == std::string::npos) return "";
    auto share_end = path.find('/', pool_end + 1);
    return path.substr(pool_end + 1,
        share_end == std::string::npos ? std::string::npos : share_end - pool_end - 1);
}

// Load /boot/config/shares/*.cfg and collect shares with shareUseCache="no"
static void load_share_cache_settings() {
    _no_cache_shares.clear();
    const std::string dir = "/boot/config/shares";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        auto fname = std::string(ent->d_name);
        if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".cfg") 
            continue;
        
        auto share = fname.substr(0, fname.size() - 4);
        std::ifstream f(dir + "/" + fname);
        if (!f.is_open()) 
            continue;

        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.rfind("shareUseCache=", 0) != 0) 
                continue;
            auto val = line.substr(14);
            if (!val.empty() && val.front() == '"') val = val.substr(1);
            if (!val.empty() && val.back()  == '"') val.pop_back();
            if (val == "no") _no_cache_shares.insert(share);
            break;
        }
    }
    closedir(d);
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
// Config loader
// ---------------------------------------------------------------------------

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

static void load_config() {
    // Last-resort fallbacks - used only if unspin.cfg.default is missing.
    // These do not usually need to be kept in sync, only if you're bored
    std::unordered_map<std::string, std::string> raw = {
        {"HOT_PATH",                 "/mnt/cache"},
        {"SCAN_PATHS",               "/mnt/user"},
        {"MAX_HOT_FILL_PERCENT",     "80"},
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
        {"EXCLUDE_PATTERNS",         "/nevercachethis,/orthis"},
        {"LOG_FILE",                 "/var/log/unspin.log"},
        {"LOG_MAX_LINES",            "3000"},
    };

    parse_cfg_into(DEFAULT_CFG_FILE, raw); // overlay shipped defaults
    parse_cfg_into(_cfg_file, raw);        // overlay user config

    auto get = [&](const std::string& k) -> const std::string& {
        static const std::string empty;
        auto it = raw.find(k);
        return (it != raw.end()) ? it->second : empty;
    };

    _cfg.hot_path                 = get("HOT_PATH");

    {
        auto raw_scans = split_csv(get("SCAN_PATHS"));
        _cfg.scan_paths.clear();
        for (const auto& sp : raw_scans) {
            if (sp.rfind("/mnt/user", 0) == 0) {
                log_warn("Ignoring scan path '" + sp + "': /mnt/user is the share mount - "
                         "use array disk mount points (e.g. /mnt/disk1) instead");
            } else {
                _cfg.scan_paths.push_back(sp);
            }
        }
    }

    _cfg.max_hot_fill_percent     = std::stod(get("MAX_HOT_FILL_PERCENT"));
    _cfg.small_file_threshold     = parse_size(get("SMALL_FILE_THRESHOLD"));
    _cfg.small_min_accesses       = std::stoi(get("SMALL_MIN_ACCESSES"));
    _cfg.large_short_min_accesses = std::stoi(get("LARGE_SHORT_MIN_ACCESSES"));
    _cfg.large_short_window_mins  = std::stoi(get("LARGE_SHORT_WINDOW_MINS"));
    _cfg.large_long_min_accesses  = std::stoi(get("LARGE_LONG_MIN_ACCESSES"));
    _cfg.large_long_window_hours  = std::stoi(get("LARGE_LONG_WINDOW_HOURS"));
    _cfg.dry_run                  = (get("DRY_RUN")            == "yes");
    _cfg.debug                    = (get("LOG_LEVEL")          == "debug");
    _cfg.rule1_enabled            = (get("RULE1_ENABLED")      != "no");
    _cfg.rule1_fallthrough        = (get("RULE1_FALLTHROUGH")  == "yes");
    _cfg.rule2_enabled            = (get("RULE2_ENABLED")      != "no");
    _cfg.rule3_enabled            = (get("RULE3_ENABLED")      != "no");
    _cfg.rule3_min_reads          = std::max(0, std::stoi(get("RULE3_MIN_READS")));
    _cfg.pause_on_rsync           = (get("PAUSE_ON_RSYNC")     != "no");
    _cfg.log_file                 = get("LOG_FILE");
    _cfg.log_max_lines            = std::stoi(get("LOG_MAX_LINES"));
    _cfg.exclude_patterns         = split_csv(get("EXCLUDE_PATTERNS"));

    load_share_cache_settings();
}

static void log_config() {
    log_info("Config: " + _cfg_file);
    log_info("Hot path: " + _cfg.hot_path);
    for (const auto& sp : _cfg.scan_paths)
        log_info("Scan path: " + sp);
    log_info("Max hot fill: " + std::to_string((int)_cfg.max_hot_fill_percent) + "%");
    {
        std::string excl;
        for (const auto& p : _cfg.exclude_patterns) {
            if (!excl.empty()) excl += ", ";
            excl += p;
        }
        log_info("Exclude: " + (excl.empty() ? std::string("(none)") : excl));
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
    if (!_no_cache_shares.empty()) {
        std::string s;
        for (const auto& sh : _no_cache_shares) { if (!s.empty()) s += ", "; s += sh; }
        log_info("Shares with Use Cache=No (skipping): " + s);
    }
}

// ---------------------------------------------------------------------------
// Disk utilities
// ---------------------------------------------------------------------------

static double disk_fill_percent(const std::string& path) {
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) != 0  ||  sv.f_blocks == 0) 
        return 100.0;
    return 100.0 * (double)(sv.f_blocks - sv.f_bfree) / (double)sv.f_blocks;
}

// Live check - mover always counted, rsync only if PAUSE_ON_RSYNC is set.
static bool transfers_running() {
    if (system("pgrep -x mover > /dev/null 2>&1") == 0) 
        return true;
    if (_cfg.pause_on_rsync && system("pgrep -x rsync > /dev/null 2>&1") == 0) 
        return true;
    return false;
}

// Cached transfers check - refreshes at most every MOVER_CACHE_SEC seconds.
// Used as a fast gate to skip counting events while transfers are running.
// Log on state transitions so it's visible when counting resumes/pauses.
static bool transfers_active() {
    auto now = time(nullptr);
    if (now - _transfers_checked < MOVER_CACHE_SEC) 
        return _transfers_cached;
    _transfers_checked = now;

    auto prev = _transfers_cached;
    _transfers_cached = transfers_running();
    if (_transfers_cached && !prev)
        log_info("Mover or rsync started - pausing monitoring");
    else if (!_transfers_cached && prev)
        log_info("Mover or rsync finished - resuming monitoring");
    return _transfers_cached;
}

// ---------------------------------------------------------------------------
// File movement
// ---------------------------------------------------------------------------

static std::string resolve_destination(const std::string& src) {
    // Strip /mnt/<pool>/ prefix and rebase under hot_path.
    // e.g. /mnt/user/media/foo.mkv -> /mnt/cache/media/foo.mkv
    const std::string mnt = "/mnt/";
    if (src.rfind(mnt, 0) == 0) {
        auto next = src.find('/', mnt.size());
        if (next != std::string::npos)
            return _cfg.hot_path + "/" + src.substr(next + 1);
    }
    auto slash = src.rfind('/');
    return _cfg.hot_path + "/" +
           (slash != std::string::npos ? src.substr(slash + 1) : src);
}

static bool copy_file(const std::string& src, const std::string& dst) {
    auto src_fd = open(src.c_str(), O_RDONLY | O_NOATIME);
    if (src_fd < 0 && errno == EPERM)
        src_fd = open(src.c_str(), O_RDONLY);
    if (src_fd < 0) {
        log_err("open(" + src + "): " + strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        log_err("fstat(" + src + "): " + strerror(errno));
        close(src_fd);
        return false;
    }

    auto dir = dst.substr(0, dst.rfind('/'));
    if (!dir.empty()) mkdir_p(dir);

    if (access(dst.c_str(), F_OK) == 0) {
        log_warn("copy_file: destination already exists, skipping: " + dst);
        close(src_fd);
        return false;
    }

    mode_t dst_mode = (st.st_mode & 0222) ? 0666 : 0444;
    int dst_fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, dst_mode);
    if (dst_fd < 0) {
        log_err("open(" + dst + "): " + strerror(errno));
        close(src_fd);
        return false;
    }

    auto ok = true;
    off_t offset    = 0;
    auto  remaining = st.st_size;

    while (remaining > 0) {
        auto chunk = (size_t)std::min(remaining, (off_t)COPY_CHUNK_BYTES);
        auto n = sendfile(dst_fd, src_fd, &offset, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_err("sendfile(" + src + "): " + strerror(errno));
            ok = false;
            break;
        }
        if (n == 0) {
            log_err("sendfile(" + src + "): unexpected EOF");
            ok = false;
            break;
        }
        remaining -= n;
    }

    close(src_fd);
    close(dst_fd);
    if (!ok) { 
        unlink(dst.c_str()); 
        return false; 
    }

    // Set Unraid-standard permissions and ownership
    chmod(dst.c_str(), dst_mode);
    chown(dst.c_str(), _nobody_uid, _nobody_gid);

    // Preserve original atime and mtime
    struct timespec times[2] = { st.st_atim, st.st_mtim };
    utimensat(AT_FDCWD, dst.c_str(), times, 0);

    return true;
}

static bool promote_file(const std::string& src, int64_t size,
                         const PromoteDecision& dec) {
    auto dst = resolve_destination(src);

    if (_cfg.dry_run) {
        log_info("[" + dec.rule + "] [DRY RUN] Would promote " +
                 src + " -> " + dst + " (" + human_size(size) + ") - " + dec.reason);
        return true;
    }

    log_info("[" + dec.rule + "] Promoting " + src + " -> " + dst +
             " (" + human_size(size) + ") - " + dec.reason);

    if (!copy_file(src, dst)) return false;

    if (unlink(src.c_str()) != 0) {
        log_err("unlink(" + src + "): " + strerror(errno) +
                " - copy kept at " + dst);
        return false;
    }

    return true;
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

    // Rules 2 & 3 - sliding windows (also reached for small files via fallthrough)
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

    // already hot?
    if (path.rfind(_cfg.hot_path, 0) == 0) return;

    // Excluded patterns
    for (const auto& ex : _cfg.exclude_patterns)
        if (!ex.empty() && path.find(ex) != std::string::npos) return;

    // Skip shares with Use Cache = No
    auto sname = path_share_name(path);
    if (!sname.empty() && _no_cache_shares.count(sname)) {
        log_debug("[no-cache] skipping " + path);
        return;
    }

    auto skip_counting = _paused || transfers_active();

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
                log_debug("[already hot] " + path);
            return;
        }
        // Hot copy is gone
        log_debug("[promoted -> reset] hot copy gone, re-evaluating: " + path);
        rec.promoted        = false;
        rec.pending_promote = false;
    }

    auto now = time(nullptr);
    rec.last_event = now;

    // Attempt an immediate promotion (transfer / fill checks included).
    auto do_promote = [&](const PromoteDecision& dec) {
        if (rec.promoted) return;
        if (transfers_running()) {
            log_info("[" + dec.rule + "] Mover or rsync active, deferring: " + path);
            return;
        }
        auto fill = disk_fill_percent(_cfg.hot_path);
        if (fill >= _cfg.max_hot_fill_percent) {
            auto msg = "[" + dec.rule + "] Hot tier " + std::to_string((int)fill) +
                              "% full, skipping: " + path;
            if (msg != _last_hot_full_msg) { 
                log_info(msg);  
                _last_hot_full_msg = msg; 
            } else { 
                log_debug(msg); 
            }
            return;
        }
        if (promote_file(path, size, dec))
            rec.promoted = true;
    };

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
        if (rec.open_count > 0) {
            auto first = !rec.pending_promote;
            rec.pending_promote = true;
            rec.pending_dec = dec;
            if (first)
                log_debug("[" + dec.rule + "] Deferred (file open): " +
                          path + " (" + human_size(size) + ") - " + dec.reason);
            return;
        }
        do_promote(dec);
    };

    if (ev == EvType::Read) {
        if (skip_counting) return;
        rec.total_reads++;
        rec.read_timestamps.push_back(now);
        if (rec.open_reads >= 0) rec.open_reads++;
        auto dec = should_promote(rec, size);
        log_debug("Read #" + std::to_string(rec.total_reads) + " " + path +
                  " (" + human_size(size) + ") - " + dec.reason);
        evaluate();

    } else if (ev == EvType::Open) {
        rec.open_count++;
        if (skip_counting) return;
        rec.open_reads = 0;
        if (_cfg.rule3_min_reads == 0) {
            // Filter disabled - count immediately
            rec.open_timestamps.push_back(now);
            auto dec = should_promote(rec, size);
            log_debug("Open " + path + " (" + human_size(size) + ") - " + dec.reason);
            evaluate();
        } else {
            log_debug("Open (pending, min reads: " + std::to_string(_cfg.rule3_min_reads) +
                      ") " + path + " (" + human_size(size) + ")");
        }

    } else { // CloseNoWrite or CloseWrite
        // Decrement open count; clamp at 0 (file may have been opened before daemon start) (ask me how I know...).
        if (rec.open_count > 0) rec.open_count--;
        // Clear if done.
        if (rec.open_count == 0) rec.has_write_open = false;

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
            }

            // Execute any promotion deferred while the file was open.
            if (!rec.promoted && rec.pending_promote && rec.open_count == 0) {
                PromoteDecision dec = rec.pending_dec;
                rec.pending_promote = false;
                do_promote(dec);
            }
        } else {
            // CloseWrite: file was modified - only update open tracking, no promotion.
            rec.open_reads = -1;
        }
    }
}

static void trim_log() {
    const auto& path = _cfg.log_file;
    const auto max = _cfg.log_max_lines;

    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        if (!in.is_open()) return;
        std::string line;
        while (std::getline(in, line)) lines.push_back(std::move(line));
    }
    if ((int)lines.size() <= max) return;

    _log_stream.close();
    {
        std::ofstream out(path, std::ios::trunc);
        auto start = (int)lines.size() - max;
        for (auto i = start; i < (int)lines.size(); ++i)
            out << lines[i] << '\n';
    }
    _log_stream.open(path, std::ios::app);
    log_info("Log trimmed to " + std::to_string(max) + " lines.");
}

static void maybe_cleanup() {
    auto now = time(nullptr);
    if (now - _last_cleanup < CLEANUP_INTERVAL_SEC) return;
    _last_cleanup = now;

    auto cutoff = now - (time_t)_cfg.large_long_window_hours * 3600;
    auto before = _access_map.size();

    for (auto it = _access_map.begin(); it != _access_map.end(); ) {
        // Remove promoted entries and entries that haven't been accessed within the window
        if (it->second.promoted || it->second.last_event < cutoff)
            it = _access_map.erase(it);
        else
            ++it;
    }

    log_info("Access map cleanup: " + std::to_string(before) + " -> " +
             std::to_string(_access_map.size()) + " entries");
    trim_log();
}

// ---------------------------------------------------------------------------
// fanotify
// ---------------------------------------------------------------------------

static int init_fanotify() {
    auto fd = fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK,
                           O_RDONLY | O_LARGEFILE);
    if (fd < 0) {
        log_err("fanotify_init: " + std::string(strerror(errno)));
        return -1;
    }

    for (const auto& path : _cfg.scan_paths) {
        // FAN_MARK_MOUNT watches the entire mount that contains this path.
        // Events are then filtered by path prefix in handle_event().
        if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
                          FAN_WATCH_MASK,
                          AT_FDCWD, path.c_str()) < 0) {
            log_warn("fanotify_mark(" + path + "): " + strerror(errno) +
                     " - mount may not exist yet, skipping");
        } else {
            log_info("Watching mount at: " + path);
        }
    }

    return fd;
}

static void process_events(int fan_fd) {
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
                EvType ev = (meta->mask & FAN_ACCESS)         ? EvType::Read
                          : (meta->mask & FAN_CLOSE_NOWRITE)  ? EvType::CloseNoWrite
                          : (meta->mask & FAN_CLOSE_WRITE)    ? EvType::CloseWrite
                          : (meta->mask & FAN_MODIFY)         ? EvType::Modify
                          :                                     EvType::Open;
                handle_event(file_path, ev);
            }
        }
        if (meta->fd >= 0) 
            close(meta->fd);
        meta = FAN_EVENT_NEXT(meta, n);
    }

    maybe_cleanup();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

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
        }
    }

    load_config();
    if (override_dry_run) _cfg.dry_run = true;
    if (override_debug)   _cfg.debug   = true;

    _log_stream.open(_cfg.log_file, std::ios::app);
    if (!_log_stream.is_open())
        std::cerr << "Warning: cannot open log file " << _cfg.log_file << "\n";

    signal(SIGTERM,  sig_handler);
    signal(SIGINT,   sig_handler);
    signal(SIGHUP,   sig_handler);
    signal(SIGUSR1,  sig_handler);
    signal(SIGUSR2,  sig_handler);
    signal(SIGRTMIN, sig_handler);

    log_info("=== unspind starting ===");
    log_config();

    int fan_fd = init_fanotify();
    if (fan_fd < 0) {
        log_err("fanotify init failed - unspind running as root?");
        return 1;
    }

    _last_cleanup = time(nullptr);

    struct pollfd pfd = { fan_fd, POLLIN, 0 };
    auto was_paused = false;
    log_info("Monitoring for file accesses...");

    while (!_quit) {
        if (_paused != was_paused) {
            was_paused = _paused;
            log_info(_paused ? "--- Paused ---" : "--- Resumed ---");
        }

        if (_reload) {
            _reload = 0;
            log_info("SIGHUP - reloading config");
            close(fan_fd);
            _log_stream.close();
            load_config();
            if (override_dry_run) _cfg.dry_run = true;
            if (override_debug)   _cfg.debug   = true;
            _log_stream.open(_cfg.log_file, std::ios::app);
            log_config();
            fan_fd = init_fanotify();
            if (fan_fd < 0) {
                log_err("fanotify re-init failed. Exiting.");
                return 1;
            }
            pfd.fd = fan_fd;
        }

        auto ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_err("poll: " + std::string(strerror(errno)));
            break;
        }
        if (ret == 0) {
            // Poll timeout - refresh transfer state so "finished" is logged promptly
            transfers_active();
            continue;
        }
        if (pfd.revents & POLLIN)
            process_events(fan_fd);
    }

    close(fan_fd);
    log_info("unspind stopped.");
    return 0;
}
