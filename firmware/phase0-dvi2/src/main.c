// Phase 0 A1 bring-up (Stage 2): the real A1 target -- 240x240 source,
// pillarboxed and circular-masked, scaled 2x into 640x480 -- see
// ../../README.md sections 3.1/3.4 and section 8's A1 description.
//
// Builds on firmware/phase0-dvi (Stage 1, confirmed working on hardware:
// see its README for the three real bugs found there -- flipped cable,
// wrong pixel clock, rotated colour channels. All three fixes/facts
// carry over unchanged here: clk_sys=126MHz, the Feather pin/lane
// mapping, and the empirically-corrected bar_colours[] table).
//
// New in this stage:
//   - 240x240 source (not 320x240), pre-doubled horizontally in software
//     into 480-wide rows in SRAM (same technique as Stage 1) -- vertical
//     2x is still done by re-reading each source row twice.
//   - Pillarbox: the 480-wide doubled content sits centered in the
//     640-wide active line, with 80px of black on each side
//     (640-480=160, /2=80) -- always present, at every row.
//   - Circular mask: computed ONCE at init (see build_row_table below),
//     one half-width value per output row (0..479), from the circle
//     equation centred on the 480x480 square. Rows near the vertical
//     centre show close to the full 480px; rows near the top/bottom
//     show progressively less, down to nothing outside the circle's
//     radius -- matching what the badge's own round display would show.
//     By symmetry the extra black this adds on each side of the visible
//     content is always equal (see build_row_table's comment).
//   - Per active row, the command list now has four DMA phases instead
//     of Stage 1's two (command list, left black fill, real pixel data,
//     right black fill) -- but critically only ONE HSTX command word
//     total per row (vactive_line's single TMDS|640), matching Stage 1
//     exactly; see the state machine in dma_irq_handler and vactive_line's
//     comment for why that distinction matters. An earlier version of
//     this file used three separate TMDS/TMDS_REPEAT *commands* per row
//     (not just DMA phases) and got "no signal" on real hardware --
//     likely from the extra command-boundary stalls the RP2350 datasheet
//     describes (section 12.11.5), breaking the constant per-line
//     duration a monitor needs to lock horizontal sync to. Reverted to
//     this single-command structure, confirmed structurally identical
//     to Stage 1's proven design. Completing a row now takes 4 IRQ calls
//     instead of Stage 1's 2 -- more IRQ overhead than Stage 1, on top of
//     Stage 1's own "simpler than the real design's ring-buffer
//     mechanism" starting point -- the CPU load percentage measured and
//     printed at boot should be read with that in mind, as an upper
//     bound rather than what a more optimised implementation would show.
//   - CPU load measurement: a tight busy-loop is timed for a fixed
//     window before HSTX/DMA starts (baseline) and again after (loaded).
//     The difference is printed over USB CDC. Adding USB CDC here (Stage
//     1 had none) also adds its own interrupt activity, which will
//     inflate the "loaded" number slightly versus a build with no USB
//     console at all -- a real caveat, disclosed rather than hidden.

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>

// ----------------------------------------------------------------------------
// DVI constants (standard CEA-861 VIC 1, 640x480@60 -- see README section 3.3)

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Source image: 240x240, pre-doubled horizontally to 480-wide RGB565 rows
// in SRAM (same technique as Stage 1 -- see its README for why). Vertical
// 2x re-reads each row twice. 240 words/row * 240 rows * 4 bytes = 230400
// bytes.

#define SRC_ROWS   240
#define DBL_W      480                 // 240 source pixels, doubled
#define DBL_WORDS  (DBL_W / 2)         // 2 RGB565 pixels/word
#define PILLARBOX  ((MODE_H_ACTIVE_PIXELS - DBL_W) / 2)  // 80px each side

static uint32_t framebuf[SRC_ROWS][DBL_WORDS];

// Same 8-bar pattern and empirically-corrected colour order as Stage 1
// (see phase0-dvi/README.md for how these values were derived) -- now
// 60px/bar (480/8) instead of 80px/bar (640/8).
static const uint16_t bar_colours[8] = {
    0xFFFF, 0x07FF, 0xF81F, 0x001F, 0xFFE0, 0x07E0, 0xF800, 0x0000,
};

static void fill_test_pattern(void) {
    for (int row = 0; row < SRC_ROWS; row++) {
        for (int w = 0; w < DBL_WORDS; w++) {
            int x0 = w * 2;
            int x1 = w * 2 + 1;
            uint16_t c0 = bar_colours[x0 / 60];
            uint16_t c1 = bar_colours[x1 / 60];
            framebuf[row][w] = (uint32_t)c0 | ((uint32_t)c1 << 16);
        }
    }
}

// Per active-output-row (0..479) half-width of the visible circle, in
// pixels, always even (word-aligned: 2 RGB565 pixels/word). Built once at
// init -- this is the "per-line descriptor table for the circular mask
// ... built once at mode-set" main README section 3.4 describes.
//
// Circle of diameter 480 (matching the doubled content), centred on the
// 480x480 square. left_black and right_black (pillarbox + mask) are
// always equal by construction -- both are PILLARBOX + (240 - half_width),
// since the circle is centred: whatever width is cut from the visible
// span is split evenly left and right of centre.
static uint16_t row_half_width[MODE_V_ACTIVE_LINES];

static void build_row_table(void) {
    const float radius = DBL_W / 2.0f;       // 240
    const float centre = (MODE_V_ACTIVE_LINES - 1) / 2.0f;  // 239.5
    for (int row = 0; row < MODE_V_ACTIVE_LINES; row++) {
        float dy = row - centre;
        float hw = 0.0f;
        if (fabsf(dy) < radius) {
            hw = sqrtf(radius * radius - dy * dy);
        }
        int half_width = (int)(hw + 0.5f);
        half_width -= half_width & 1;  // round down to even, for word alignment
        if (half_width < 0) half_width = 0;
        if (half_width > (int)radius) half_width = (int)radius;
        row_half_width[row] = (uint16_t)half_width;
    }
}

// ----------------------------------------------------------------------------
// HSTX command lists

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

// vactive_line: byte-for-byte Stage 1's proven command list -- ONE
// HSTX_CMD_TMDS command covering the full 640-pixel active width, same
// as Stage 1's own vactive_line. Deliberately NOT extended with extra
// TMDS_REPEAT commands for the black fill (an earlier version of this
// file did that, and got "no signal" on real hardware): per RP2350
// datasheet section 12.11.5, "the command expander cannot output data on
// the cycle where it pops a command from the FIFO, so the expansion
// shift register is empty for at least one cycle" -- every extra command
// boundary costs a stall Stage 1's proven timing doesn't have, breaking
// the constant per-line duration a monitor needs to lock horizontal sync
// to. Instead, the black fill for pillarbox/mask is sourced as plain
// DATA (from zero_buf below), under this SAME single TMDS command's
// declared 640-pixel/320-word consumption -- no extra commands, so no
// extra stalls, exactly matching Stage 1's structure.
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS
};

// All-zero source for the black pillarbox/mask fill -- DMA reads
// (auto-incrementing) whatever number of words it needs from here; since
// every word is zero regardless of offset, no explicit length tracking
// or wraparound is needed, just "don't ask for more than
// ZERO_BUF_WORDS". Sized for the worst case: max black run in one row is
// PILLARBOX + DBL_W/2 = 80+240 = 320 pixels = 160 words (right at the
// edge of the visible circle, one pixel-row before full mask).
//
// Deliberately NOT `const` (which would place it in flash/XIP) --
// vactive_line and framebuf are both plain SRAM, and reading DMA source
// data for the timing-critical active-scanout path from two different
// memory technologies (SRAM with deterministic access vs. flash behind
// an XIP cache) is an avoidable variable in a mechanism that's already
// proven finicky about timing. Costs nothing (640 bytes) to keep
// everything in SRAM uniformly.
#define ZERO_BUF_WORDS 160
static uint32_t zero_buf[ZERO_BUF_WORDS] = {0};

// ----------------------------------------------------------------------------
// DMA logic -- two channels ping-ponging via chain_to, same as Stage 1.
// Active rows now take 4 phases (command list / left fill / pixel data /
// right fill) instead of Stage 1's 2 (command list / pixel data) -- but
// critically only ONE HSTX command word total per active row, same as
// Stage 1 -- see vactive_line's comment above for why that distinction
// matters. Phases 1/3 (black fill) are pure data, no commands.

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static int active_phase = 0;  // 0=left fill, 1=pixel data, 2=right fill

void __scratch_x("") dma_irq_handler(void) {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    uint buf_idx = dma_pong ? 1 : 0;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
        return;
    }
    if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
        return;
    }

    // Active video. Four phases per row, but only phase 0 carries any
    // HSTX command word (vactive_line's single TMDS|640) -- phases 1-3
    // are pure data, sourced from up to three different memory regions,
    // all consumed under that one command's declared 320-word budget.
    // This mirrors Stage 1's structure exactly (one command, one
    // contiguous logical pixel run) -- see the comments on vactive_line
    // and zero_buf above for why that match matters.
    //
    // left_words/right_words are never zero (PILLARBOX=80 alone is
    // already 40 words), so phases 0/1/3 always run every active row;
    // only phase 2 (real pixel data) can be zero-length, on fully-masked
    // rows near the very top/bottom of the circle -- a zero-length DMA
    // transfer is harmless (completes immediately, fires its IRQ, moves
    // straight to phase 3), so no special-casing is needed for that.
    uint v_active = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    uint half_width = row_half_width[v_active];      // pixels; visible span is 2*half_width
    uint left_black = PILLARBOX + (DBL_W / 2 - half_width);
    uint right_black = left_black;                   // always equal -- see row_half_width comment

    if (active_phase == 0) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        active_phase = 1;
    } else if (active_phase == 1) {
        ch->read_addr = (uintptr_t)zero_buf;
        ch->transfer_count = left_black / 2;
        active_phase = 2;
    } else if (active_phase == 2) {
        uint src_row = (v_active / 2) % SRC_ROWS;  // vertical 2x
        uint col_word_offset = (DBL_W / 2 - half_width) / 2;
        ch->read_addr = (uintptr_t)(&framebuf[src_row][col_word_offset]);
        ch->transfer_count = half_width;  // 2*half_width pixels / 2 px-per-word = half_width words
        active_phase = 3;
    } else {
        ch->read_addr = (uintptr_t)zero_buf;
        ch->transfer_count = right_black / 2;
        active_phase = 0;
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

// ----------------------------------------------------------------------------
// CPU load measurement: a tight busy-loop, timed for a fixed window
// before HSTX/DMA starts (baseline) and again after (loaded). The
// difference is the DVI IRQ's share of one core. See file header for
// caveats (this stage's per-scanline-IRQ mechanism is not the CPU-
// efficient design the real product would use; USB CDC's own interrupt
// activity adds a bit of noise too).

static volatile uint32_t busy_counter;

static uint32_t run_busy_loop(uint32_t window_ms) {
    busy_counter = 0;
    absolute_time_t end = make_timeout_time_ms(window_ms);
    while (!time_reached(end)) {
        busy_counter++;
    }
    return busy_counter;
}

// ----------------------------------------------------------------------------

int main(void) {
    set_sys_clock_khz(126000, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(100);
    }

    printf("\nphase0-dvi2: A1 Stage 2 -- 240x240 pillarboxed, circular-masked\n");

    build_row_table();
    fill_test_pattern();

    uint32_t baseline = run_busy_loop(200);
    printf("  CPU load baseline (DVI not yet running): %lu busy-loop iterations / 200ms\n",
           (unsigned long)baseline);

    // Configure HSTX's TMDS encoder for RGB565 -- unchanged from Stage 1,
    // see its README/file header for where these values came from.
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        27 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        21 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // CLKDIV=5=N_SHIFTS (not a pixel-clock divisor -- see Stage 1's
    // README for the datasheet-sourced explanation of why).
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    static const int lane_to_output_bit[3] = {6, 4, 0};  // Feather pinout, see Stage 1
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = lane_to_output_bit[lane];
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0);  // HSTX
    }

    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PING, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false
    );
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PONG, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false
    );

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);

    sleep_ms(100);  // let a few frames run before measuring
    uint32_t loaded = run_busy_loop(200);
    double load_pct = 100.0 * (1.0 - (double)loaded / (double)baseline);
    printf("  CPU load with DVI running: %lu busy-loop iterations / 200ms\n", (unsigned long)loaded);
    printf("  -> approx %.1f%% of one core spent servicing the DVI scanout IRQ\n", load_pct);
    printf("  (design's estimate for the real, more efficient mechanism: 2-5%%; this\n");
    printf("   stage uses a 4-DMA-phase-per-row IRQ scheme, and USB CDC adds its\n");
    printf("   own overhead -- read this as an upper bound, not the final number)\n");

    bool led_on = false;
    while (1) {
        led_on = !led_on;
        gpio_put(PICO_DEFAULT_LED_PIN, led_on);
        sleep_ms(500);
    }
}
