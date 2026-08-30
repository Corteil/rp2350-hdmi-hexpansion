// Hexi-GFX end-to-end link test (rp2350-hdmi-hexpansion, VID=0x1969
// PID=0x4544): the first firmware combining every Phase 0 piece into
// one image and one demonstration -- see ../../../README.md section 8.
//
// Badge plugs in -> discovers the emulated EEPROM (I2C0 target,
// eeprom_i2c.c, real VID/PID) -> mounts the real LittleFS image inside
// it (fs_image.h) -> auto-launches badge_app/app.py -> that app opens
// SPI0 (mode 3, CS-continuous -- the exact protocol proven in Phase 0
// A2/C3) and sends a pattern-select command -> this firmware applies it
// and the connected monitor (HSTX/DVI, colour-correct per Phase 0 A1)
// shows it. Every link in that chain was proven individually in Phase 0;
// this is the first firmware to run all of them at once.
//
// Board: Metro RP2350 -- same bench rig as A2/C3/C4 (I2C0 on GPIO4/5,
// SPI0 on GPIO20-23) and phase0-dvi-colorfix (HSTX on GPIO12-19, its own
// dedicated 22-pin connector). All three pin groups are disjoint, so one
// physical board runs the whole demo.
//
// HSTX geometry: Stage 1's exact-double 320x240->640x480 shape (no
// pillarbox/circular mask -- that's A1 Stage 2's concern, mirroring the
// badge's own round display; this is a generic test card, not a mirror,
// so the simpler exact-fit geometry is the right one here). Colour
// config is the root-cause fix from firmware/phase0-dvi-colorfix/, not
// the empirical compensation table Stage 1 originally shipped with.

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "eeprom_i2c.h"

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
// Source image: 320x240, pre-expanded to 640-wide RGB565 rows in SRAM
// (Stage 1's exact-double geometry -- see file header). Vertical 2x is
// done by re-reading each row twice (see dma_irq_handler).

#define SRC_ROWS  240
#define ROW_WORDS (MODE_H_ACTIVE_PIXELS / 2)  // 2 RGB565 pixels/word

static uint32_t framebuf[SRC_ROWS][ROW_WORDS];

#define PATTERN_COUNT 4

// Classic 8-bar pattern, TRUE RGB565 values -- see
// firmware/phase0-dvi-colorfix/README.md for the expand_tmds derivation
// that makes these display correctly without compensation.
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

static void fill_pattern(uint8_t pattern_id) {
    for (int row = 0; row < SRC_ROWS; row++) {
        for (int w = 0; w < ROW_WORDS; w++) {
            int x0 = w * 2;
            int x1 = w * 2 + 1;
            uint16_t c0, c1;
            switch (pattern_id) {
                case 1:  // solid red
                    c0 = c1 = 0xF800;
                    break;
                case 2:  // solid green
                    c0 = c1 = 0x07E0;
                    break;
                case 3: {  // checkerboard, 40px blocks
                    int block_x0 = x0 / 40, block_x1 = x1 / 40, block_y = row / 40;
                    c0 = ((block_x0 + block_y) & 1) ? 0xFFFF : 0x0000;
                    c1 = ((block_x1 + block_y) & 1) ? 0xFFFF : 0x0000;
                    break;
                }
                default:  // 0 (and any out-of-range fallback): colour bars
                    c0 = bar_colours[x0 / 80];
                    c1 = bar_colours[x1 / 80];
                    break;
            }
            framebuf[row][w] = (uint32_t)c0 | ((uint32_t)c1 << 16);
        }
    }
}

// ----------------------------------------------------------------------------
// HSTX command lists (padded with NOPs to be >= HSTX FIFO size) -- byte-for-
// byte Stage 1's proven shape (firmware/phase0-dvi/).

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
// DMA logic -- two channels ping-ponging via chain_to, unchanged from
// Stage 1. Reads whatever fill_pattern() last wrote into framebuf; a
// pattern change from the SPI0 command loop below can land mid-scanout
// (framebuf isn't double-buffered here) -- worst case is a one-frame
// visual tear during a pattern switch, an acceptable cosmetic cost for a
// test card, not a correctness issue.

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

static void hstx_dvi_init(void) {
    // Corrected TMDS lane config for RGB565 -- see
    // firmware/phase0-dvi-colorfix/README.md for the full derivation.
    // L0=Blue, L1=Green, L2=Red (the real DVI/HDMI lane order).
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |  // Red:   5 bits
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |  // Green: 6 bits
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |  // Blue:  5 bits
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // N_SHIFTS=5, SHIFT=2, CLKDIV=5 -- see phase0-dvi/README.md for the
    // datasheet-sourced derivation (CLKDIV is NOT a pixel-clock divisor).
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Metro's HSTX port: CKP/CKN=GPIO14/15, D0P/D0N=GPIO18/19,
    // D1P/D1N=GPIO16/17, D2P/D2N=GPIO12/13 -- silicon-fixed, same as the
    // Feather (HSTX bit index n is always GPIO 12+n).
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    static const int lane_to_output_bit[3] = {6, 4, 0};  // lane0->D0, lane1->D1, lane2->D2
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
}

// ----------------------------------------------------------------------------
// SPI0 slave: pattern-select command link -- mode 3 (CPOL=1, CPHA=1), CS
// held low for the whole burst, the exact protocol confirmed working on
// real hardware in Phase 0 A2/C3 (firmware/phase0-a2-spi/). Pin roles:
// HS_F=MOSI(GPIO20), HS_G=CS(GPIO21), HS_H=SCK(GPIO22), HS_I=MISO(GPIO23)
// -- main README section 4.1's resolved role assignment.

#define SPI_PORT  spi0
#define PIN_MOSI  20
#define PIN_CS    21
#define PIN_SCK   22
#define PIN_MISO  23
#define XFER_LEN  16
#define ACK_BYTE  0xC0  // must match badge_app/app.py's ACK_BYTE

static void spi0_slave_init(void) {
    spi_init(SPI_PORT, 1000 * 1000);  // nominal only -- slave derives timing from the badge's SCK
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
}

// ----------------------------------------------------------------------------

int main(void) {
    // Section 5 mitigation #1: I2C0 target up before anything else,
    // including clock reconfiguration -- see eeprom_i2c.h.
    eeprom_i2c_start();

    // clk_hstx follows clk_sys undivided by default; 640x480@60 needs
    // clk_sys=126MHz -- see phase0-dvi/README.md for the derivation.
    set_sys_clock_khz(126000, true);

    fill_pattern(0);  // colour bars, shown until the badge requests otherwise
    hstx_dvi_init();
    spi0_slave_init();

    // tx_buf primed with the ack for whatever's currently showing (0 at
    // boot) -- correct even before any SPI transfer has happened.
    uint8_t current_pattern = 0;
    uint8_t tx_buf[XFER_LEN] = {0};
    tx_buf[0] = ACK_BYTE;
    tx_buf[1] = current_pattern;

    for (;;) {
        uint8_t rx_buf[XFER_LEN];
        spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, XFER_LEN);

        // rx_buf[0] is the badge's requested pattern for THIS transfer;
        // tx_buf just sent was the ack for the PREVIOUS one (full-duplex
        // means a single blocking transfer can't reply to data it's
        // still receiving) -- the badge app's own status line accounts
        // for this one-round lag; see its file header.
        uint8_t requested = rx_buf[0];
        if (requested < PATTERN_COUNT && requested != current_pattern) {
            current_pattern = requested;
            fill_pattern(current_pattern);
        }

        tx_buf[0] = ACK_BYTE;
        tx_buf[1] = current_pattern;
    }
}
