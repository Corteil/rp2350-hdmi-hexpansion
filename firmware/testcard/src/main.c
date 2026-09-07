// Hexi-GFX end-to-end link test (rp2350-hdmi-hexpansion, VID=0x1969
// PID=0x4544): the first firmware combining every Phase 0 piece into
// one image and one demonstration -- see ../../../README.md section 8.
//
// Badge plugs in -> discovers the emulated EEPROM (I2C0 target,
// eeprom_i2c.c, real VID/PID) -> mounts the real LittleFS image inside
// it (fs_image.h) -> auto-launches badge_app/app.py -> that app opens
// SPI0 (mode 3, CS-continuous -- the exact protocol proven in Phase 0
// A2/C3) and streams a live, badge-generated frame, row by row -> this
// firmware writes each row into framebuf and the connected monitor
// (HSTX/DVI, colour-correct per Phase 0 A1) shows it, continuously, "as
// if mirrored from the badge". Every link in that chain was proven
// individually in Phase 0; this is the first firmware to run all of
// them at once.
//
// 2026-08-30: reworked from a 4-pattern SELECT-command demo (badge picks
// among a few RP2350-generated test cards) into this -- the badge itself
// generates 240x240 RGB565 frame content in Python and streams it,
// matching what the real product's primary mode actually does (main
// README section 3.1), not just proving a control link. NOT a literal
// mirror of the badge's own rendered screen -- that needs display.get_fb()
// (Phase 0 C2's patch, written but never verified, and would mean
// flashing custom firmware onto a real physical badge, out of scope
// here) -- the badge instead builds pixel data directly in Python. Same
// wire format and bandwidth profile the real thing needs either way.
//
// Board: Metro RP2350 -- same bench rig as A2/C3/C4 (I2C0 on GPIO4/5,
// SPI0 on GPIO20-23) and phase0-dvi-colorfix (HSTX on GPIO12-19, its own
// dedicated 22-pin connector). All three pin groups are disjoint, so one
// physical board runs the whole demo.
//
// HSTX geometry: A1 Stage 2's real target -- 240x240 source, pillarboxed
// and circular-masked into 640x480 (matching the badge's own round
// display, per main README section 3.1/3.4) -- not Stage 1's simpler
// exact-double 320x240 shape the original pattern-select version of this
// file used. Row-fill/mask logic is phase0-dvi2's proven code, unchanged;
// colour config is the root-cause fix from firmware/phase0-dvi-colorfix/.

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ws2812.pio.h"

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

#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Source image: 240x240 (the badge's own screen size), pre-doubled
// horizontally to 480-wide RGB565 rows in SRAM -- byte-for-byte
// phase0-dvi2's proven geometry. Vertical 2x re-reads each row twice
// (see dma_irq_handler). 240 words/row * 240 rows * 4 bytes = 230400
// bytes.

#define SRC_ROWS   240
#define DBL_W      480                 // 240 source pixels, doubled
#define DBL_WORDS  (DBL_W / 2)         // 2 RGB565 pixels/word
#define PILLARBOX  ((MODE_H_ACTIVE_PIXELS - DBL_W) / 2)  // 80px each side

// Real double buffering (2026-09-07) -- HEX_EEPROM_SIZE shrank from
// 64 KiB to 8 KiB (see eeprom_i2c.c) specifically to make room for this
// second 230400-byte copy: framebuf[2][...][...], one "front" (DMA scans
// it out continuously) and one "back" (the CPU writes freely into it,
// with NO per-row gating needed -- DMA never touches this index, so
// there is no race to guard against). front_index is flipped by
// dma_irq_handler itself, only ever at the start of vblank (v_scanline
// == 0, see below) and only when back_buffer_ready says a complete new
// frame is waiting -- the flip is a single word write, so it can't be
// caught mid-scanout the way the old single-buffer row-by-row copy
// could. This replaces both frame_staging (received rows now go
// straight into the back buffer's doubled/BGR-corrected words, no
// separate source-resolution staging copy needed) and
// framebuf_row_write_is_safe()/wait_until_row_write_safe() (nothing
// outside dma_irq_handler touches the front buffer at all, so there's
// nothing left for them to guard).
static uint32_t framebuf[2][SRC_ROWS][DBL_WORDS];
static volatile int front_index = 0;
static volatile bool back_buffer_ready = false;

static inline int back_index(void) {
    return 1 - front_index;
}

// "No signal" pattern -- analog-TV-style static, generated here on the
// RP2350 (not sent by the badge), shown at boot before the badge's first
// streamed row ever arrives. A simple xorshift32 PRNG, not
// cryptographic-quality -- fine for visual noise.
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
    // RGB565 gray: top bits of the same 8-bit value into each channel's
    // field width (5/6/5), so R==B and G is the closest 6-bit match --
    // visually neutral gray, not tinted.
    return (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
}

// The real GC9A01 panel driver (badge-2024-software's
// components/flow3r_bsp/flow3r_bsp_gc9a01.c, flow3r_bsp_gc9a01_init())
// sets MADCTL_BGR, so panel hardware -- not the badge's software --
// swaps which 5-bit field drives Red vs. Blue when it lights the
// subpixels. The badge's own ctx_565_pack() packs plain RGB565 with no
// BGR awareness, so a receiver that displays those wire bytes as
// straight RGB565 shows every pixel with Red and Blue swapped. Found
// and fixed in spaceagon-display-tap's firmware/phase3-merged/ (same
// name there); ported here because this firmware will hit the
// identical bug once badge_app/app.py's synthetic rotating-colour
// generator is replaced by a real display.get_fb() mirror (this
// project's own README section 3.1/9 risk 4) -- synthetic content never
// goes through the real panel driver, so the bug is latent, not yet
// visible, until that swap happens.
static inline uint16_t undo_madctl_bgr(uint16_t px) {
    uint16_t r = (px >> 11) & 0x1F;
    uint16_t g = (px >> 5) & 0x3F;
    uint16_t b = px & 0x1F;
    return (uint16_t)((b << 11) | (g << 5) | r);
}

static void fill_static_noise_row(int buf, int row) {
    for (int w = 0; w < DBL_WORDS; w++) {
        uint16_t c0 = random_gray_pixel();
        uint16_t c1 = random_gray_pixel();
        framebuf[buf][row][w] = (uint32_t)c0 | ((uint32_t)c1 << 16);
    }
}

// Fills the BACK buffer and flags it ready -- dma_irq_handler flips
// front_index at the next vblank (see its own comment). Safe to call at
// boot too (before core 1/HSTX has even started, back_buffer_ready is
// simply picked up the first time dma_irq_handler runs).
static void fill_static_noise(void) {
    int buf = back_index();
    for (int row = 0; row < SRC_ROWS; row++) {
        fill_static_noise_row(buf, row);
    }
    back_buffer_ready = true;
}

// EMF Camp colour-bar + logo test card -- a boot-time liveness indicator
// shown while waiting for the badge to stream its first full frame,
// distinct from fill_static_noise() above ("no signal" -- the link was
// working and has since gone quiet). Ported verbatim (same tables, same
// rasterisation) from spaceagon-display-tap's firmware/phase3-merged/,
// which uses this same pattern for the same purpose on the same
// framebuf geometry (240 source rows x 480 doubled-word columns).
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

// Rasterized from the official symbol-only SVG (emfcamp.org/about/
// branding, emf2026-logo-black.svg) at 64x64 cells (each cell = one
// framebuf (row,word) entry = a 2x2 output-pixel block), 1 bit/pixel,
// MSB-first, 8 bytes/row.
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
#define EMF_LOGO_ORANGE 0xF3E0u  // EMF's brand orange #F77F02 in RGB565
#define EMF_LOGO_BLACK  0x0000u

// Raw per-buffer writers (like fill_static_noise_row) -- callers pick
// the target buffer once, do all the writes a single logical update
// needs (bars, then optionally the logo on top), and only flag
// back_buffer_ready afterward. Doing it this way (rather than each
// function self-contained like fill_static_noise()) matters here
// specifically because the logo overlay must land in the SAME back
// buffer as the bars underneath it, before that buffer is handed to
// dma_irq_handler -- see both call sites in main().
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

// Per active-output-row (0..479) half-width of the visible circle, in
// pixels, always even (word-aligned: 2 RGB565 pixels/word) -- unchanged
// from phase0-dvi2, see that file for the full derivation.
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
// HSTX command lists -- unchanged from phase0-dvi2 (proven on hardware).

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

// One HSTX_CMD_TMDS covering the full 640px active width -- see
// phase0-dvi2/src/main.c's comment for why the black pillarbox/mask
// fill must be plain DMA data under this same command, never separate
// TMDS commands (extra command-boundary stalls break horizontal sync).
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
// DMA logic -- two channels ping-ponging via chain_to, four phases per
// active row (command list / left fill / pixel data / right fill), one
// HSTX command total per row -- unchanged from phase0-dvi2. Reads
// framebuf[front_index], never the back buffer -- see framebuf's own
// declaration comment above for the double-buffering design and why the
// swap below can never land mid-scanout.

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static volatile uint v_scanline = 2;  // read from main() only for DEBUG_SERIAL logging now -- must not be cached across a spin loop
static int active_phase = 0;  // 0=left fill, 1=pixel data, 2=right fill

void __scratch_x("") dma_irq_handler(void) {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    // Front/back swap -- only ever right at the start of vertical
    // blanking (v_scanline==0), so the entire blanking period is still
    // ahead of us: a single-word pointer flip here can never be caught
    // mid-scanout of a visible line, unlike the old single-buffer
    // per-row copy this replaced. Whichever of main()'s three framebuf
    // writers (real mirror rows, EMF test card, static noise) just
    // finished a complete update sets back_buffer_ready.
    if (v_scanline == 0 && back_buffer_ready) {
        front_index = 1 - front_index;
        back_buffer_ready = false;
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
        uint src_row = (v_active / 2) % SRC_ROWS;  // vertical 2x
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
    // Highest priority (2026-08-30, found via real hardware testing):
    // this A1-Stage-2-style geometry (4 DMA phases/active row, tighter
    // per-line timing than Stage 1's simpler 2-phase shape -- its own
    // README already documents it as more command-boundary-sensitive)
    // lost HDMI sync specifically once the SPI0 row-streaming loop below
    // started running continuously, while a fresh boot (HSTX/DMA alone,
    // no sustained SPI0 traffic yet) displayed correctly -- isolating
    // the cause to IRQ/bus contention between the two, not a DMA/HSTX
    // config bug (that part is unchanged from phase0-dvi2, already
    // proven). Forcing DMA_IRQ_0 to preempt promptly regardless of what
    // the SPI0 polling loop is doing is the direct fix for exactly that
    // class of problem.
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}

// ----------------------------------------------------------------------------
// SPI0 slave: row-streaming mirror link -- mode 3 (CPOL=1, CPHA=1), CS
// held low per row, the protocol proven in Phase 0 A2/C3
// (firmware/phase0-a2-spi/). Pin roles: HS_F=MOSI(GPIO20),
// HS_G=CS(GPIO21), HS_H=SCK(GPIO22), HS_I=MISO(GPIO23) -- main README
// section 4.1's resolved role assignment.
//
// One SPI burst per FRAME (all 240 rows, 115,200 bytes), not per row --
// changed 2026-08-31 to hit a 15fps target (matching the badge's own
// worst-case app render rate, main README §3.1's C1 measurement).
// Per-row bursts were originally chosen so a mid-transfer disconnect
// only cost one row, not a whole frame -- real, but this receive
// function (see spi0_receive_row(), below) stopped depending on
// CS-level framing entirely on the same day (its own fix #1), so it
// already treats the incoming bytes as one continuous, chunking-agnostic
// stream; the row-vs-frame burst boundary only ever mattered on the
// badge side's own robustness story, not this side's correctness. Badge
// SPI baudrate also raised 1MHz -> 20MHz for the same reason -- both the
// proven-reliable ceiling AND enough real, measured bandwidth for a full
// frame well inside a 15fps budget (Phase 0 C3: 20MHz, one 115,200-byte
// burst, 0 mismatches, 21fps-equivalent, on this same connector+dupont
// wiring). 30MHz+ showed genuine bit errors in that same measurement --
// not used here.
//
// No frame-sync marker: rows are received in a fixed repeating 0..239
// cycle with no explicit "this is row 0" signal, so a resync after a
// corrupted burst can leave the RP2350's row index briefly out of phase
// with the badge's actual row count -- self-corrects within one 240-row
// cycle, visible (if it happens at all) as a brief vertical shift/tear,
// not a lasting problem. A real product wants an explicit frame marker;
// out of scope for this bench proof.

#define SPI_PORT     spi0
#define PIN_MOSI     20
#define PIN_CS       21
#define PIN_SCK      22
#define PIN_MISO     23
#define ROW_RX_BYTES 480  // 240 source RGB565 pixels, 2 bytes each, low byte first (matches badge_app/app.py's bytearray packing)

static void spi0_slave_init(void) {
    spi_init(SPI_PORT, 1000 * 1000);  // nominal only -- slave derives timing from the badge's SCK
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
}

// CS-framed polling receive, with a SHORT internal timeout purely to
// guarantee this function always returns promptly (never a hang, at
// either phase) -- how long ago the last SUCCESSFUL row arrived, for
// deciding "genuinely idle" vs. "brief gap between rows", is tracked by
// the CALLER on a wall clock, not by this function's own timeout value.
//
// Went through two other approaches first (2026-08-30, both on real
// hardware) before landing here:
//   1. A plain byte-counting poll with no CS-awareness at all: any
//      timeout mid-burst left stale bytes sitting in the RX FIFO, which
//      the NEXT call would treat as the start of a fresh row -- a
//      persistent byte-shift, once introduced, that never
//      self-corrected (confirmed on real hardware: streamed pixel data
//      was consistently misaligned from that point on).
//   2. A single call to the SDK's own spi_write_read_blocking() after
//      only a ONE-TIME, non-blocking CS peek beforehand (no ongoing
//      CS-awareness during the transfer itself): reused proven transfer
//      mechanics, but reopened almost the exact hang risk that motivated
//      moving off a single blocking call in the first place -- if CS
//      happens to go back high in the gap between the peek and the
//      actual transfer engaging (a real race, given how fast the badge
//      toggles CS between rows), the SDK call has no way to know and
//      just sits waiting forever for bytes that will never arrive.
//      Confirmed on real hardware: the whole main loop hung completely
//      (not even the wall-clock heartbeat LED kept blinking).
// This version keeps CS-awareness THROUGHOUT the byte-by-byte receive
// (not just as a one-time gate beforehand), so it can safely abort an
// incomplete burst instead of ever waiting on data that isn't coming,
// while ALSO explicitly resynchronizing to each fresh CS-low edge (fix
// #1's actual bug) rather than trusting stale FIFO content.
//
// 2026-08-31, real hardware: gpio_get(PIN_CS) read 0 on literally every
// single poll, including multi-second idle stretches with nothing
// plugged in at all -- so it is NOT reliably reflecting this pin's real
// level while PIN_CS is configured GPIO_FUNC_SPI on this hardware/SDK
// combination (the "input path reflects the pad regardless of function
// select" assumption from fix #2, above, does not hold here). Given
// that, the CS-low-wait loop above always fell straight through (0
// iterations), and the mid-burst "did CS go back high" abort check
// never fired either -- so that version was effectively an unconditional
// per-call "flush the FIFO, then drain up to 480 bytes with a timeout"
// running in a tight loop against a stream it was never actually
// synchronized to. Its rare successes (~5 out of ~6000 calls, captured
// on the console) only happened when a call's timing accidentally
// landed on a genuine row boundary; every other call's flush step threw
// away legitimately-in-flight bytes of whatever row was already
// mid-transfer, permanently shifting alignment for that row.
//
// The badge sends one row as a single continuous CS-low burst of
// exactly ROW_RX_BYTES bytes back-to-back (see _send_frame() in
// badge_app/app.py), with no filler bytes in the gaps between rows.
// That means the RX FIFO's byte stream is ALREADY naturally
// row-aligned as long as this side never discards a byte it has
// actually received and never fabricates one it hasn't -- no GPIO-level
// framing signal is needed at all. So: drop CS entirely from this
// function, never flush, and let `received` persist ACROSS calls
// (static) so a row that arrives a few bytes at a time (this function
// gets called from a tight main loop that also does other per-iteration
// work) still accumulates correctly instead of being reset. The
// deadline covers one whole call, not reset per byte: within a single
// CS-low burst, bytes arrive back-to-back at the SPI clock rate with no
// natural gaps (the badge never pauses mid-burst), so a real stall can
// only happen BETWEEN bursts -- one deadline per call already catches
// that. A per-byte reset was tried and removed (2026-08-31): each reset
// calls make_timeout_time_ms(), which reads a hardware timer -- cheap at
// the original 1MHz badge-side rate, but 20MHz (needed for 15fps, see
// badge_app/app.py) leaves only ~400ns/byte, and that timer read alone
// was eating a meaningful fraction of it. A single deadline per call
// removes that cost from the hot per-byte path entirely, at any speed.
#define SPI0_RECEIVE_INTERNAL_TIMEOUT_MS 50

static bool spi0_receive_row(uint8_t *rx_buf) {
    static size_t received = 0;  // persists across calls -- never reset except on a full row

    absolute_time_t deadline = make_timeout_time_ms(SPI0_RECEIVE_INTERNAL_TIMEOUT_MS);
    while (received < ROW_RX_BYTES) {
        if (spi_is_readable(SPI_PORT)) {
            rx_buf[received++] = (uint8_t)spi_get_hw(SPI_PORT)->dr;
        } else if (time_reached(deadline)) {
            return false;  // genuine silence -- come back later, `received` is preserved
        }
    }
    received = 0;
    return true;
}

// ----------------------------------------------------------------------------

// Heartbeat/activity LED via the Metro's onboard NeoPixel (GPIO25) --
// GPIO23 (PICO_DEFAULT_LED_PIN on this board) is unusable here, already
// claimed as SPI0's MISO, and a plain GPIO needs an LED wired up to be
// visible at all, whereas the NeoPixel is already on the board. Uses
// pico-sdk's own official ws2812.pio program (BSD-3, Raspberry Pi) via
// its documented ws2812_program_init() helper, not a hand-rolled driver.
// RED blinks at 1Hz on a real wall clock (not tied to row/frame
// activity, so it keeps blinking with nothing plugged in at all --
// spi0_receive_row()'s timeout is what makes that possible); GREEN
// briefly overrides it whenever a new frame actually completes, so red
// = alive, green = link active.
#define PIN_NEOPIXEL   25
#define NEOPIXEL_PIO   pio0
#define NEOPIXEL_SM    0

static inline void neopixel_put(uint8_t r, uint8_t g, uint8_t b) {
    // ws2812.pio shifts out the top 24 bits MSB-first as G,R,B (one byte
    // each) -- the order the WS2812 protocol itself expects on the wire.
    uint32_t grb = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    pio_sm_put_blocking(NEOPIXEL_PIO, NEOPIXEL_SM, grb);
}

static void neopixel_init(void) {
    uint offset = pio_add_program(NEOPIXEL_PIO, &ws2812_program);
    ws2812_program_init(NEOPIXEL_PIO, NEOPIXEL_SM, offset, PIN_NEOPIXEL, 800000.0f, false);
    neopixel_put(0, 0, 0);  // off until the main loop starts toggling it
}

// Runs on core 1 (see main()'s comment for why) -- everything past
// hstx_dvi_init() is interrupt-driven (dma_irq_handler), so core 1 has
// nothing further to do once it returns.
static void core1_video_entry(void) {
    hstx_dvi_init();
    for (;;) {
        tight_loop_contents();
    }
}

int main(void) {
    // Section 5 mitigation #1: I2C0 target up before anything else,
    // including clock reconfiguration -- see eeprom_i2c.h.
    eeprom_i2c_start();

    // clk_hstx follows clk_sys undivided by default; 640x480@60 needs
    // clk_sys=126MHz -- see phase0-dvi/README.md for the derivation.
    set_sys_clock_khz(126000, true);

    // RP2350 core1-launch voltage erratum (spaceagon-display-tap's
    // firmware/phase3-merged/README.md, "Bug 1"): multicore_launch_core1()
    // can hang -- core 1 stuck in a bootrom FIFO-status-polling loop,
    // never reaching its entry function -- at stock 1.10V, independent
    // of target clock frequency. Bump before launching core 1, with a
    // short settle delay.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);

    build_row_table();
    fill_test_card(back_index());  // EMF bars, shown until the badge's first full frame arrives -- see ever_received_frame below
    back_buffer_ready = true;
    spi0_slave_init();
    neopixel_init();

    // Video (HSTX/DMA setup + dma_irq_handler) now runs on its own core;
    // this core's main loop below keeps the SPI0 receive path. Matches
    // the core assignment spaceagon-display-tap's firmware/phase3-merged
    // found necessary on real RP2350 hardware ("Bug 2" in that project's
    // README): the opposite assignment (video on core 0, the
    // receive/decode job launched onto core 1) reproducibly broke video
    // on that project's identical silicon -- confirmed fixed by swapping
    // which job runs on which core, root cause never pinned down at the
    // register level. Applied here pre-emptively, before this firmware
    // hits the same wall. This also removes hstx_dvi_init()'s original
    // need to fight the SPI0 polling loop for core time via a DMA_IRQ_0
    // priority boost (see that function's own history) -- they no longer
    // share a core at all.
    multicore_launch_core1(core1_video_entry);

    uint8_t row_rx[ROW_RX_BYTES];
    uint row_index = 0;
    int recv_buf = back_index();  // which buffer the in-progress frame's rows land in -- refreshed at each frame's row 0, see below

    // Heartbeat: RED, 1Hz, wall-clock-timed (not tied to row/frame
    // arrival -- this must keep blinking even with no badge connected at
    // all, which the old row-count-based version couldn't do since it
    // never ran without SPI activity). GREEN briefly overrides it for
    // ~80ms whenever a new frame actually completes, so red = alive,
    // green = link active, distinguishable at a glance.
    bool heartbeat_red_on = false;
    absolute_time_t next_red_toggle = make_timeout_time_ms(500);
    absolute_time_t green_flash_until = nil_time;

    // Idle detection is wall-clock-timed, not iteration-counted --
    // spi0_receive_row() blocks internally (spinning on the RX FIFO)
    // until either a full row completes or SPI0_RECEIVE_INTERNAL_TIMEOUT_MS
    // of genuine byte-to-byte silence passes, so it does NOT return
    // near-instantly on every miss -- while truly idle, the outer loop
    // only comes back around roughly once per that internal timeout. A
    // gap between successful rows under IDLE_AFTER_MS is unremarkable
    // (MicroPython GC pauses, background WiFi/BT servicing, scheduler
    // jitter between one row and the next); only sustained silence past
    // that is "actually idle" and falls back to static.
    #define IDLE_AFTER_MS 500
    absolute_time_t last_row_time = get_absolute_time();

    // Set the first time a full frame lands (row_index wraps back to 0
    // below) -- gates which fallback pattern genuine idle shows.
    // Before it: EMF test card (this hexpansion is up, just waiting for
    // the badge's app to start). After it: static noise (the link WAS
    // working and has since gone quiet) -- see spi0_receive_row's else
    // branch below. Matches spaceagon-display-tap's own frame_ready
    // semantics ("stop overwriting framebuf with the synthetic
    // bars/logo pattern" once real content has ever arrived).
    bool ever_received_frame = false;
    // Set on the very first successful got_row, before ever_received_frame --
    // gates the EMF test card blink below (must stop the instant real
    // bytes start arriving, not only once a full frame completes; see
    // that block's own comment).
    bool ever_received_any_row = false;
    bool testcard_logo_on = false;
    absolute_time_t next_testcard_blink = make_timeout_time_ms(700);

#ifdef DEBUG_SERIAL
    stdio_init_all();
    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        sleep_ms(50);
    }
    printf("\ntestcard DEBUG_SERIAL build: entering SPI0 receive loop\n");
    uint32_t rows_received = 0;
#endif

    for (;;) {
        bool got_row = spi0_receive_row(row_rx);

        if (got_row) {
#ifdef DEBUG_SERIAL
            rows_received++;
            if (rows_received <= 5 || (rows_received % 240) == 0) {
                printf("row_rx #%lu: first 8: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       (unsigned long)rows_received,
                       row_rx[0], row_rx[1], row_rx[2], row_rx[3],
                       row_rx[4], row_rx[5], row_rx[6], row_rx[7]);
            }
#endif
            last_row_time = get_absolute_time();
            ever_received_any_row = true;  // stop the EMF test card blink, even mid-frame -- see its own comment below

            if (row_index == 0) {
                // Starting a new frame's reception: make sure the
                // PREVIOUS frame's flip has actually happened (dma_irq_
                // handler clears back_buffer_ready exactly when it does
                // the swap, at the next vblank after it was set) before
                // grabbing a fresh back_index() to write this one into
                // -- otherwise this could start overwriting a buffer
                // that's still waiting to become front. In practice this
                // never actually spins: a full frame takes ~1s+ to
                // receive at this app's own pace, many multiples of one
                // ~16ms vblank period, so the previous flip is always
                // long done by the time reception loops back to row 0.
                while (back_buffer_ready) {
                    tight_loop_contents();
                }
                recv_buf = back_index();
            }

            // Straight into the back buffer, doubled + BGR-corrected --
            // no separate staging copy needed (frame_staging is gone):
            // DMA never touches recv_buf until back_buffer_ready flips
            // it to front, so there's nothing to race here.
            uint16_t *src16 = (uint16_t *)(void *)row_rx;
            uint32_t *dst32 = framebuf[recv_buf][row_index];
            for (int i = 0; i < DBL_WORDS; i++) {
                uint32_t px = undo_madctl_bgr(src16[i]);
                dst32[i] = px | (px << 16);
            }

            row_index = (row_index + 1) % SRC_ROWS;
            if (row_index == 0) {  // a full frame just landed
                ever_received_frame = true;  // stop showing the EMF test card forever, even across future idle gaps
                back_buffer_ready = true;
                green_flash_until = make_timeout_time_ms(80);
            }
        } else {
            sleep_us(200);  // idle -- don't busy-spin the CS peek needlessly

            bool genuinely_idle =
                absolute_time_diff_us(last_row_time, get_absolute_time()) >= IDLE_AFTER_MS * 1000;

#ifdef DEBUG_SERIAL
            static uint32_t misses = 0;
            misses++;
            if (misses <= 10 || (misses % 2000) == 0) {
                printf("spi0_receive_row missed (#%lu), genuinely_idle=%d\n",
                       (unsigned long)misses, (int)genuinely_idle);
            }
#endif

            if (genuinely_idle && ever_received_frame) {
                // "No signal" static -- writes straight into the back
                // buffer (fill_static_noise() picks back_index() and
                // sets back_buffer_ready itself; see its own comment),
                // no per-row gating needed since DMA never touches the
                // back buffer at all. Only shown once a real frame has
                // landed before -- until then, the EMF test card below
                // owns it instead (this is "signal lost", not "no
                // signal yet").
                row_index = 0;  // next real row, once streaming resumes, starts a fresh frame cleanly
                fill_static_noise();
            }
            // else: recent activity -- not idle yet, just a gap between rows, nothing to do
        }

        // EMF bars/logo test card: blinks (bars <-> bars+logo, 700ms,
        // wall-clock-timed like the heartbeat below) until the badge's
        // link goes live -- gated on ever_received_any_row, not
        // ever_received_frame: it must stop the instant real bytes start
        // arriving, not only once a full frame completes, otherwise this
        // block and the receive loop above would both be writing into
        // the SAME back buffer during that first, still-incomplete
        // frame (recv_buf is snapshotted once at that frame's start --
        // see above) and stomp each other. Runs every loop iteration
        // (not gated on genuinely_idle) so it keeps blinking right up
        // until that first byte, covering the entire "badge not
        // connected yet" window.
        if (!ever_received_any_row && time_reached(next_testcard_blink)) {
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
            next_red_toggle = delayed_by_ms(next_red_toggle, 500);  // fixed 500ms cadence, no drift
        }
        bool green_active = !is_nil_time(green_flash_until) && !time_reached(green_flash_until);
        if (green_active) {
            neopixel_put(0, 20, 0);  // dim green: link active (new frame just landed)
        } else {
            neopixel_put(heartbeat_red_on ? 20 : 0, 0, 0);  // dim red, 1Hz: alive
        }
    }
}
