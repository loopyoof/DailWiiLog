# Daily Log (Wii / vWii homebrew)

Shows your Wii play history: total hours, days played, a play calendar, per-game stats.
Reads the Wii Message Board play log (`/title/00000001/00000002/data/cdb.vff`) directly.

## Build (needs devkitPro with devkitPPC/libogc)
    dkp-pacman -S wii-dev ppc-freetype ppc-libpng ppc-libjpeg-turbo ppc-zlib ppc-libvorbisidec
    # GRRLIB is not in the repos: build it from https://github.com/GRRLIB/GRRLIB  (make clean all install)
    make            # produces boot.elf and boot.dol
    make install    # copies them into sdcard/apps/dailylog/
Then copy `sdcard/apps/dailylog/` to `sd:/apps/dailylog/` and add your `audio.ogg`.
Package/library names vary between devkitPro releases; if the linker or `tremor/ivorbisfile.h`
complains, adjust LIBS in the Makefile / the include path.

## Forwarder
Point a forwarder channel at `sd:/apps/dailylog/boot.elf`, title "Daily Log".

## Controls
+/- switch screen | D-pad move | A latest day (calendar) | 1/2 month (calendar) | B music on/off | HOME exit

## Status
- `source/playlog.[ch]`: parser, tested on a PC against a real vWii dump (1,260 sessions).
  `tools/test_playlog.c` is the PC test.
- `source/main.c`: UI, input and audio. Only syntax-checked (against stub headers), never run
  on a console. Expect small fixes on first build: text positions, ASND buffering, library names.
