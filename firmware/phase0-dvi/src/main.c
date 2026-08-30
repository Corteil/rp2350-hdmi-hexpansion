// Phase 0 A1/A3 bring-up (Stage 1): basic HSTX DVI output on the Feather
// RP2350 -- see ../../README.md section 8, Phase 0 Part A. Proves the
// fundamental TMDS/HSTX/DMA/adapter/monitor chain works before tackling
// the real A1 target (240x240 source, pillarboxed circular-masked
// scale-during-scanout into 640x480, per section 3.4).
//
// This is deliberately NOT that yet. Simplifications from the real
// design, to reduce what could go wrong on the first attempt:
//   - 320x240 source doubled to fill 640x480 exactly -- no pillarbox, no
//     circular mask needed, since 320*2=640 and 240*2=480 exactly.
//   - Horizontal 2x is done by pre-expanding into 640-wide rows in SRAM
//     at init, not via HSTX/DMA's pixel-duplication register trick --
//     sidesteps a fiddly bit-packing detail for this first bring-up.
//   - Uses the simpler two-DMA-channel ping-pong + per-scanline IRQ
//     mechanism from the official pico-examples hstx/dvi_out_hstx_encoder
//     (github.com/raspberrypi/pico-examples), not the more CPU-efficient
//     one-IRQ-per-frame ring-buffer mechanism Adafruit's own PicoDVI
//     driver (adafruit/circuitpython, Framebuffer_RP2350.c, MIT) uses --
//     that's a real thing to adopt once this simpler version is
//     confirmed working on hardware, since core-load is what A1 actually
//     needs to measure, just not yet.
//
// RGB565 TMDS expand config and the RGB565+BSWAP-during-DMA idea are
// taken from Adafruit's driver (same silicon, proven on real HSTX+DVI
// hardware) -- see the comment above vactive_line's expand_tmds setup.
// Video timing (640x480@60, V_BACK_PORCH=33) is the official/spec-correct
// CEA-861 VIC 1 numbers from pico-examples and this design's own README
// section 3.3, not Adafruit's driver (whose MODE_640 constants use a
// non-standard V_BACK_PORCH=133 for reasons unclear -- not trusted here).
//
// Feather-specific: GPIO12/13 = TMDS lane 2, GPIO14/15 = clock, GPIO16/17
// = lane 1, GPIO18/19 = lane 0, per Adafruit's own pinout page
// (learn.adafruit.com/adafruit-feather-rp2350/pinouts) -- different from
// the Pico DVI Sock pinout pico-examples' own comment describes.

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
// 320 words/row * 240 rows * 4 bytes = 307200 bytes.

#define SRC_ROWS 240
#define ROW_WORDS (MODE_H_ACTIVE_PIXELS / 2)  // 2 RGB565 pixels/word

static uint32_t framebuf[SRC_ROWS][ROW_WORDS];

// Classic 8-bar test pattern: white/yellow/cyan/green/magenta/red/blue/black,
// 80px each (640/8). Lets a glance confirm both geometry (bars should be
// sharp and evenly spaced) and colour channel wiring (each bar is a known
// colour) at once.
static const uint16_t bar_colours[8] = {
    0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
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
// while the other is already running. One IRQ per scanline (not per
// frame) -- simple and known-correct, but not what the real design's CPU
// budget assumes; see the file header.

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
    // clk_hstx follows clk_sys undivided by default (see the CSR comment
    // in the HSTX setup below), and the real pixel clock is fixed at
    // clk_hstx/5 by our N_SHIFTS=5 DDR scheme. 640x480@60 needs a 25.2 MHz
    // pixel clock, i.e. clk_sys = 126 MHz -- confirmed against
    // Panda381/DispHSTX's own tested video mode table (vmodetime_640x480,
    // Unlicense), which explicitly uses "system clock 126 MHz" for this
    // exact mode. The stock default (150 MHz) was the root cause of the
    // "out of range" result on first bring-up.
    set_sys_clock_khz(126000, true);

    // Heartbeat/diagnostic LED (GPIO7, board default) -- this firmware has
    // no USB serial and video-or-nothing is a useless signal for telling
    // *where* a problem is. Blink 3x fast now (proves boot + GPIO work at
    // all), then the main loop blinks continuously at ~1Hz once HSTX/DMA
    // setup below has completed -- so "no blinking at all" means it never
    // got here, "3 blinks then stuck" means it hung during HSTX/DMA setup,
    // and "continuous blinking, still no picture" means the code runs fine
    // and the bug is specifically in the video config/timing/wiring.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 3; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(150);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(150);
    }

    fill_test_pattern();

    // Configure HSTX's TMDS encoder for RGB565. NBITS fields are
    // encoded as (true bit count - 1) -- 4/5/4 here means 5/6/5 bits,
    // matching RGB565's actual layout. Values from Adafruit's PicoDVI
    // driver (MIT, adafruit/circuitpython) -- see file header.
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        27 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        21 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels (TMDS) come in 2 16-bit pixels per 32-bit word for RGB565.
    // Control symbols (RAW) are an entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config. N_SHIFTS=5, SHIFT=2 means 2 bits/pin shifted
    // out per HSTX clock cycle (DDR), 5 cycles per refill -- one 10-bit
    // TMDS symbol per lane per refill. CSR.CLKDIV is NOT a pixel-clock
    // divisor (an earlier attempt here assumed it was and made things
    // worse -- see git history and README): per the RP2350 datasheet
    // section 12.11.4, CLKDIV only sets the *clock generator*'s period
    // (the dedicated CK+/CK- output), and "generally the clock period
    // will be a divisor of CSR.N_SHIFTS so that clock and data maintain
    // a consistent alignment" -- for this N_SHIFTS=5 scheme, CLKDIV must
    // equal 5, full stop. The datasheet's own worked TMDS example uses
    // exactly N_SHIFTS=5, CLKDIV=5. Changing CLKDIV alone desyncs clock
    // from data and breaks TMDS decoding entirely (confirmed on
    // hardware: went from monitor-detected-but-wrong-mode to
    // completely undetected).
    //
    // The actual pixel clock is clk_hstx_freq / 5 (fixed by this DDR/
    // N_SHIFTS scheme), so getting 25.2 MHz needs clk_hstx itself
    // reconfigured to 126 MHz -- see clk_hstx setup before this function
    // is called, or main README section 3.4's own note that clk_hstx
    // needs to run independently of clk_sys for this reason.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Clock pair always on GPIO14/15 (silicon-fixed: HSTX bit index n is
    // always GPIO 12+n, so bit[2]/bit[3] is always the 14/15 pair).
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    // Feather pinout: lane 0 -> GPIO18/19, lane 1 -> GPIO16/17,
    // lane 2 -> GPIO12/13 (see file header).
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

    // Continuous ~1Hz heartbeat: reaching here means HSTX/DMA setup above
    // completed without hanging. Plain sleep_ms() rather than __wfi(), so
    // the blink rate is a clean ~1Hz regardless of how often the scanline
    // IRQ wakes this core.
    bool led_on = false;
    while (1) {
        led_on = !led_on;
        gpio_put(PICO_DEFAULT_LED_PIN, led_on);
        sleep_ms(500);
    }
}
