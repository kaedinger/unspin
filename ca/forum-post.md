# Unspin - Rudimentary Hot-File Tiering for Unraid

Unspin automatically promotes your most-accessed files to faster storage (cache/NVMe) using a lightweight C++ daemon that watches file access in real time. The main goal is to have fewer disk spin-ups (hence the name), but we'll take the faster access, too ;-)

It also tells you **which program, container, VM or SMB client woke each of your sleeping disks**, so the eternal "what spun up my disk?" question finally gets an answer.

There's no explicit cold demotion - Unraid's mover (or Mover Tuning, see below) takes care of that.

**Make sure this does what you want! There's a dry-run mode; use it!**

## How It Works

Unspin runs `unspind`, a daemon that uses Linux's **fanotify** interface to receive an event every time a file is opened or read on your array disks. It applies three configurable promotion rules:

- **Small file rule** - files below a size threshold get promoted after N total reads (e.g. promote a config file on first access)
- **Large file - short window** - N reads within M minutes triggers promotion (e.g. 100 reads in 5 min = streaming video)
- **Large file - long window** - N opens within H hours triggers promotion (e.g. 3 opens in 24 h = frequently consulted PDF)

**Why these rules?** Under ideal circumstances we would cache everything - but we don't have the space. We can't have a huge Linux ISO using up precious cache space if it's just held open for a few bytes of torrent upload. But of course that changes if it's being constantly read, or accessed on a regular basis. You'll have to play around with the rule settings to best match your own use cases.

### Safe by design

- **Promotion waits until the file is closed everywhere.** A movie you're watching keeps playing from the array and is promoted after you stop; the next open picks up the cache copy. (No live switching during playback.)
- **A file is never lost, doubled, left half-copied or missing from the share.** The copy stays invisible until it's complete and is only swapped in if nobody moved, changed, deleted or opened the file in the meantime. Whatever you do with the file during a promotion wins, and Unspin simply tries again later. Even a crash mid-promotion is cleaned up on the next start.
- **Unspin never wakes a sleeping disk.** A file can be read from RAM while its disk sleeps; it isn't promoted then (removing the array copy would wake the disk and parity), but on a later access with the disk awake.

## Features

- **Event-driven** - reacts to each file read in real time via fanotify, no polling or cron
- **Three-rule promotion** - separate thresholds for small files and large files (streaming burst / periodic access), each rule can be switched on or off
- **Per-share cache pool routing** - destinations follow each share's own cache pool setting in Unraid, so different shares can promote to different pools. Shares with "Use Cache: No" or "Only" are skipped automatically
- **Detected Shares** - the settings page lists every share with its routing and a Watch toggle to opt individual shares out
- **Per-pool fill guard** - each pool gets its own fill % limit; promotion to that pool stops once it's exceeded (ZFS pools with child datasets included)
- **Disk Wake-ups** - which program, container, VM or SMB client (user and IP) woke which disk, with counts per day and the latest wake-ups. Access through user shares is traced back to the program that asked for the file. Directory listings, name lookups and metadata writes are named as such
- **Promotion statistics** - files and size moved to each pool today, per day on average and in total
- **Mover / rsync aware** - pauses while Unraid's mover (including Mover Tuning) or, optionally, any rsync runs
- **Named pause locks for backups** - stackable `rc.unspin pause NAME`, optionally timed (`--ttl 15m`) so a crashed backup job can't leave Unspin paused forever
- **Pool Access Times** - turns on access times for the pools Unspin promotes into, so Mover Tuning's "Use ATIME" filter can demote files that haven't been read for a while
- **ZFS array disks** - dataset-per-share mounts are detected and watched automatically
- **Waits for slow mounts at boot**, with an Unraid notification if it has to
- **Dry-run mode** - logs every decision without moving anything
- **Exclude patterns** - skip any path substring
- **Live log viewer** and, on debug logging, the recently accessed files per disk

## Using Unspin with Mover Tuning

Unspin promotes files because they're *read*, so the natural way to demote them is Mover Tuning's age filter with **Use ATIME**: files go back to the array once they haven't been accessed for a while. Unraid mounts every pool with `noatime`, so access times never change on a read - switch on **Pool Access Times** in Unspin's settings to fix that for the pools it promotes into. Promoted files get the promotion time as their access time, so they aren't demoted on Mover Tuning's next run.

## Screenshots

*(new screenshots coming)*

## Installation

**Via Community Applications (recommended):** search for **Unspin** and click Install.

**Manual:**

```
plugin install https://github.com/kaedinger/unspin/releases/latest/download/unspin.plg
```

## Pause for Backups

Wrap your backup jobs with pause/unpause over SSH so Unspin doesn't count (or promote) files while your backup tool reads them:

```
ssh root@nas /etc/rc.d/rc.unspin pause borgbackup
borg create ssh://root@nas/mnt/disk1/backups::daily /data
ssh root@nas /etc/rc.d/rc.unspin unpause borgbackup
```

Each lock has a name, and locks stack - multiple backup tools can pause independently, and Unspin only resumes once all of them are released. `rc.unspin status` lists the locks currently held.

### Locks that expire

A normal lock stays until it's released, so if the job that set it dies (killed, crashed, its VM stopped mid-run), Unspin stays paused until you unpause it or reboot. To avoid that, give the lock a time to live and refresh it while the job runs:

```
ssh root@nas /etc/rc.d/rc.unspin pause mybackup --ttl 15m
# ... during the job, every 5 minutes:
ssh root@nas /etc/rc.d/rc.unspin pause mybackup --ttl 15m
# ... at the end:
ssh root@nas /etc/rc.d/rc.unspin unpause mybackup
```

- Calling `pause` again with the same name refreshes the lock; `pause mybackup` without `--ttl` makes it permanent again.
- Times are a number with `s`, `m`, `h` or `d` (e.g. `90s`, `15m`, `2h`, `1d`).
- A lock that isn't refreshed in time is removed and logged as `Pause lock expired - mybackup`.
- `rc.unspin status` shows how long each of these locks has left.

## Requirements

- Unraid 7.0+ (probably even earlier, but tested with 7.2.3 onwards, currently 7.2.4)
- No external dependencies - single C++ binary

## Links

- **GitHub:** https://github.com/kaedinger/unspin
- **Issues / feature requests:** https://github.com/kaedinger/unspin/issues
- **License:** GPLv2

## AI disclaimer / History

Claude has been mainly used to simplify refactoring, restructuring, commenting & documenting, cleaning up etc. It also helped immensely setting up the general structure of an Unraid plugin and with getting the settings page right. BUT: Every functional code line has been human written at some point by meticulously reordering bits on an old hard drive using a VERY small magnet. Jokes aside, this one started as a batch script a few years ago on my old Ubuntu machine Gregory (RIP), then turned into a C++ daemon for a proprietary project I did for a German car company (where it was called Hotfile and already using fanotify). As I started using Unraid this year and wanted to give something back to its fantastic community, I turned it into this plugin.
