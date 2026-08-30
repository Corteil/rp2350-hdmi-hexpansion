// Phase 0 A1 colour-channel-rotation root cause fix, on the Metro RP2350.
// See ../../README.md section 8's A1 "Still open" note and
// ../phase0-dvi/README.md's Results section for the original finding.
//
// phase0-dvi (Feather) found a clean 3-way colour rotation on real
// hardware -- displayed_R = sent_G, displayed_G = sent_B, displayed_B =
// sent_R -- and worked around it by empirically inverting the test
// pattern's own colour table (a test-pattern-level fix, not usable for
// real badge pixel data). This firmware is the actual root-cause fix,
// derived from first principles rather than guessed:
//
// 1. RP2350's own HSTX_CTRL_EXPAND_TMDS register fields (per pico-sdk's
//    hardware/regs/hstx_ctrl.h, straight from the SVD): each lane's ROT
//    is a RIGHT-ROTATE applied to the current 32-bit shifter word before
//    that lane's encoder; NBITS is the valid-bit count "starting from
//    bit 7 of the rotated data" (field value N encodes N+1 bits, low
//    bits below the window masked to 0). So a lane's colour-channel
//    field, wherever it sits in the source word, must be rotated until
//    its OWN MSB lands exactly on bit 7 of the rotated result.
// 2. Validated this formula against a known-good reference before
//    trusting it: raspberrypi/pico-examples' own hstx/dvi_out_hstx_encoder
//    RGB332 sample (L0 NBITS=1/ROT=26, L1 NBITS=2/ROT=29, L2 NBITS=2/
//    ROT=0) decodes EXACTLY to L0=Blue, L1=Green, L2=Red under this
//    formula, matching RGB332's known bit layout -- confirms both the
//    formula and that L0=Blue/L1=Green/L2=Red is the real, standard
//    lane order (matching the DVI/HDMI spec's own TMDS-channel-to-colour
//    convention: Channel 0 = Blue, Channel 1 = Green, Channel 2 = Red --
//    NOT alphabetical, a common trap).
// 3. phase0-dvi's ROT/NBITS values were copied from Adafruit's PicoDVI
//    driver (adafruit/circuitpython, MIT) under the ASSUMPTION L0=Red,
//    L1=Green, L2=Blue -- backwards on lane order alone (per point 2),
//    and (checked against this project's own RGB565 packing convention:
//    two pixels/word, earlier pixel in the low 16 bits, standard
//    R[15:11]/G[10:5]/B[4:0] layout) Adafruit's specific ROT numbers
//    don't even land on a clean single-channel window under the
//    verified formula -- they must assume a different internal packing
//    convention than this project's framebuf uses. Recomputed from
//    scratch for THIS project's actual layout instead of re-guessing:
//
//      channel gets ROT = (channel_MSB_bit_index - 7) mod 32
//
//    R: MSB at bit 15 (5 bits)  -> ROT = 8,  NBITS field = 4 (5 bits)
//    G: MSB at bit 10 (6 bits)  -> ROT = 3,  NBITS field = 5 (6 bits)
//    B: MSB at bit 4  (5 bits)  -> ROT = 29, NBITS field = 4 (5 bits)
//
//    With L0=Blue, L1=Green, L2=Red (point 2), and expand_shift's
//    ENC_SHIFT=16 right-rotating the whole word by 16 between the two
//    packed pixels -- which brings the second pixel down into the same
//    low-16-bit position the first pixel started in, so these same
//    fixed ROT values correctly decode both pixels of each 32-bit word,
//    the same way pico-examples' single fixed set works across all 4
//    sub-pixels of its RGB332 case.
//
// Everything else (DMA ping-pong, 640x480@60 timing, 320x240->640x480
// exact-double geometry) is unchanged from phase0-dvi and already
// hardware-confirmed correct there -- this firmware isolates the colour
// fix as the only variable. Test pattern below uses the TRUE, intended
// RGB565 values (no compensation table) -- if the fix is right, colours
// should come out correctly ordered directly.
//
// Board: Metro RP2350 (RP2350B), not the Feather -- chosen because it
// was the board available to test with. HSTX is silicon-fixed to
// GPIO12-19 regardless of RP2350A/B package (bit index n = GPIO 12+n
// always), and confirmed from Adafruit's own Metro RP2350 pinout page
// that its dedicated 22-pin HSTX connector breaks out exactly
// GPIO12-19 as D0P/D0N/D1P/D1N/D2P/D2N/CKP/CKN -- the identical mapping
// phase0-dvi already uses on the Feather. The main README's Phase 0
// section previously claimed the Metro has "no HSTX peripheral broken
// out" -- that was wrong; corrected alongside this firmware.

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

// ----------------------------------------------------------------------------
// DVI constants (standard CEA-861 VIC 1, 640x480@60 -- see main README section 3.3)

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

#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH  + MODE_H_ACTIVE_PIXELS \
)
#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Source image: 320x240, pre-expanded to 640-wide RGB565 rows in SRAM.
// Vertical 2x is done by re-reading each row twice (see dma_irq_handler).

#define SRC_ROWS 240
#define ROW_WORDS (MODE_H_ACTIVE_PIXELS / 2)  // 2 RGB565 pixels/word

static uint32_t framebuf[SRC_ROWS][ROW_WORDS];

// Classic 8-bar test pattern, TRUE/intended RGB565 values -- NOT a
// compensation table. If the expand_tmds fix above is correct, these
// should display in this exact order with no further massaging.
static const uint16_t bar_colours[8] = {
    0xFFFF, // white   R+G+B
    0xFFE0, // yellow  R+G
    0x07FF, // cyan    G+B
    0x07E0, // green   G
    0xF81F, // magenta R+B
    0xF800, // red     R
    0x001F, // blue    B
    0x0000, // black
};

static void fill_test_pattern(void) {
    for (int row = 0; row < SRC_ROWS; row++) {
        for (int w = 0; w < ROW_WORDS; w++) {
            int x0 = w * 2;
            int x1 = w * 2 + 1;
            uint16_t c0 = bar_colours[x0 / 80];
            uint16_t c1 = bar_colours[x1 / 80];
            framebuf[row][w] = (uint32_t)c0 | ((uint32_t)c1 << 16);
        }
    }
}

// ----------------------------------------------------------------------------
// HSTX command lists (padded with NOPs to be >= HSTX FIFO size)

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

// ----------------------------------------------------------------------------
// DMA logic -- two channels ping-ponging via chain_to; whichever one just
// finished gets reconfigured (by this IRQ handler) for the next scanline
// while the other is already running. Unchanged from phase0-dvi.

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

void __scratch_x("") dma_irq_handler(void) {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        uint v_active = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
        uint src_row = (v_active / 2) % SRC_ROWS;  // vertical 2x: re-read each row twice
        ch->read_addr = (uintptr_t)framebuf[src_row];
        ch->transfer_count = ROW_WORDS;
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

// ----------------------------------------------------------------------------

int main(void) {
    // clk_hstx follows clk_sys undivided by default; real pixel clock is
    // clk_hstx/5 (N_SHIFTS=5 DDR scheme below). 640x480@60 needs 25.2 MHz
    // pixel clock -> clk_sys = 126 MHz. See phase0-dvi/README.md for the
    // full derivation (already confirmed on hardware there).
    set_sys_clock_khz(126000, true);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 3; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(150);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(150);
    }

    fill_test_pattern();

    // Corrected TMDS lane config for RGB565 -- see file header for the
    // full derivation. L0=Blue, L1=Green, L2=Red (the real DVI/HDMI lane
    // order, not the L0=Red assumption phase0-dvi copied from Adafruit).
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |  // Red:   5 bits
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |  // Green: 6 bits
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |  // Blue:  5 bits
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels (TMDS) come in 2 16-bit pixels per 32-bit word for RGB565.
    // Control symbols (RAW) are an entire 32-bit word. Unchanged from
    // phase0-dvi -- this part was never in question.
    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // N_SHIFTS=5, SHIFT=2, CLKDIV=5 -- see phase0-dvi/README.md and its
    // src/main.c for the full derivation (CLKDIV is NOT a pixel-clock
    // divisor; it must equal N_SHIFTS, full stop). Unchanged.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Clock pair always on GPIO14/15 (silicon-fixed: HSTX bit index n is
    // always GPIO 12+n). Metro's HSTX port: CKP/CKN = GPIO14/15,
    // D0P/D0N = GPIO18/19, D1P/D1N = GPIO16/17, D2P/D2N = GPIO12/13 --
    // per Adafruit's own Metro RP2350 pinout page, identical to the
    // Feather's mapping since it's silicon-fixed either way.
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    // Lane 0 -> GPIO18/19 (D0), lane 1 -> GPIO16/17 (D1), lane 2 ->
    // GPIO12/13 (D2) -- same physical GPIO/lane wiring as phase0-dvi;
    // only which colour each lane CARRIES changed (expand_tmds above),
    // not which GPIO pair each lane comes out on.
    static const int lane_to_output_bit[3] = {6, 4, 0};
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

    bool led_on = false;
    while (1) {
        led_on = !led_on;
        gpio_put(PICO_DEFAULT_LED_PIN, led_on);
        sleep_ms(500);
    }
}
