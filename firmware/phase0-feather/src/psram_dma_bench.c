// Phase 0 B2, PSRAM-backed scanout half -- see ../../README.md section 8
// ("Phase 0 - Part B, B2") and section 3.3's PSRAM-backed 640x480 16bpp
// video mode, rated "Medium - needs measurement, but no longer gates
// v1". Target from that section: line-buffer DMA at ~37 MB/s for
// 640x480@60 scanout.
//
// B1 measured CPU-driven *writes* into PSRAM (ctx's rasteriser, one
// scalar store at a time). This measures DMA-driven *reads* out of
// PSRAM -- what real HSTX scanout would actually do -- a materially
// different access pattern: the DMA engine can pipeline QMI
// transactions back to back with no per-word CPU instruction overhead
// between them, so there's no reason to expect B1's ~8.7 MB/s ceiling
// to apply here.
//
// Reads via the *uncached* XIP alias for CS1 (0x15000000+, mirroring
// 0x11000000+'s cached one) rather than the cached alias main.c/
// ctx_bench.c use. Real scanout reads different framebuffer content
// every frame, so it should never hit in the small on-chip XIP cache;
// using the cached alias here could make repeated timing runs look
// faster than a real scanout stream would be.

#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/psram.h"
#include "hardware/flash.h"

#include "psram_dma_bench.h"

#define BULK_BYTES (256 * 1024)
#define LINE_BYTES (640 * 2)  // one 640-wide RGB565 scanline
#define FRAME_BYTES (640 * 480 * 2)
#define ITERATIONS 10

static uint8_t bulk_dst[BULK_BYTES] __attribute__((aligned(4)));
static uint8_t line_dst[LINE_BYTES] __attribute__((aligned(4)));

static int64_t timed_dma_copy(int chan, void *dst, const void *src, size_t bytes) {
    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    absolute_time_t start = get_absolute_time();
    dma_channel_configure(chan, &c, dst, src, bytes / 4, true);
    dma_channel_wait_for_finish_blocking(chan);
    return absolute_time_diff_us(start, get_absolute_time());
}

// One big DMA transfer per iteration -- the achievable ceiling, without
// per-transaction setup overhead.
static void run_bulk(int chan, const uint8_t *psram_base) {
    int64_t total_us = 0, min_us = -1, max_us = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        int64_t us = timed_dma_copy(chan, bulk_dst, psram_base, BULK_BYTES);
        total_us += us;
        if (min_us < 0 || us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }

    double avg_us = (double)total_us / ITERATIONS;
    printf("  %-8s %3u KB in one transfer   min %6.2f ms  avg %6.2f ms  max %6.2f ms  [~%.2f MB/s]\n",
           "bulk", BULK_BYTES / 1024, min_us / 1000.0, avg_us / 1000.0, max_us / 1000.0,
           (double)BULK_BYTES / (avg_us / 1e6) / (1024 * 1024));
}

// One DMA transfer per scanline, 480 of them per iteration -- closer to
// how real chained per-line HSTX scanout descriptors would actually
// drive this (see README section 3.4), so it captures per-transaction
// setup cost the bulk number above doesn't.
static void run_per_line(int chan, const uint8_t *psram_base) {
    const int lines = FRAME_BYTES / LINE_BYTES;  // 480
    int64_t total_us = 0, min_us = -1, max_us = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        absolute_time_t start = get_absolute_time();
        for (int line = 0; line < lines; line++) {
            const uint8_t *src = psram_base + (size_t)line * LINE_BYTES;
            dma_channel_config c = dma_channel_get_default_config(chan);
            channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
            channel_config_set_read_increment(&c, true);
            channel_config_set_write_increment(&c, true);
            dma_channel_configure(chan, &c, line_dst, src, LINE_BYTES / 4, true);
            dma_channel_wait_for_finish_blocking(chan);
        }
        int64_t us = absolute_time_diff_us(start, get_absolute_time());
        total_us += us;
        if (min_us < 0 || us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }

    double avg_us = (double)total_us / ITERATIONS;
    printf("  %-8s %d x %d B (one frame)   min %6.2f ms  avg %6.2f ms  max %6.2f ms  [~%.2f MB/s, %.1f fps]\n",
           "per-line", lines, LINE_BYTES, min_us / 1000.0, avg_us / 1000.0, max_us / 1000.0,
           (double)FRAME_BYTES / (avg_us / 1e6) / (1024 * 1024), 1e6 / avg_us);
}

void psram_dma_bench_run(void) {
    printf("\nphase0-feather: B2 PSRAM-backed scanout (DMA read) benchmark\n");

    if (!psram_is_available()) {
        printf("  skipped: PSRAM not available\n");
        return;
    }

    int chan = dma_claim_unused_channel(false);
    if (chan < 0) {
        printf("  skipped: no free DMA channel\n");
        return;
    }

    const uint8_t *psram_base = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE +
                                                    flash_devinfo_size_to_bytes(FLASH_DEVINFO_SIZE_MAX));

    printf("  %d iterations each; target for 640x480@60 scanout: ~37 MB/s (README section 3.3)\n",
           ITERATIONS);
    run_bulk(chan, psram_base);
    run_per_line(chan, psram_base);

    dma_channel_unclaim(chan);
}
