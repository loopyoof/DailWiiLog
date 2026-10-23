/*
 * playlog.h - reader for the Wii Message Board play history (cdb.vff)
 *
 * Portable C (no libogc dependency) so it can be tested on a PC.
 * Format notes (reverse-engineered from a real vWii dump):
 *  - cdb.vff is a FAT16-style image: 0x200-byte clusters, FAT at 0x20 (little-endian
 *    16-bit entries), root directory at 0x28020 (0x1000 bytes), cluster 2 at 0x29020.
 *  - Play history lives in files with header "CDBFILE\x02" and type string "playtimelog"
 *    (folder layout: year/month0/day/hour/minute/HAEA_#1/log/XXXXXXXX.000).
 *  - Each file holds up to 10 session records after a "03_0" marker. Record stride 0x88:
 *      +0x00  UTF-16BE title name (0x40 bytes)
 *      +0x54  u64 BE start ticks  (60750000 ticks/s, epoch 2000-01-01, console local time)
 *      +0x5C  u64 BE end ticks
 *      +0x64  char[6] game code (e.g. "RSPE01")
 *  - Days with more than 10 sessions lump the rest into "Others" as text only; that time
 *    cannot be attributed to a title.
 */
#ifndef PLAYLOG_H
#define PLAYLOG_H

#include <stdint.h>
#include <stddef.h>

#define PL_TICKS_PER_SEC     60750000ULL
#define PL_UNIX_OFFSET       946684800ULL      /* 2000-01-01 in unix seconds */
#define PL_MAX_SESSION_SECS  (72ULL * 3600ULL) /* ignore longer "sessions" as corrupt */

typedef struct {
    char     code[7];     /* 6 raw bytes + NUL; may contain NULs for 4-char codes */
    char     name[100];   /* UTF-8 */
    uint64_t start;       /* unix-style seconds, console local time */
    uint32_t secs;        /* duration in seconds */
} PlaySession;

typedef struct {
    char     code[7];
    char     name[100];
    uint32_t sessions;
    uint32_t days;        /* distinct days this title was played */
    uint64_t secs;        /* total seconds */
    uint64_t first, last; /* unix-style seconds of first/last session start */
    uint32_t last_day;    /* internal */
} TitleStat;

typedef struct {
    uint32_t day;         /* days since 1970-01-01 (console local) */
    uint32_t secs;
    uint32_t sessions;
} DayStat;

typedef struct {
    PlaySession *sessions; size_t n_sessions; /* sorted by start */
    TitleStat   *titles;   size_t n_titles;   /* sorted by total time, descending */
    DayStat     *days;     size_t n_days;     /* sorted by day, ascending */
    uint64_t     total_secs;
    size_t       skipped;                     /* records rejected by sanity checks */
} PlayLog;

/* Reader callback: copy len bytes at byte offset off of the VFF image into dst.
 * len is always <= PL_READ_MAX and off is always a multiple of 32 (ISFS-friendly).
 * Return 0 on success. */
#define PL_READ_MAX 512
typedef int (*pl_read_fn)(void *ud, size_t off, void *dst, size_t len);

/* size = total size of the VFF image in bytes.
 * Returns 0 on success, negative on error (-1 bad image/read error, -2 out of memory). */
int  playlog_load(pl_read_fn rd, void *ud, size_t size, PlayLog *out);
/* Convenience wrapper for an image already in memory (PC tests, SD dumps). */
int  playlog_load_mem(const uint8_t *vff, size_t size, PlayLog *out);
void playlog_free(PlayLog *pl);

/* days since 1970-01-01 -> calendar date; weekday 0=Sunday */
void pl_civil(uint32_t days, int *y, int *m, int *d);
int  pl_weekday(uint32_t days);
uint32_t pl_days_from_civil(int y, int m, int d);

#endif
