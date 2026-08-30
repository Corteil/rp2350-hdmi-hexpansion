#ifndef PSRAM_DMA_BENCH_H
#define PSRAM_DMA_BENCH_H

// Phase 0 B2, PSRAM-backed scanout half: measures DMA-driven bulk reads
// out of the PSRAM framebuffer -- what real HSTX scanout would actually
// do. B1 measured the opposite direction (CPU-driven writes into PSRAM,
// ctx's rasteriser); this is the read side, DMA case, a materially
// different access pattern. Skips itself if PSRAM isn't available.
void psram_dma_bench_run(void);

#endif
