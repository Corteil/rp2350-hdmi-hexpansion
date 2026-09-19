// pio-testcard: firmware/testcard's real HSTX/DVI video pipeline and
// double-buffered framebuffer, combined with firmware/mirror_debug's
// PIO-based SPI0 slave receiver -- see this project's own CMakeLists.txt
// comment and hexigfx-mirror-status memory (2026-09-19) for the full
// history. Where testcard/src/main.c's own receiver spoke the OLDER
// badge_app/app.py synthetic-frame protocol (mode 3, an 8-byte 0xA55A
// marker, no real CS framing), this firmware speaks hazanjon's REAL
// `flow3r_bsp_display_mirror.c` protocol from badge-2024-software: mode 0
// (CPOL=0, CPHA=0), a real 4-byte "TDHD" header, one continuous CS-low
// burst per whole frame (header immediately followed by the raw 240x240
// RGB565 pixel payload, CS only deasserting once the whole frame is
// sent) -- exactly what mirror_debug proved receives correctly on real
// hardware via a from-scratch PIO program instead of the RP2350's own
// hardware SPI0/PL022 peripheral (which loses byte-alignment mid-burst
// and never recovers -- proven via a real logic-analyzer capture plus a
// fine-grained SSPSR/DMA trace, see spi_slave_rx.pio's own comment).
//
// Board: Metro RP2350, same bench rig as testcard/mirror_debug (SPI0 on
// GPIO20-23, HSTX on GPIO12-19). No I2C0/EEPROM hexpansion emulation here
// (unlike testcard) -- the real driver under test lives inside
// badge-2024-software itself (triggered via its own sideloaded
// mirror_screen_test/display_manager apps), not via anything this
// hexpansion's own EEPROM would need to serve, and mirror_debug proved
// this exact bench setup works fine without it.
//
// HSTX geometry, framebuffer layout, double-buffering, EMF test-card/
// static-noise fallback patterns, and the DMA ping-pong scanout itself
// are all UNCHANGED from testcard/src/main.c (already proven on real
// hardware) -- only the SPI0 receive path and main()'s own frame-
// assembly loop are new.

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "spi_slave_rx.pio.h"
#include "ws2812.pio.h"

// ----------------------------------------------------------------------------
// DVI constants (standard CEA-861 VIC 1, 640x480@60) -- identical to
// firmware/testcard/src/main.c.

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
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Source image: 240x240 (the real Tildagon screen's own resolution,
// matching hazanjon's real payload size of SRC_ROWS*ROW_RX_BYTES =
// 240*480 = 115200 bytes -- see [[hexigfx-mirror-status]]'s own repeated
// "~100-110ms per 115200-byte payload" measurements), pre-doubled
// horizontally to 480-wide RGB565 rows in SRAM -- unchanged geometry from
// testcard/src/main.c.

#define SRC_ROWS   240
#define DBL_W      480                 // 240 source pixels, doubled
#define DBL_WORDS  (DBL_W / 2)         // 2 RGB565 pixels/word -- also equals the real, undoubled pixels/row (240), since this image happens to be square
#define PILLARBOX  ((MODE_H_ACTIVE_PIXELS - DBL_W) / 2)  // 80px each side

static uint32_t framebuf[2][SRC_ROWS][DBL_WORDS];
static volatile int front_index = 0;
static volatile bool back_buffer_ready = false;
static volatile uint32_t swap_count = 0;  // TEMP DEBUG: real front/back swaps done by dma_irq_handler

static inline int back_index(void) {
    return 1 - front_index;
}

// "No signal" pattern -- shown once a real frame has landed before, then
// the link goes idle. Unchanged from testcard/src/main.c.
static uint32_t rng_state = 0xC0FFEEu;

static inline uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static inline uint16_t random_gray_pixel(void) {
    uint8_t v = (uint8_t)(xorshift32() & 0xFF);
    return (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
}

static void fill_static_noise_row(int buf, int row) {
    for (int w = 0; w < DBL_WORDS; w++) {
        uint16_t c0 = random_gray_pixel();
        uint16_t c1 = random_gray_pixel();
        framebuf[buf][row][w] = (uint32_t)c0 | ((uint32_t)c1 << 16);
    }
}

static void fill_static_noise(void) {
    int buf = back_index();
    for (int row = 0; row < SRC_ROWS; row++) {
        fill_static_noise_row(buf, row);
    }
    back_buffer_ready = true;
}

// EMF Camp colour-bar + logo test card -- boot-time liveness indicator
// shown until the badge's first full frame arrives. Unchanged from
// testcard/src/main.c.
static const uint16_t bar_colours[8] = {
    0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
};

#define EMF_LOGO_SIZE 64
static const uint8_t emf_logo_bitmap[EMF_LOGO_SIZE][EMF_LOGO_SIZE / 8] = {
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0xc0,0x00,0x00,0x00},
    {0x1f,0x80,0x00,0x07,0xe0,0x00,0x00,0x00},
    {0x7f,0xfc,0x00,0x7f,0xfe,0x00,0x00,0x00},
    {0x78,0x00,0x01,0xff,0xff,0x80,0x00,0x00},
    {0x70,0x00,0x07,0xf0,0x0f,0xe0,0x00,0x38},
    {0x70,0x00,0x1f,0x80,0x01,0xf0,0x00,0x38},
    {0x70,0x00,0x1e,0x00,0x00,0x7c,0x00,0x10},
    {0x30,0x00,0x3c,0x00,0x00,0x3e,0x00,0x00},
    {0x30,0x00,0x78,0x00,0x00,0x1e,0x00,0x00},
    {0x10,0x00,0xf0,0x00,0x00,0x0f,0x00,0x00},
    {0x08,0x01,0xe0,0x00,0x00,0x07,0x80,0x00},
    {0x0c,0x01,0xc0,0x00,0x00,0x03,0x80,0x00},
    {0x06,0x03,0xc0,0x00,0x00,0x01,0xc0,0x00},
    {0x02,0x03,0x80,0x00,0x00,0x01,0xc0,0x00},
    {0x01,0x07,0x80,0x00,0x00,0x00,0xe0,0x00},
    {0x00,0x87,0x00,0x00,0x00,0x00,0xe0,0x00},
    {0x00,0x47,0x00,0x00,0x00,0x00,0xe0,0x00},
    {0x00,0x27,0x00,0x00,0x00,0x00,0xe0,0x00},
    {0x00,0x0e,0x00,0x1c,0x00,0x00,0x70,0x00},
    {0x00,0x0e,0x00,0x3f,0x07,0x00,0x70,0x00},
    {0x00,0x0e,0x00,0x7f,0xcf,0x00,0x70,0x00},
    {0x00,0x0f,0x00,0xff,0xfe,0x00,0x70,0x00},
    {0x00,0x0f,0x00,0xe1,0xfe,0x00,0x70,0x00},
    {0x00,0x06,0x00,0x40,0x78,0x00,0x78,0x00},
    {0x00,0x07,0x00,0x00,0x00,0x00,0xe4,0x00},
    {0x00,0x07,0x00,0x00,0x00,0x00,0xe2,0x00},
    {0x00,0x07,0x00,0x00,0x00,0x00,0xe1,0x00},
    {0x00,0x03,0x80,0x00,0x00,0x01,0xe0,0x80},
    {0x00,0x03,0x80,0x00,0x00,0x01,0xc0,0x40},
    {0x00,0x03,0xc0,0x00,0x00,0x03,0xc0,0x20},
    {0x00,0x01,0xc0,0x00,0x00,0x03,0x80,0x10},
    {0x00,0x01,0xe0,0x00,0x00,0x07,0x80,0x18},
    {0x00,0x00,0xf0,0x00,0x00,0x0f,0x00,0x08},
    {0x00,0x00,0x78,0x00,0x00,0x1e,0x00,0x0c},
    {0x00,0x00,0x3c,0x00,0x00,0x3c,0x00,0x0e},
    {0x00,0x00,0x1f,0x00,0x01,0xf8,0x00,0x06},
    {0x00,0x00,0x0f,0xc0,0x03,0xf8,0x00,0x0e},
    {0x00,0x00,0x03,0xfc,0x3f,0xe0,0x00,0x0f},
    {0x00,0x00,0x01,0xff,0xff,0x80,0xc0,0x3e},
    {0x00,0x00,0x00,0x3f,0xfc,0x00,0x1f,0xfc},
    {0x00,0x00,0x00,0x03,0xc0,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x01,0x80,0x00,0x00,0x00},
};

#define EMF_LOGO_ROW0 88
#define EMF_LOGO_COL0 88
#define EMF_LOGO_ORANGE 0xF3E0u
#define EMF_LOGO_BLACK  0x0000u

static void fill_test_card_row(int buf, int row) {
    for (int w = 0; w < DBL_WORDS; w++) {
        int x0 = w * 2;
        int x1 = w * 2 + 1;
        uint16_t c0 = bar_colours[x0 / 60];
        uint16_t c1 = bar_colours[x1 / 60];
        framebuf[buf][row][w] = (uint32_t)c0 | ((uint32_t)c1 << 16);
    }
}

static void fill_test_card(int buf) {
    for (int row = 0; row < SRC_ROWS; row++) {
        fill_test_card_row(buf, row);
    }
}

static void draw_test_card_logo_row(int buf, int logo_row) {
    int fr = EMF_LOGO_ROW0 + logo_row;
    for (int w = 0; w < EMF_LOGO_SIZE; w++) {
        int fw = EMF_LOGO_COL0 + w;
        bool ink = (emf_logo_bitmap[logo_row][w / 8] >> (7 - (w % 8))) & 1;
        uint16_t colour = ink ? EMF_LOGO_ORANGE : EMF_LOGO_BLACK;
        framebuf[buf][fr][fw] = (uint32_t)colour | ((uint32_t)colour << 16);
    }
}

static uint16_t row_half_width[MODE_V_ACTIVE_LINES];

static void build_row_table(void) {
    const float radius = DBL_W / 2.0f;
    const float centre = (MODE_V_ACTIVE_LINES - 1) / 2.0f;
    for (int row = 0; row < MODE_V_ACTIVE_LINES; row++) {
        float dy = row - centre;
        float hw = 0.0f;
        if (fabsf(dy) < radius) {
            hw = sqrtf(radius * radius - dy * dy);
        }
        int half_width = (int)(hw + 0.5f);
        half_width -= half_width & 1;
        if (half_width < 0) half_width = 0;
        if (half_width > (int)radius) half_width = (int)radius;
        row_half_width[row] = (uint16_t)half_width;
    }
}

// ----------------------------------------------------------------------------
// HSTX command lists -- unchanged from testcard/src/main.c.

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

#define ZERO_BUF_WORDS 160
static uint32_t zero_buf[ZERO_BUF_WORDS] = {0};

// ----------------------------------------------------------------------------
// DMA scanout -- unchanged from testcard/src/main.c.

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static volatile uint v_scanline = 2;
static int active_phase = 0;

void __scratch_x("") dma_irq_handler(void) {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline == 0 && back_buffer_ready) {
        front_index = 1 - front_index;
        back_buffer_ready = false;
        swap_count++;
    }

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

    uint v_active = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    uint half_width = row_half_width[v_active];
    uint left_black = PILLARBOX + (DBL_W / 2 - half_width);
    uint right_black = left_black;

    if (active_phase == 0) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        active_phase = 1;
    } else if (active_phase == 1) {
        ch->read_addr = (uintptr_t)zero_buf;
        ch->transfer_count = left_black / 2;
        active_phase = 2;
    } else if (active_phase == 2) {
        uint src_row = (v_active / 2) % SRC_ROWS;
        uint col_word_offset = (DBL_W / 2 - half_width) / 2;
        ch->read_addr = (uintptr_t)(&framebuf[front_index][src_row][col_word_offset]);
        ch->transfer_count = half_width;
        active_phase = 3;
    } else {
        ch->read_addr = (uintptr_t)zero_buf;
        ch->transfer_count = right_black / 2;
        active_phase = 0;
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

static void hstx_dvi_init(void) {
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

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
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(
        DMACH_PING, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false
    );
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(
        DMACH_PONG, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false
    );

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}

static void core1_video_entry(void) {
    hstx_dvi_init();
    for (;;) {
        tight_loop_contents();
    }
}

// ----------------------------------------------------------------------------
// SPI0 slave receiver -- PIO-based (see spi_slave_rx.pio's own comment and
// hexigfx-mirror-status memory, 2026-09-19, for the full evidence trail),
// speaking hazanjon's REAL protocol: mode 0, a real 4-byte "TDHD" header,
// one continuous CS-low burst per whole frame. Unlike mirror_debug (which
// only ever tracked byte/header counters for a diagnostic border colour),
// this receiver writes real pixels straight into the framebuffer as they
// arrive.

#define PIN_MOSI 20
#define PIN_CS   21
#define PIN_SCK  22
#define PIN_MISO 23

#define SPI_SLAVE_PIO pio1
#define SPI_SLAVE_SM  0

// 2048, not 4096: SRAM_SCRATCH_Y is 4KB total but the SDK's default linker
// script already places core0's own stack at its end (2KB, see
// sections_stack.incl), leaving only 2KB genuinely free -- confirmed by a
// real link-time overflow error when this was first tried at 4096. Still
// ~1.6ms of buffering margin at SPI0's real ~1.25MB/s max byte rate, and
// the actual point of this move (see spi_rx_ring's own declaration
// comment) is bank ISOLATION from HSTX's own SRAM traffic, not raw size.
#define SPI_RING_BITS 11
#define SPI_RING_SIZE (1u << SPI_RING_BITS)
// Placed in SRAM_SCRATCH_Y (a genuinely separate 4KB SRAM region from the
// main striped SRAM banks framebuf/vactive_line/etc. live in, RP2350's own
// dma_irq_handler already uses the OTHER such region, SCRATCH_X, for the
// same reason -- see that function's own __scratch_x attribute above).
// Added after real hardware evidence (this session) that header_match_count
// stays at exactly 0 -- total data corruption, not just occasional glitches
// -- specifically whenever HSTX video is genuinely active, despite the
// FIFO-join fix and normal (non-boosted) SPI DMA priority: this ring is the
// highest-bandwidth, most timing-critical DMA destination in the whole SPI
// receive path (a continuous ~1.25MB/s stream from PIO's own FIFO), so if
// it happens to share a physical SRAM bank with whatever HSTX is hammering
// for its own zero-margin scanout reads, that's real bank-level contention
// no DMA-channel-priority setting can arbitrate away. SPI_RING_SIZE is
// exactly 4096 bytes -- fits the whole scratch region precisely, and
// SRAM_SCRATCH_Y_BASE is itself 4096-aligned, satisfying the DMA ring's own
// alignment requirement for free.
static uint8_t spi_rx_ring[SPI_RING_SIZE] __scratch_y("spi_rx_ring") __attribute__((aligned(SPI_RING_SIZE)));
static uint32_t spi_ring_read_pos = 0;
static uint spi_rx_dma_chan;

// "TDHD" as hazanjon's driver sends it (MSB-first per byte).
#define TDHD_MARKER_VALUE 0x54444844u
static uint32_t marker_window = 0;

#define PIXELS_PER_ROW DBL_WORDS               // 240 real (undoubled) pixels/row
#define TOTAL_PIXELS   (SRC_ROWS * PIXELS_PER_ROW)  // 57600 -- 115200 payload bytes / 2

static volatile bool header_matched = false;   // true once "TDHD" has been seen in the CURRENT CS-low burst
static volatile uint32_t pixel_index = 0;      // 0..TOTAL_PIXELS-1 once header_matched
static uint8_t pending_high_byte = 0;
static bool have_high_byte = false;

// Set the instant "TDHD" is matched; cleared by main() once it has safely
// claimed a fresh back buffer for this frame (waiting out any in-progress
// front/back flip first) -- see main()'s own comment. Bytes arriving while
// this is still true are dropped (not written, not counted into
// pixel_index), which only ever costs the very first few pixels of one
// frame in the (expected to be effectively never, per testcard's own
// original reasoning: a frame takes >>1 vblank period to arrive) case
// where the wait actually takes any real time.
static volatile bool frame_start_pending = false;
static int recv_buf = 0;                        // touched only by main(), see above
static volatile bool frame_complete = false;    // set once pixel_index reaches TOTAL_PIXELS; cleared by main()

// TEMP DEBUG counters, same spirit as mirror_debug's own diagnostics.
static volatile uint32_t total_bytes_seen = 0;
static volatile uint32_t header_match_count = 0;
static volatile uint32_t frames_received = 0;

static void pio_spi_slave_init(void) {
    pio_gpio_init(SPI_SLAVE_PIO, PIN_MOSI);
    pio_gpio_init(SPI_SLAVE_PIO, PIN_CS);
    pio_gpio_init(SPI_SLAVE_PIO, PIN_SCK);
    gpio_disable_pulls(PIN_MOSI);
    gpio_disable_pulls(PIN_CS);
    gpio_disable_pulls(PIN_SCK);
    gpio_disable_pulls(PIN_MISO);

    uint offset = pio_add_program(SPI_SLAVE_PIO, &spi_slave_rx_program);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + spi_slave_rx_wrap_target, offset + spi_slave_rx_wrap);
    sm_config_set_in_pins(&c, PIN_MOSI);
    sm_config_set_jmp_pin(&c, PIN_CS);
    sm_config_set_in_shift(&c, false, true, 8);
    // This SM never transmits (RX-only), so join the unused TX FIFO to the
    // RX side: doubles the RX FIFO from 4 to 8 words. Added after directly
    // confirming (via PIO1's own FDEBUG.RXSTALL bit for this SM, read live
    // over SWD) that the RX FIFO was genuinely overflowing once the SPI RX
    // DMA channel was made non-high-priority (see this function's own DMA
    // config comment below for why that change was needed) -- HSTX's own
    // bus bursts could occasionally starve DMA servicing for long enough
    // to overrun a plain 4-word FIFO. This directly doubles the real time
    // margin (400ns -> 800ns at the badge's 10MHz SCK) with no DMA-
    // priority/bus-arbitration tradeoff at all.
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(SPI_SLAVE_PIO, SPI_SLAVE_SM, offset, &c);
    pio_sm_set_enabled(SPI_SLAVE_PIO, SPI_SLAVE_SM, true);

    spi_rx_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c2 = dma_channel_get_default_config(spi_rx_dma_chan);
    channel_config_set_transfer_data_size(&c2, DMA_SIZE_8);
    channel_config_set_read_increment(&c2, false);
    channel_config_set_write_increment(&c2, true);
    channel_config_set_dreq(&c2, pio_get_dreq(SPI_SLAVE_PIO, SPI_SLAVE_SM, false));  // false = RX
    channel_config_set_ring(&c2, true, SPI_RING_BITS);
    // Deliberately NOT high-priority, unlike mirror_debug's own copy of
    // this init function -- testcard/src/main.c's own history (see its
    // hstx_dvi_init() comment) already documents that a competing DMA
    // channel broke HDMI sync on this exact HSTX geometry even WITHOUT
    // being marked high-priority, which is the whole reason video runs on
    // its own core here. This session's own first attempt at this port
    // copied mirror_debug's high-priority flag verbatim and produced a
    // real, reproducible loss of signal (confirmed via a control-test
    // reflash of testcard's own long-proven bringup.elf on the identical
    // physical rig, which displayed correctly, isolating the cause to
    // this firmware specifically) -- removing it is the direct fix:
    // competing with HSTX's own two high-priority channels at the SAME
    // top arbitration tier undermines the very isolation the two-DMA-
    // channel/two-core split was designed to provide, whereas SPI0's own
    // real byte rate (~1.25MB/s at 10MHz) has enormous slack against a
    // plain (non-boosted) DMA channel's normal throughput.
    dma_channel_configure(
        spi_rx_dma_chan, &c2,
        spi_rx_ring, &SPI_SLAVE_PIO->rxf[SPI_SLAVE_SM],
        0xFFFFFFFFu,
        true
    );
}

// CS-edge GPIO IRQ: only needed to reset header/marker state at the start
// of each fresh burst (the PIO program itself handles bit-level framing
// entirely in hardware -- see spi_slave_rx.pio's own comment).
static void cs_edge_irq_handler(uint gpio, uint32_t events) {
    if (gpio != PIN_CS) return;
    if (events & GPIO_IRQ_EDGE_FALL) {
        header_matched = false;
        marker_window = 0;
        have_high_byte = false;
    }
}

// Drains whatever spi_rx_dma_chan has written into spi_rx_ring since the
// last call, scans for "TDHD", then writes real pixels straight into
// framebuf[recv_buf] as they arrive. Called directly from main()'s loop,
// once per iteration -- not an interrupt handler.
static void spi0_process_ring(void) {
    uint32_t write_addr = dma_channel_hw_addr(spi_rx_dma_chan)->write_addr;
    uint32_t write_pos = (uint32_t)(write_addr - (uintptr_t)spi_rx_ring) & (SPI_RING_SIZE - 1);

    while (spi_ring_read_pos != write_pos) {
        uint8_t b = spi_rx_ring[spi_ring_read_pos];
        spi_ring_read_pos = (spi_ring_read_pos + 1) & (SPI_RING_SIZE - 1);
        total_bytes_seen++;

        if (!header_matched) {
            marker_window = (marker_window << 8) | b;
            if (marker_window == TDHD_MARKER_VALUE) {
                header_matched = true;
                header_match_count++;
                have_high_byte = false;
                // Unconditionally treats every "TDHD" match as the start
                // of a fresh frame (reset pixel_index, claim a new back
                // buffer). A "smarter" version that tried to detect and
                // handle a possible per-chunk header re-send (treating a
                // mid-frame match as a continuation instead) was tried
                // this session and made things WORSE (header_match_count
                // got stuck at exactly 0) for reasons not fully understood
                // -- reverted. header_match_count does sometimes run
                // slightly ahead of frames_received in practice (a header
                // match that doesn't lead to a completed frame), which may
                // point to a genuine per-chunk resend, but a correct fix
                // for that needs more careful investigation than this
                // session had time for. See hexigfx-mirror-status memory,
                // 2026-09-19/20, for the full evidence trail.
                pixel_index = 0;
                frame_start_pending = true;
            }
            continue;  // this byte was part of the marker scan, not payload
        }

        if (frame_start_pending) {
            continue;  // main() hasn't claimed a fresh back buffer yet -- see its own declaration comment
        }

        if (pixel_index >= (uint32_t)TOTAL_PIXELS) {
            continue;  // trailing bytes before CS actually rises -- ignore
        }

        if (!have_high_byte) {
            pending_high_byte = b;
            have_high_byte = true;
        } else {
            have_high_byte = false;
            // hazanjon's attach_mirror sink forwards tildagon_fb raw
            // (CTX_FORMAT_RGB565_BYTESWAPPED) -- high byte first, matching
            // testcard/src/main.c's own original byte-order derivation.
            uint32_t px = ((uint32_t)pending_high_byte << 8) | b;
            uint32_t row = pixel_index / PIXELS_PER_ROW;
            uint32_t col = pixel_index % PIXELS_PER_ROW;
            framebuf[recv_buf][row][col] = px | (px << 16);
            pixel_index++;
            if (pixel_index == (uint32_t)TOTAL_PIXELS) {
                frame_complete = true;
            }
        }
    }
}

// ----------------------------------------------------------------------------

#define PIN_NEOPIXEL   25
#define NEOPIXEL_PIO   pio0
#define NEOPIXEL_SM    0

static inline void neopixel_put(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    pio_sm_put_blocking(NEOPIXEL_PIO, NEOPIXEL_SM, grb);
}

static void neopixel_init(void) {
    uint offset = pio_add_program(NEOPIXEL_PIO, &ws2812_program);
    ws2812_program_init(NEOPIXEL_PIO, NEOPIXEL_SM, offset, PIN_NEOPIXEL, 800000.0f, false);
    neopixel_put(0, 0, 0);
}

int main(void) {
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    set_sys_clock_khz(126000, true);

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);

    build_row_table();
    fill_test_card(back_index());
    back_buffer_ready = true;

    // Nothing but the bare minimum happens between the vreg settle delay
    // and multicore_launch_core1() -- pio_spi_slave_init() (a PIO program
    // load), neopixel_init() (also a PIO program load), and the CS-edge
    // GPIO IRQ enable are ALL deferred until AFTER the launch. Matches
    // mirror_debug's own hard-won lesson (see its src/main.c's identical
    // comment): anything that can preempt or briefly block core0 -- an
    // IRQ, or PIO program loading/SM config -- risks disrupting
    // multicore_launch_core1()'s SIO-FIFO-based handoff sequence if it
    // lands mid-handshake (a documented RP2350 core1-launch erratum).
    // mirror_debug itself only moved neopixel_init() after the launch and
    // left its own pio_spi_slave_init() call before it -- this file goes
    // one step further and moves both, since this session's first attempt
    // at this exact ordering (PIO SPI init before the launch) produced a
    // healthy-looking SPI/frame-counter trail but no monitor picture at
    // all, consistent with core1 never actually reaching hstx_dvi_init().
    multicore_launch_core1(core1_video_entry);

    pio_spi_slave_init();
    neopixel_init();
    gpio_set_irq_enabled_with_callback(PIN_CS, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &cs_edge_irq_handler);

    bool heartbeat_red_on = false;
    absolute_time_t next_red_toggle = make_timeout_time_ms(500);
    absolute_time_t green_flash_until = nil_time;

    #define IDLE_AFTER_MS 500
    absolute_time_t last_byte_time = get_absolute_time();
    uint32_t last_total_bytes_seen = 0;

    bool ever_received_frame = false;
    bool testcard_logo_on = false;
    absolute_time_t next_testcard_blink = make_timeout_time_ms(700);

#ifdef DEBUG_SERIAL
    stdio_init_all();
    sleep_ms(200);
    printf("\npio_testcard DEBUG_SERIAL build: entering SPI0 receive loop (PIO-based, mode 0, TDHD header)\n");
#endif

    for (;;) {
        spi0_process_ring();

        if (total_bytes_seen != last_total_bytes_seen) {
            last_total_bytes_seen = total_bytes_seen;
            last_byte_time = get_absolute_time();
        }

        if (frame_start_pending) {
            // Same reasoning as testcard/src/main.c's original row_index==0
            // wait: a full frame takes ~100ms+ to arrive at 10MHz, many
            // multiples of one ~16ms vblank period, so in practice the
            // previous flip is always long done by the time a new frame's
            // header arrives -- this essentially never actually spins.
            while (back_buffer_ready) {
                tight_loop_contents();
            }
            recv_buf = back_index();
            frame_start_pending = false;
        }

        if (frame_complete) {
            frame_complete = false;
            frames_received++;
            ever_received_frame = true;
            back_buffer_ready = true;
            green_flash_until = make_timeout_time_ms(80);
#ifdef DEBUG_SERIAL
            printf("frame landed #%lu, recv_buf=%d, front_index=%d, swap_count=%lu, header_match_count=%lu, total_bytes_seen=%lu\n",
                   (unsigned long)frames_received, recv_buf, front_index,
                   (unsigned long)swap_count, (unsigned long)header_match_count,
                   (unsigned long)total_bytes_seen);
#endif
        }

        bool genuinely_idle =
            absolute_time_diff_us(last_byte_time, get_absolute_time()) >= IDLE_AFTER_MS * 1000;
        if (genuinely_idle && ever_received_frame) {
            fill_static_noise();
        }

        if (total_bytes_seen == 0 && time_reached(next_testcard_blink)) {
            testcard_logo_on = !testcard_logo_on;
            next_testcard_blink = delayed_by_ms(next_testcard_blink, 700);
            int buf = back_index();
            fill_test_card(buf);
            if (testcard_logo_on) {
                for (int logo_row = 0; logo_row < EMF_LOGO_SIZE; logo_row++) {
                    draw_test_card_logo_row(buf, logo_row);
                }
            }
            back_buffer_ready = true;
        }

        if (time_reached(next_red_toggle)) {
            heartbeat_red_on = !heartbeat_red_on;
            next_red_toggle = delayed_by_ms(next_red_toggle, 500);
        }
        bool green_active = !is_nil_time(green_flash_until) && !time_reached(green_flash_until);
        if (green_active) {
            neopixel_put(0, 20, 0);
        } else {
            neopixel_put(heartbeat_red_on ? 20 : 0, 0, 0);
        }

        sleep_us(200);
    }
}
