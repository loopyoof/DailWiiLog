/*
 * Daily Log - Wii play history viewer (Wii / vWii homebrew)
 *
 * SD layout (all next to boot.elf / boot.dol):
 *   audio.ogg   background music, replace any time (optional)
 *   font.ttf    UI font, replace any time
 *   cdbackup.vff  optional fallback if the NAND copy can't be read
 *
 * Controls: +/- switch screen, D-pad move, A latest day (calendar), 1/2 prev/next month
 *           (calendar), B mute music, HOME exit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <wchar.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <asndlib.h>
#include <grrlib.h>
#include <tremor/ivorbisfile.h>
#include "playlog.h"

/* ------------------------------------------------------------------ colours */
#define C_BG      0xF1F4F7FF
#define C_CARD    0xFFFFFFFF
#define C_CYAN    0x1EA7E1FF
#define C_CYAN_LT 0xCDEBF8FF
#define C_TEXT    0x474B52FF
#define C_SUB     0x8A9099FF
#define C_LINE    0xDDE2E8FF
#define C_ACCENT  0xF5A623FF
#define C_WHITE   0xFFFFFFFF

static const char *MONTHS[12] = { "January","February","March","April","May","June","July",
                                  "August","September","October","November","December" };
static const char *MON3[12]   = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *WDAY[7]    = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

/* ------------------------------------------------------------------ paths */
static char appdir[256] = "sd:/apps/dailylog/";
static void init_paths(int argc, char **argv) {
    if (argc > 0 && argv && argv[0] && strrchr(argv[0], '/')) {
        strncpy(appdir, argv[0], sizeof appdir - 1); appdir[sizeof appdir - 1] = 0;
        *(strrchr(appdir, '/') + 1) = 0;
    }
}
static void path_in_app(char *out, size_t n, const char *name) { snprintf(out, n, "%s%s", appdir, name); }

/* ------------------------------------------------------------------ fatal error (console) */
static void fatal(const char *msg) {
    VIDEO_Init();
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rm));
    console_init(xfb, 20, 20, rm->fbWidth, rm->xfbHeight, rm->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rm); VIDEO_SetNextFramebuffer(xfb); VIDEO_SetBlack(false); VIDEO_Flush(); VIDEO_WaitVSync();
    printf("\n\n  Daily Log\n\n  %s\n\n  Press HOME to exit.\n", msg);
    WPAD_Init();
    for (;;) { WPAD_ScanPads(); if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0); VIDEO_WaitVSync(); }
}

/* ------------------------------------------------------------------ play log loading */
static char nand_path[] ATTRIBUTE_ALIGN(32) = "/title/00000001/00000002/data/cdb.vff";
static s32 g_fd = -1;
static u8 *g_bounce = NULL;

static int rd_isfs(void *ud, size_t off, void *dst, size_t len) {
    (void)ud;
    if (len > PL_READ_MAX) return -1;
    if (ISFS_Seek(g_fd, (s32)off, 0) < 0) return -1;
    if (ISFS_Read(g_fd, g_bounce, (u32)len) != (s32)len) return -1;
    memcpy(dst, g_bounce, len);
    return 0;
}
static int rd_file(void *ud, size_t off, void *dst, size_t len) {
    FILE *f = (FILE *)ud;
    if (fseek(f, (long)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

static int load_from_nand(PlayLog *pl) {
    if (ISFS_Initialize() < 0) return -1;
    int r = -1;
    s32 fd = ISFS_Open(nand_path, ISFS_OPEN_READ);
    if (fd >= 0) {
        fstats *st = (fstats *)memalign(32, sizeof(fstats));
        g_bounce = (u8 *)memalign(32, PL_READ_MAX);
        if (st && g_bounce && ISFS_GetFileStats(fd, st) >= 0 && st->file_length > 0) {
            g_fd = fd;
            r = playlog_load(rd_isfs, NULL, st->file_length, pl);
        }
        free(st); free(g_bounce); g_bounce = NULL;
        ISFS_Close(fd); g_fd = -1;
    }
    ISFS_Deinitialize();
    return r;
}

static int load_from_file(const char *path, PlayLog *pl) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long size = ftell(f);
    int r = size > 0 ? playlog_load(rd_file, f, (size_t)size, pl) : -1;
    fclose(f);
    return r;
}

static char g_source[64] = "";
static int load_play_log(PlayLog *pl) {
    char p[300];
    if (load_from_nand(pl) == 0 && pl->n_sessions) { strcpy(g_source, "Wii NAND"); return 0; }
    const char *names[] = { "cdbackup.vff", "cdb.vff" };
    for (int i = 0; i < 2; i++) {
        path_in_app(p, sizeof p, names[i]);
        if (load_from_file(p, pl) == 0 && pl->n_sessions) { strcpy(g_source, names[i]); return 0; }
    }
    if (load_from_file("sd:/cdbackup.vff", pl) == 0 && pl->n_sessions) { strcpy(g_source, "sd:/cdbackup.vff"); return 0; }
    return -1;
}

/* ------------------------------------------------------------------ audio (Ogg Vorbis, looped) */
#define ABUF 16384
static u8 abuf[2][ABUF] ATTRIBUTE_ALIGN(32);
static OggVorbis_File vf;
static FILE *afile = NULL;
static lwp_t athread;
static volatile bool arun = false;
static bool a_muted = false;
static int a_fmt, a_rate;

static void a_fill(u8 *dst) {
    int got = 0, zero = 0;
    while (got < ABUF) {
        int bs; long r = ov_read(&vf, (char *)dst + got, ABUF - got, &bs);
        if (r > 0) { got += (int)r; zero = 0; }
        else if (r == 0) { if (++zero > 2) break; ov_time_seek(&vf, 0); } /* end of file: loop */
        else break;
    }
    if (got < ABUF) memset(dst + got, 0, ABUF - got);
}

static void *a_thread(void *arg) {
    (void)arg;
    a_fill(abuf[0]);
    ASND_SetVoice(0, a_fmt, a_rate, 0, abuf[0], ABUF, 200, 200, NULL);
    int freeb = 1;
    while (arun) {
        if (ASND_StatusVoice(0) == SND_WAITING) {
            a_fill(abuf[freeb]);
            ASND_AddVoice(0, abuf[freeb], ABUF);
            freeb ^= 1;
        }
        usleep(2000);
    }
    ASND_StopVoice(0);
    return NULL;
}

static void audio_start(void) {
    char p[300]; path_in_app(p, sizeof p, "audio.ogg");
    afile = fopen(p, "rb");
    if (!afile) return;                                   /* no music file: run silently */
    if (ov_open(afile, &vf, NULL, 0) < 0) { fclose(afile); afile = NULL; return; }
    vorbis_info *vi = ov_info(&vf, -1);
    if (!vi || vi->channels < 1 || vi->channels > 2) { ov_clear(&vf); afile = NULL; return; }
    a_fmt = vi->channels == 1 ? VOICE_MONO_16BIT : VOICE_STEREO_16BIT;
    a_rate = (int)vi->rate;
    arun = true;
    LWP_CreateThread(&athread, a_thread, NULL, NULL, 16 * 1024, 64);
}
static void audio_stop(void) {
    if (!arun) return;
    arun = false; LWP_JoinThread(athread, NULL);
    ov_clear(&vf); afile = NULL;
}
static void audio_toggle_mute(void) { if (!arun) return; a_muted = !a_muted; ASND_PauseVoice(0, a_muted ? 1 : 0); }

/* ------------------------------------------------------------------ text helpers */
static GRRLIB_ttfFont *font = NULL;
static u8 *fontdata = NULL;

static void to_w(const char *s, wchar_t *o, size_t max) {
    size_t n = 0;
    while (*s && n + 1 < max) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) { o[n++] = c; s++; }
        else if ((c & 0xE0) == 0xC0 && s[1]) { o[n++] = ((c & 0x1F) << 6) | (s[1] & 0x3F); s += 2; }
        else if ((c & 0xF0) == 0xE0 && s[1] && s[2]) { o[n++] = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3; }
        else s++;
    }
    o[n] = 0;
}
static int tw(int size, const char *s) { wchar_t w[160]; to_w(s, w, 160); return (int)GRRLIB_WidthTTFW(font, w, size); }
static void txt(int x, int y, int size, u32 col, const char *s) { wchar_t w[160]; to_w(s, w, 160); GRRLIB_PrintfTTFW(x, y, font, w, size, col); }
static void txtr(int xr, int y, int size, u32 col, const char *s) { txt(xr - tw(size, s), y, size, col, s); }
static void txtc(int xc, int y, int size, u32 col, const char *s) { txt(xc - tw(size, s) / 2, y, size, col, s); }
static void txt_fit(int x, int y, int maxw, int size, u32 col, const char *s) {
    char buf[110]; strncpy(buf, s, sizeof buf - 4); buf[sizeof buf - 4] = 0;
    size_t n = strlen(buf);
    while (n > 1 && tw(size, buf) > maxw) { n--; while (n > 1 && (buf[n] & 0xC0) == 0x80) n--; buf[n] = '.'; buf[n+1] = '.'; buf[n+2] = '.'; buf[n+3] = 0; }
    txt(x, y, size, col, buf);
}
static void box(int x, int y, int w, int h, u32 col) { GRRLIB_Rectangle(x, y, w, h, col, true); }
static void frame(int x, int y, int w, int h, int t, u32 col) { box(x, y, w, t, col); box(x, y + h - t, w, t, col); box(x, y, t, h, col); box(x + w - t, y, t, h, col); }

static void fmt_dur(char *b, size_t n, uint64_t s) {
    unsigned long long h = s / 3600, m = (s % 3600) / 60;
    if (h) snprintf(b, n, "%lluh %02llum", h, m); else snprintf(b, n, "%llum", m);
}
static void fmt_date(char *b, size_t n, uint64_t unix_s) {
    int y, m, d; pl_civil((uint32_t)(unix_s / 86400), &y, &m, &d);
    snprintf(b, n, "%s %d, %d", MON3[m - 1], d, y);
}
static u32 blend(u32 a, u32 b, float t) {
    u32 r = 0;
    for (int sh = 24; sh >= 0; sh -= 8) {
        float ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        r |= ((u32)(ca + (cb - ca) * t) & 0xFF) << sh;
    }
    return r;
}

/* ------------------------------------------------------------------ app state */
static PlayLog PL;
enum { SCR_OVERVIEW, SCR_CALENDAR, SCR_GAMES, SCR_COUNT };
static int screen = SCR_OVERVIEW;
static uint32_t sel_day;       /* calendar selection, days since 1970 */
static int title_sel = 0;

static const DayStat *find_day(uint32_t day) {
    size_t lo = 0, hi = PL.n_days;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (PL.days[mid].day < day) lo = mid + 1; else hi = mid; }
    return (lo < PL.n_days && PL.days[lo].day == day) ? &PL.days[lo] : NULL;
}
static size_t first_session_on(uint32_t day) {
    uint64_t t = (uint64_t)day * 86400u; size_t lo = 0, hi = PL.n_sessions;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (PL.sessions[mid].start < t) lo = mid + 1; else hi = mid; }
    return lo;
}
static int days_in_month(int y, int m) {
    int ny = m == 12 ? y + 1 : y, nm = m == 12 ? 1 : m + 1;
    return (int)(pl_days_from_civil(ny, nm, 1) - pl_days_from_civil(y, m, 1));
}
static void shift_month(int delta) {
    int y, m, d; pl_civil(sel_day, &y, &m, &d);
    m += delta; while (m < 1) { m += 12; y--; } while (m > 12) { m -= 12; y++; }
    int dim = days_in_month(y, m); if (d > dim) d = dim;
    sel_day = pl_days_from_civil(y, m, d);
}

/* ------------------------------------------------------------------ drawing */
static void draw_header(void) {
    box(0, 0, 640, 52, C_CYAN);
    txt(22, 9, 30, C_WHITE, "Daily Log");
    const char *tabs[SCR_COUNT] = { "Overview", "Calendar", "Games" };
    for (int i = 0; i < SCR_COUNT; i++) {
        int x = 270 + i * 120;
        if (i == screen) { box(x, 12, 110, 28, C_WHITE); txtc(x + 55, 15, 18, C_CYAN, tabs[i]); }
        else txtc(x + 55, 15, 18, C_WHITE, tabs[i]);
    }
}
static void draw_footer(const char *hint) {
    box(0, 452, 640, 28, C_CARD); box(0, 452, 640, 1, C_LINE);
    txt(16, 458, 15, C_SUB, hint);
    if (a_muted) txtr(624, 458, 15, C_ACCENT, "music off");
}

static void screen_overview(void) {
    char b[64], b2[64];
    struct { const char *label; char val[32]; } cards[4];
    snprintf(cards[0].val, 32, "%.1f h", PL.total_secs / 3600.0); cards[0].label = "Total play time";
    snprintf(cards[1].val, 32, "%u", (unsigned)PL.n_days);        cards[1].label = "Days played";
    snprintf(cards[2].val, 32, "%u", (unsigned)PL.n_titles);      cards[2].label = "Games";
    snprintf(cards[3].val, 32, "%u", (unsigned)PL.n_sessions);    cards[3].label = "Sessions";
    for (int i = 0; i < 4; i++) {
        int x = 20 + i * 152;
        box(x, 68, 142, 70, C_CARD); box(x, 68, 142, 4, C_CYAN);
        txtc(x + 71, 82, 26, C_TEXT, cards[i].val);
        txtc(x + 71, 114, 15, C_SUB, cards[i].label);
    }
    txt(20, 152, 18, C_TEXT, "Most played");
    uint64_t top = PL.n_titles ? PL.titles[0].secs : 1;
    for (int i = 0; i < 6 && (size_t)i < PL.n_titles; i++) {
        const TitleStat *t = &PL.titles[i]; int y = 178 + i * 30;
        txt_fit(20, y, 230, 16, C_TEXT, t->name);
        int bw = (int)(300.0 * t->secs / top); if (bw < 3) bw = 3;
        box(260, y + 3, 300, 16, C_LINE); box(260, y + 3, bw, 16, C_CYAN);
        fmt_dur(b, sizeof b, t->secs); txtr(620, y, 16, C_SUB, b);
    }
    txt(20, 364, 18, C_TEXT, "Recent days");
    int n = PL.n_days < 7 ? (int)PL.n_days : 7; uint32_t maxs = 1;
    for (int i = 0; i < n; i++) { uint32_t s = PL.days[PL.n_days - n + i].secs; if (s > maxs) maxs = s; }
    for (int i = 0; i < n; i++) {
        const DayStat *d = &PL.days[PL.n_days - n + i]; int x = 40 + i * 84;
        int h = (int)(44.0 * d->secs / maxs); if (h < 3) h = 3;
        box(x, 436 - h - 14, 56, h, C_CYAN);
        int y, m, dd; pl_civil(d->day, &y, &m, &dd);
        snprintf(b, sizeof b, "%d/%d", m, dd); txtc(x + 28, 428, 13, C_SUB, b);
        fmt_dur(b2, sizeof b2, d->secs); txtc(x + 28, 436 - h - 32, 13, C_TEXT, b2);
    }
    draw_footer("+/-  switch screen      B  music on/off      HOME  exit");
}

static void screen_calendar(void) {
    int y, m, d; pl_civil(sel_day, &y, &m, &d);
    char b[96];
    snprintf(b, sizeof b, "%s %d", MONTHS[m - 1], y);
    txtc(320, 62, 24, C_TEXT, b);
    txt(40, 66, 18, C_CYAN, "<  1"); txtr(600, 66, 18, C_CYAN, "2  >");
    uint32_t first = pl_days_from_civil(y, m, 1); int dim = days_in_month(y, m), wd0 = pl_weekday(first);
    for (int i = 0; i < 7; i++) txtc(80 + i * 80, 96, 14, C_SUB, WDAY[i]);
    for (int cell = 0; cell < 42; cell++) {
        int dn = cell - wd0 + 1; if (dn < 1 || dn > dim) continue;
        int cx = 40 + (cell % 7) * 80, cy = 114 + (cell / 7) * 40;
        uint32_t day = first + (uint32_t)(dn - 1);
        const DayStat *ds = find_day(day);
        float t = ds ? (ds->secs / 10800.0f) : 0; if (t > 1) t = 1; if (ds && t < 0.15f) t = 0.15f;
        box(cx + 2, cy + 2, 76, 36, ds ? blend(C_CYAN_LT, C_CYAN, t) : C_CARD);
        snprintf(b, sizeof b, "%d", dn);
        txt(cx + 8, cy + 4, 14, ds && t > 0.6f ? C_WHITE : C_TEXT, b);
        if (ds) { snprintf(b, sizeof b, "%.1fh", ds->secs / 3600.0); txtr(cx + 74, cy + 20, 13, t > 0.6f ? C_WHITE : C_TEXT, b); }
        if (day == sel_day) frame(cx + 1, cy + 1, 78, 38, 3, C_ACCENT);
    }
    /* sessions of the selected day */
    box(20, 358, 600, 90, C_CARD);
    snprintf(b, sizeof b, "%s, %s %d, %d", WDAY[pl_weekday(sel_day)], MON3[m - 1], d, y);
    txt(30, 362, 16, C_TEXT, b);
    const DayStat *ds = find_day(sel_day);
    if (!ds) txt(30, 392, 15, C_SUB, "No play recorded");
    else {
        size_t i = first_session_on(sel_day); int row = 0;
        for (; i < PL.n_sessions && PL.sessions[i].start / 86400u == sel_day; i++, row++) {
            if (row == 3) { txt(30, 362 + 20 + row * 18, 13, C_SUB, "..."); break; }
            const PlaySession *s = &PL.sessions[i]; unsigned hh = (unsigned)(s->start % 86400u) / 3600, mm = (unsigned)(s->start % 3600u) / 60;
            snprintf(b, sizeof b, "%02u:%02u", hh, mm); txt(30, 384 + row * 18, 14, C_SUB, b);
            txt_fit(90, 384 + row * 18, 380, 14, C_TEXT, s->name);
            fmt_dur(b, sizeof b, s->secs); txtr(606, 384 + row * 18, 14, C_TEXT, b);
        }
    }
    draw_footer("D-pad  move      1/2  month      A  latest day      +/-  screen");
}

static void screen_games(void) {
    char b[96];
    int per = 9, top = title_sel - 4; if (top < 0) top = 0; if (top > (int)PL.n_titles - per) top = (int)PL.n_titles - per; if (top < 0) top = 0;
    for (int r = 0; r < per && (size_t)(top + r) < PL.n_titles; r++) {
        int i = top + r, y = 66 + r * 42; const TitleStat *t = &PL.titles[i];
        box(16, y, 320, 38, i == title_sel ? C_CYAN_LT : C_CARD);
        if (i == title_sel) box(16, y, 4, 38, C_CYAN);
        txt_fit(28, y + 3, 220, 16, C_TEXT, t->name);
        snprintf(b, sizeof b, "%.1f h", t->secs / 3600.0); txtr(328, y + 3, 16, C_TEXT, b);
        int bw = (int)(200.0 * t->secs / PL.titles[0].secs); if (bw < 2) bw = 2;
        box(28, y + 27, bw, 5, C_CYAN);
    }
    if (PL.n_titles) {
        const TitleStat *t = &PL.titles[title_sel];
        box(348, 66, 276, 376, C_CARD); box(348, 66, 276, 4, C_CYAN);
        txt_fit(362, 82, 250, 20, C_TEXT, t->name);
        snprintf(b, sizeof b, "%.6s", t->code); txt(362, 108, 14, C_SUB, b);
        struct { const char *k; char v[40]; } rows[6];
        fmt_dur(rows[0].v, 40, t->secs);              rows[0].k = "Total play time";
        snprintf(rows[1].v, 40, "%u", t->sessions);   rows[1].k = "Times played";
        snprintf(rows[2].v, 40, "%u", t->days);       rows[2].k = "Days played";
        fmt_dur(rows[3].v, 40, t->secs / t->sessions);rows[3].k = "Average session";
        fmt_date(rows[4].v, 40, t->first);            rows[4].k = "First played";
        fmt_date(rows[5].v, 40, t->last);             rows[5].k = "Last played";
        for (int i = 0; i < 6; i++) {
            int y = 140 + i * 48;
            txt(362, y, 14, C_SUB, rows[i].k); txt(362, y + 18, 20, C_TEXT, rows[i].v);
            box(362, y + 44, 248, 1, C_LINE);
        }
    }
    draw_footer("D-pad up/down  select      +/-  screen      B  music on/off");
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    init_paths(argc, argv);
    fatInitDefault();

    char p[300]; path_in_app(p, sizeof p, "font.ttf");
    FILE *f = fopen(p, "rb");
    if (!f) fatal("font.ttf not found next to boot.elf.");
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    fontdata = (u8 *)malloc(fsz);
    if (!fontdata || fread(fontdata, 1, fsz, f) != (size_t)fsz) fatal("Could not read font.ttf.");
    fclose(f);

    GRRLIB_Init(); WPAD_Init();
    font = GRRLIB_LoadTTF(fontdata, (s32)fsz);
    if (!font) { GRRLIB_Exit(); fatal("font.ttf is not a usable TrueType font."); }
    ASND_Init(); ASND_Pause(0);

    GRRLIB_FillScreen(C_BG); draw_header(); txtc(320, 220, 20, C_TEXT, "Reading play history...");
    GRRLIB_Render();
    audio_start();

    int rc = load_play_log(&PL);
    bool ok = rc == 0 && PL.n_days > 0;
    if (ok) { sel_day = PL.days[PL.n_days - 1].day; }

    while (1) {
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        if (down & WPAD_BUTTON_HOME) break;
        if (down & WPAD_BUTTON_B) audio_toggle_mute();
        if (ok) {
            if (down & WPAD_BUTTON_PLUS)  screen = (screen + 1) % SCR_COUNT;
            if (down & WPAD_BUTTON_MINUS) screen = (screen + SCR_COUNT - 1) % SCR_COUNT;
            if (screen == SCR_CALENDAR) {
                if (down & WPAD_BUTTON_LEFT)  sel_day -= 1;
                if (down & WPAD_BUTTON_RIGHT) sel_day += 1;
                if (down & WPAD_BUTTON_UP)    sel_day -= 7;
                if (down & WPAD_BUTTON_DOWN)  sel_day += 7;
                if (down & WPAD_BUTTON_1) shift_month(-1);
                if (down & WPAD_BUTTON_2) shift_month(+1);
                if (down & WPAD_BUTTON_A) sel_day = PL.days[PL.n_days - 1].day;
            } else if (screen == SCR_GAMES) {
                if ((down & WPAD_BUTTON_UP) && title_sel > 0) title_sel--;
                if ((down & WPAD_BUTTON_DOWN) && title_sel + 1 < (int)PL.n_titles) title_sel++;
            }
        }
        GRRLIB_FillScreen(C_BG);
        if (!ok) {
            draw_header();
            txtc(320, 190, 20, C_TEXT, "Couldn't read the play history.");
            txtc(320, 224, 15, C_SUB, "Needs access to the Wii NAND (cdb.vff), or put cdbackup.vff in the app folder.");
            draw_footer("HOME  exit");
        } else {
            draw_header();
            if (screen == SCR_OVERVIEW) screen_overview();
            else if (screen == SCR_CALENDAR) screen_calendar();
            else screen_games();
        }
        GRRLIB_Render();
    }
    audio_stop();
    ASND_End();
    playlog_free(&PL);
    GRRLIB_FreeTTF(font); GRRLIB_Exit(); free(fontdata);
    exit(0);
}
