#include "playlog.h"
#include <stdlib.h>
#include <string.h>

#define VFF_CL      0x200u
#define VFF_FAT     0x20u
#define VFF_ROOT    0x28020u
#define VFF_ROOTSZ  0x1000u
#define VFF_DATA    0x29020u
#define MAX_CHAIN   8192u
#define MAX_DEPTH   10

typedef struct {
    pl_read_fn rd; void *ud; size_t size;
    uint8_t *fat;
    PlaySession *s; size_t n, cap;
    size_t skipped;
    int oom;
} Ctx;

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64be(const uint8_t *p) {
    uint64_t r = 0; for (int i = 0; i < 8; i++) r = (r << 8) | p[i]; return r;
}

#define FAT_BYTES 0x14000u
static uint32_t fat_next(const Ctx *c, uint32_t cl) {
    size_t off = (size_t)cl * 2;
    if (off + 2 > FAT_BYTES) return 0xFFFF;
    return rd16le(c->fat + off);
}

/* Read a cluster chain into a malloc'd buffer. fsize==0 means "whole chain" (directories). */
static uint8_t *read_chain(const Ctx *c, uint32_t cl, uint32_t fsize, size_t *outlen) {
    size_t cap = 0, len = 0; uint8_t *buf = NULL;
    for (unsigned n = 0; cl >= 2 && cl < 0xFFF0 && n < MAX_CHAIN; n++) {
        size_t off = VFF_DATA + (size_t)(cl - 2) * VFF_CL;
        if (off + VFF_CL > c->size) break;
        if (len + VFF_CL > cap) {
            cap = cap ? cap * 2 : 4 * VFF_CL;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        if (c->rd(c->ud, off, buf + len, VFF_CL) != 0) break;
        len += VFF_CL;
        if (fsize && len >= fsize) break;
        cl = fat_next(c, cl);
    }
    if (fsize && len > fsize) len = fsize;
    *outlen = len;
    return buf;
}

static void utf16be_to_utf8(const uint8_t *in, size_t maxch, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < maxch; i++) {
        uint32_t u = (in[i * 2] << 8) | in[i * 2 + 1];
        if (!u) break;
        if (u >= 0xD800 && u < 0xE000) continue;
        if (u < 0x80)       { if (o + 1 >= outsz) break; out[o++] = (char)u; }
        else if (u < 0x800) { if (o + 2 >= outsz) break; out[o++] = (char)(0xC0 | (u >> 6)); out[o++] = (char)(0x80 | (u & 0x3F)); }
        else                { if (o + 3 >= outsz) break; out[o++] = (char)(0xE0 | (u >> 12)); out[o++] = (char)(0x80 | ((u >> 6) & 0x3F)); out[o++] = (char)(0x80 | (u & 0x3F)); }
    }
    out[o] = 0;
}

static int push(Ctx *c, const PlaySession *s) {
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        PlaySession *p = (PlaySession *)realloc(c->s, nc * sizeof *p);
        if (!p) return -1;
        c->s = p; c->cap = nc;
    }
    c->s[c->n++] = *s;
    return 0;
}

static void parse_log_file(Ctx *c, const uint8_t *b, size_t len) {
    if (len < 0x500 || memcmp(b, "CDBFILE\x02", 8) != 0 || memcmp(b + 0x14, "playtimelog", 11) != 0) return;
    size_t m = 0; int found = 0;
    for (m = 0x400; m + 4 <= len; m++) if (!memcmp(b + m, "03_0", 4)) { found = 1; break; }
    if (!found) return;
    for (size_t n = m + 0x14; n + 0x6A <= len; n += 0x88) {
        if (b[n] == 0 && b[n + 1] == 0) break;
        uint64_t st = rd64be(b + n + 0x54), en = rd64be(b + n + 0x5C);
        if (en < st) { c->skipped++; continue; }
        uint64_t secs = (en - st) / PL_TICKS_PER_SEC;
        if (secs > PL_MAX_SESSION_SECS) { c->skipped++; continue; }
        PlaySession s; memset(&s, 0, sizeof s);
        utf16be_to_utf8(b + n, 32, s.name, sizeof s.name);
        memcpy(s.code, b + n + 0x64, 6); s.code[6] = 0;
        s.start = st / PL_TICKS_PER_SEC + PL_UNIX_OFFSET;
        s.secs = (uint32_t)secs;
        if (push(c, &s) < 0) { c->oom = 1; return; }
    }
}

static void walk_dir(Ctx *c, const uint8_t *d, size_t len, int depth) {
    for (size_t i = 0; i + 32 <= len && !c->oom; i += 32) {
        const uint8_t *e = d + i;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
        if (e[0] == '.') continue;
        uint32_t cl = rd16le(e + 26), fsize = rd32le(e + 28);
        if (e[11] & 0x10) {
            if (depth >= MAX_DEPTH) continue;
            size_t dl; uint8_t *sub = read_chain(c, cl, 0, &dl);
            if (sub) { walk_dir(c, sub, dl, depth + 1); free(sub); }
        } else if (fsize >= 0x500 && fsize <= 0x10000) {
            size_t fl; uint8_t *f = read_chain(c, cl, fsize, &fl);
            if (f) { parse_log_file(c, f, fl); free(f); }
        }
    }
}

static int cmp_start(const void *a, const void *b) {
    const PlaySession *x = (const PlaySession *)a, *y = (const PlaySession *)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return memcmp(x->code, y->code, 6);
}
static int cmp_title(const void *a, const void *b) {
    const TitleStat *x = (const TitleStat *)a, *y = (const TitleStat *)b;
    return x->secs == y->secs ? 0 : (x->secs < y->secs ? 1 : -1);
}

int playlog_load(pl_read_fn rd, void *ud, size_t size, PlayLog *out) {
    memset(out, 0, sizeof *out);
    uint8_t hdr[32];
    if (size < VFF_DATA + VFF_CL || rd(ud, 0, hdr, 32) != 0 ||
        memcmp(hdr, "VFF ", 4) != 0 || hdr[4] != 0xFE || hdr[5] != 0xFF) return -1;
    Ctx c; memset(&c, 0, sizeof c); c.rd = rd; c.ud = ud; c.size = size;
    /* FAT is read as 0x14020 bytes starting at offset 0 (we skip the first 0x20) */
    c.fat = (uint8_t *)malloc(FAT_BYTES);
    uint8_t *root = (uint8_t *)malloc(VFF_ROOTSZ);
    if (!c.fat || !root) { free(c.fat); free(root); return -2; }
    for (size_t o = 0; o < FAT_BYTES; o += VFF_CL) {
        if (rd(ud, VFF_FAT + o, c.fat + o, VFF_CL) != 0) { free(c.fat); free(root); return -1; }
    }
    for (size_t o = 0; o < VFF_ROOTSZ; o += VFF_CL) {
        if (rd(ud, VFF_ROOT + o, root + o, VFF_CL) != 0) { free(c.fat); free(root); return -1; }
    }
    walk_dir(&c, root, VFF_ROOTSZ, 0);
    free(root); free(c.fat);
    if (c.oom) { free(c.s); return -2; }

    qsort(c.s, c.n, sizeof *c.s, cmp_start);
    /* drop exact duplicates (same code, start, duration) */
    size_t w = 0;
    for (size_t i = 0; i < c.n; i++) {
        if (w && c.s[i].start == c.s[w-1].start && c.s[i].secs == c.s[w-1].secs &&
            !memcmp(c.s[i].code, c.s[w-1].code, 6)) { c.skipped++; continue; }
        c.s[w++] = c.s[i];
    }
    c.n = w;

    TitleStat *t = NULL; size_t nt = 0;
    DayStat *dy = NULL; size_t nd = 0;
    if (c.n) {
        t = (TitleStat *)calloc(c.n, sizeof *t);       /* upper bound */
        dy = (DayStat *)calloc(c.n, sizeof *dy);
        if (!t || !dy) { free(t); free(dy); free(c.s); return -2; }
    }
    uint64_t total = 0;
    for (size_t i = 0; i < c.n; i++) {
        PlaySession *s = &c.s[i];
        uint32_t day = (uint32_t)(s->start / 86400u);
        size_t k;
        for (k = 0; k < nt; k++) if (!memcmp(t[k].code, s->code, 6)) break;
        if (k == nt) {
            memcpy(t[k].code, s->code, 7); t[k].first = s->start; t[k].last_day = 0xFFFFFFFFu; nt++;
        }
        strcpy(t[k].name, s->name);
        t[k].sessions++; t[k].secs += s->secs; t[k].last = s->start;
        if (t[k].last_day != day) { t[k].days++; t[k].last_day = day; }
        if (nd && dy[nd-1].day == day) { dy[nd-1].secs += s->secs; dy[nd-1].sessions++; }
        else { dy[nd].day = day; dy[nd].secs = s->secs; dy[nd].sessions = 1; nd++; }
        total += s->secs;
    }
    qsort(t, nt, sizeof *t, cmp_title);
    out->sessions = c.s; out->n_sessions = c.n;
    out->titles = t; out->n_titles = nt;
    out->days = dy; out->n_days = nd;
    out->total_secs = total; out->skipped = c.skipped;
    return 0;
}

void playlog_free(PlayLog *pl) {
    free(pl->sessions); free(pl->titles); free(pl->days);
    memset(pl, 0, sizeof *pl);
}

void pl_civil(uint32_t days, int *y, int *m, int *d) {
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

int pl_weekday(uint32_t days) { return (int)((days + 4) % 7); } /* 1970-01-01 was a Thursday */

typedef struct { const uint8_t *v; size_t size; } MemSrc;
static int mem_read(void *ud, size_t off, void *dst, size_t len) {
    const MemSrc *m = (const MemSrc *)ud;
    if (off + len > m->size) return -1;
    memcpy(dst, m->v + off, len);
    return 0;
}
int playlog_load_mem(const uint8_t *vff, size_t size, PlayLog *out) {
    MemSrc m = { vff, size };
    return playlog_load(mem_read, &m, size, out);
}

uint32_t pl_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}
