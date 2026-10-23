/* Host-side check: gcc -O2 -I../source test_playlog.c ../source/playlog.c -o test_playlog
 * ./test_playlog cdbackup.vff */
#include <stdio.h>
#include <stdlib.h>
#include "playlog.h"
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s cdbackup.vff\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz); if (fread(buf, 1, sz, f) != (size_t)sz) return 1; fclose(f);
    PlayLog pl; int r = playlog_load_mem(buf, sz, &pl);
    if (r) { fprintf(stderr, "load failed: %d\n", r); return 1; }
    printf("sessions %zu, titles %zu, days played %zu, total %.1f h, skipped %zu\n",
           pl.n_sessions, pl.n_titles, pl.n_days, pl.total_secs / 3600.0, pl.skipped);
    for (size_t i = 0; i < pl.n_titles && i < 15; i++) {
        TitleStat *t = &pl.titles[i]; int y1,m1,d1,y2,m2,d2;
        pl_civil((uint32_t)(t->first/86400), &y1,&m1,&d1); pl_civil((uint32_t)(t->last/86400), &y2,&m2,&d2);
        printf("%-6.6s %-28.28s %7.1fh %4u sess %4u days  %04d-%02d-%02d .. %04d-%02d-%02d\n",
               t->code, t->name, t->secs / 3600.0, t->sessions, t->days, y1,m1,d1,y2,m2,d2);
    }
    playlog_free(&pl); free(buf); return 0;
}
