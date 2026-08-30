#ifndef CTX_BENCH_H
#define CTX_BENCH_H

// Runs the B1 ctx rasterisation benchmark: 640x480 RGB565_BYTESWAPPED,
// framebuffer in PSRAM. Prints timing to stdio. Prints a "skipped"
// message instead if PSRAM isn't available or isn't big enough.
void ctx_bench_run(void);

#endif
