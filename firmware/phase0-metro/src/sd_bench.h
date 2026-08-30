#ifndef SD_BENCH_H
#define SD_BENCH_H

// Phase 0 B2 (microSD half): mounts the SD card over SPI0, reports
// capacity/free space, writes+reads-back a small distinctly-named test
// file to prove the stack works (never touches raw sectors or any other
// file, so it can't clobber whatever's already on the card), then times
// writing a larger buffer for a rough bandwidth number. Prints a clear
// "skipped" message and returns cleanly if no card is present or mount
// fails -- never panics.
void sd_bench_run(void);

#endif
