// B1: ctx rasterisation benchmark at 640x480, RGB565_BYTESWAPPED -- the
// badge's own framebuffer format (see ../../README.md section 1.5 and
// section 8, "Phase 0 - Part B, B1"). Renders into a framebuffer in
// PSRAM (614 KB doesn't fit in the RP2350's 520 KB of SRAM), timing two
// synthetic scenes:
//   - "typical": a handful of cards, icons and a header gradient, roughly
//     the complexity of a simple Tildagon app screen.
//   - "worst case": ~160 overlapping translucent shapes plus a
//     full-screen radial gradient, to stress the alpha-compositing and
//     AA paths hard.
//
// These are hand-built scenes, not a drawlist captured from a real badge
// app -- that needs the badge plus the display.get_fb() upstream patch
// (Phase 0 Part C, not done yet). See third_party/ctx/VENDORED.md for
// what "representative" means here and its limits.

#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "hardware/flash.h"

// --- ctx configuration (must precede the #include) ---
// Slimmed for a microcontroller target: no text/fonts (none baked in
// here), no XML/parser/formatter/events (desktop-oriented features), and
// only the one pixel format this benchmark actually uses.
#define CTX_IMPLEMENTATION
// Match the badge firmware's own ctx_config.h (see ../../README.md
// section 1.5) rather than ctx.h's own default of 15 -- otherwise this
// benchmark measures 3x the scanline supersampling the real hardware
// actually runs, and the timing numbers aren't comparable to anything.
#define CTX_RASTERIZER_AA              5
#define CTX_BACKEND_TEXT               0
#define CTX_XML                        0
#define CTX_PARSER                     0
#define CTX_FORMATTER                  0
#define CTX_EVENTS                     0
#define CTX_ENABLE_CM                  0
#define CTX_SHAPE_CACHE                0
#define CTX_LIMIT_FORMATS              1
#define CTX_ENABLE_RGB565_BYTESWAPPED  1
#include "ctx.h"

#include "ctx_bench.h"

#define BENCH_W 640
#define BENCH_H 480
#define BENCH_STRIDE (BENCH_W * 2)
#define BENCH_FB_BYTES ((size_t)BENCH_STRIDE * BENCH_H)
#define BENCH_ITERATIONS 10
#define TAU 6.2831853f

static uint32_t bench_rand(uint32_t *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return *seed >> 8;
}

// Cheapest possible ctx draw call: one opaque, axis-aligned, full-canvas
// rectangle. No gradient evaluation, no partial-coverage AA edge pixels
// beyond the four canvas edges. Used to isolate "cost of one full-canvas
// pass through ctx's rasteriser" from the gradient/shape-heavy scenes
// above, since dropping AA level 15->5 barely moved their timings and
// that needed explaining rather than guessing at.
static void draw_solid_fill_scene(Ctx *ctx) {
    ctx_rectangle(ctx, 0, 0, BENCH_W, BENCH_H);
    ctx_set_rgba(ctx, 0.2f, 0.2f, 0.2f, 1.0f);
    ctx_fill(ctx);
}

static void draw_typical_scene(Ctx *ctx) {
    ctx_rectangle(ctx, 0, 0, BENCH_W, BENCH_H);
    ctx_set_rgba(ctx, 0.08f, 0.08f, 0.10f, 1.0f);
    ctx_fill(ctx);

    ctx_rectangle(ctx, 0, 0, BENCH_W, 60);
    ctx_linear_gradient(ctx, 0, 0, BENCH_W, 0);
    ctx_gradient_add_stop(ctx, 0.0f, 0.10f, 0.20f, 0.45f, 1.0f);
    ctx_gradient_add_stop(ctx, 1.0f, 0.25f, 0.10f, 0.40f, 1.0f);
    ctx_fill(ctx);

    for (int i = 0; i < 6; i++) {
        float x = 20 + (i % 3) * 200;
        float y = 90 + (i / 3) * 160;

        ctx_round_rectangle(ctx, x, y, 180, 130, 14);
        ctx_set_rgba(ctx, 0.15f, 0.16f, 0.20f, 1.0f);
        ctx_fill(ctx);

        ctx_arc(ctx, x + 30, y + 30, 16, 0.0f, TAU, 0);
        ctx_set_rgba(ctx, 0.95f, 0.55f, 0.15f, 1.0f);
        ctx_fill(ctx);
    }

    ctx_set_rgba(ctx, 0.3f, 0.3f, 0.35f, 1.0f);
    ctx_set_line_width(ctx, 2.0f);
    for (int i = 1; i < 3; i++) {
        float y = 90 + i * 160 - 15;
        ctx_move_to(ctx, 0, y);
        ctx_line_to(ctx, BENCH_W, y);
        ctx_stroke(ctx);
    }
}

static void draw_worst_case_scene(Ctx *ctx) {
    ctx_rectangle(ctx, 0, 0, BENCH_W, BENCH_H);
    ctx_radial_gradient(ctx, BENCH_W / 2.0f, BENCH_H / 2.0f, 0,
                         BENCH_W / 2.0f, BENCH_H / 2.0f, BENCH_W / 2.0f);
    ctx_gradient_add_stop(ctx, 0.0f, 0.9f, 0.3f, 0.5f, 1.0f);
    ctx_gradient_add_stop(ctx, 1.0f, 0.05f, 0.05f, 0.15f, 1.0f);
    ctx_fill(ctx);

    uint32_t seed = 12345;
    for (int i = 0; i < 120; i++) {
        float x = (float)(bench_rand(&seed) % BENCH_W);
        float y = (float)(bench_rand(&seed) % BENCH_H);
        float r = (float)(10 + bench_rand(&seed) % 40);
        ctx_arc(ctx, x, y, r, 0.0f, TAU, 0);
        ctx_set_rgba(ctx, 0.9f, 0.9f, 1.0f, 0.15f);
        ctx_fill(ctx);
    }

    seed = 999;
    for (int i = 0; i < 40; i++) {
        float x = (float)(bench_rand(&seed) % BENCH_W);
        float y = (float)(bench_rand(&seed) % BENCH_H);
        ctx_round_rectangle(ctx, x, y, 80, 50, 10);
        ctx_set_rgba(ctx, 0.1f, 0.1f, 0.1f, 0.25f);
        ctx_fill(ctx);
    }
}

typedef void (*scene_fn)(Ctx *ctx);

static void run_timed(const char *name, Ctx *ctx, scene_fn scene) {
    int64_t total_us = 0, min_us = -1, max_us = 0;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        absolute_time_t start = get_absolute_time();
        scene(ctx);
        int64_t us = absolute_time_diff_us(start, get_absolute_time());

        total_us += us;
        if (min_us < 0 || us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }

    double avg_us = (double)total_us / BENCH_ITERATIONS;
    printf("  %-11s min %7.2f ms  avg %7.2f ms (%5.1f fps)  max %7.2f ms\n",
           name, min_us / 1000.0, avg_us / 1000.0, 1e6 / avg_us, max_us / 1000.0);
}

// Raw sequential scalar write to the same PSRAM region, same byte count
// as one full-canvas pass, as an upper-bound reference: if ctx's own
// solid-fill time is close to this, PSRAM write bandwidth is what's
// dominating scene time, not ctx's rasteriser. volatile so the compiler
// can't fold this into something that doesn't reflect real per-word
// store cost (same reasoning as the PSRAM self-test in main.c).
static void bench_raw_psram_write(volatile uint16_t *fb) {
    size_t pixels = (size_t)BENCH_W * BENCH_H;
    int64_t total_us = 0, min_us = -1, max_us = 0;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        absolute_time_t start = get_absolute_time();
        for (size_t p = 0; p < pixels; p++) {
            fb[p] = (uint16_t)(0x2104u + i);
        }
        int64_t us = absolute_time_diff_us(start, get_absolute_time());

        total_us += us;
        if (min_us < 0 || us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }

    double avg_us = (double)total_us / BENCH_ITERATIONS;
    printf("  %-11s min %7.2f ms  avg %7.2f ms (%5.1f fps)  max %7.2f ms"
           "  [~%.2f MB/s]\n",
           "raw write", min_us / 1000.0, avg_us / 1000.0, 1e6 / avg_us,
           max_us / 1000.0, (double)BENCH_FB_BYTES / (avg_us / 1e6) / (1024 * 1024));
}

void ctx_bench_run(void) {
    printf("\nphase0-metro: B1 ctx rasterisation benchmark\n");

    if (!psram_is_available() || psram_get_size() < BENCH_FB_BYTES) {
        printf("  skipped: need >= %u bytes of PSRAM for a %dx%d "
               "framebuffer, have %u\n",
               (unsigned)BENCH_FB_BYTES, BENCH_W, BENCH_H,
               (unsigned)psram_get_size());
        return;
    }

    void *fb = (void *)(XIP_BASE + flash_devinfo_size_to_bytes(FLASH_DEVINFO_SIZE_MAX));

    Ctx *ctx = ctx_new_for_framebuffer(fb, BENCH_W, BENCH_H, BENCH_STRIDE,
                                        CTX_FORMAT_RGB565_BYTESWAPPED);

    printf("  %dx%d RGB565_BYTESWAPPED, %d iterations per scene\n",
           BENCH_W, BENCH_H, BENCH_ITERATIONS);
    bench_raw_psram_write((volatile uint16_t *)fb);
    run_timed("solid fill", ctx, draw_solid_fill_scene);
    run_timed("typical", ctx, draw_typical_scene);
    run_timed("worst case", ctx, draw_worst_case_scene);

    ctx_free(ctx);
}
