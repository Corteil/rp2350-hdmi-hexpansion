// mirror_debug: a purpose-built diagnostic firmware for the Hexi-GFX
// mirror-protocol investigation (see hexigfx-mirror-status memory for the
// full history). Built 2026-09-12 out of two pieces:
//
// 1. The known-good HSTX/DVI video path -- the same 640x480@60 timing and
//    colour-correct RGB565 TMDS config firmware/testcard/src/main.c
//    already proved on this exact bench board, simplified to a single
//    solid-colour full-screen fill (no pillarboxing/circle mask, no real
//    framebuffer -- this tool only needs to SHOW A STATE, not mirror
//    content), so the monitor itself becomes a live diagnostic readout
//    with nothing else needed.
//
// 2. A FRESH SPI0 slave receiver written against hazanjon's REAL,
//    CURRENT wire protocol (components/flow3r_bsp/flow3r_bsp_display_mirror.c
//    in badge-2024-software), not the older badge_app/app.py protocol
//    firmware/testcard/src/main.c's own receiver was actually built
//    against. Two protocol-level differences matter here:
//      - hazanjon's master hardcodes `spi_device_interface_config_t.mode
//        = 0` (CPOL=0, CPHA=0) -- testcard/src/main.c currently uses
//        SPI_CPOL_0/SPI_CPHA_1, an empirical choice from testing the OLD
//        protocol's own sender, never revalidated against this one.
//      - hazanjon's driver sends a real 4-byte magic header ("TDHD" for
//        the HDMI driver specifically) once per frame, immediately after
//        manually asserting CS low, then the raw pixel payload, all in
//        one continuous CS-low burst, deasserting CS only once the whole
//        frame is sent -- genuine hardware CS framing exists on this
//        protocol, unlike the old one. testcard/src/main.c's receiver
//        instead free-scans for an unrelated 8-byte 0xA55AA55AA55AA55A
//        marker that hazanjon's real driver never sends at all -- even if
//        that receiver started seeing bytes, it could never frame-sync
//        against this driver's real output.
//
// This firmware tracks FOUR independent diagnostic signals so a genuine
// electrical/timing failure (no CS edges, or CS edges but zero clocked
// bits) can be told apart from a protocol-framing failure (bytes arrive
// but never spell "TDHD"):
//   - frame_start_count / frame_end_count: raw GPIO edge counts on the CS
//     pin itself (falling/rising), independent of the SPI peripheral --
//     these fire even if the SPI hardware never clocks in a single valid
//     bit, so they isolate "does this pin even toggle the way a real
//     per-frame CS burst should" from everything downstream of it.
//   - total_bytes_seen: raw bytes the SPI0 peripheral actually clocked in,
//     via the same DMA-ring-buffer technique testcard/src/main.c already
//     proved reliable at high baudrates.
//   - header_match_count: number of times the literal 4-byte "TDHD"
//     pattern was seen anywhere in the byte stream (a free-running scan,
//     in case CS-relative alignment turns out not to matter).
//   - bytes_seen_this_frame: total_bytes_seen delta across the most
//     recent CS-low burst specifically (captured at the CS rising edge)
//     -- if CS toggles convincingly but this stays 0 every time, that
//     points squarely at an SPI-mode/clock mismatch, not a wiring or
//     framing problem.
//
// The monitor shows the single BEST diagnostic state ever reached (never
// regresses, so a momentary success isn't missed if you glance at the
// screen after the fact): boot->dark grey, any CS edge->blue, any raw
// byte->yellow, "TDHD" ever matched->green. Full counters and live state
// are also on the UART debug bridge (/dev/ttyACM1, 115200 8N1) for
// detailed diagnosis.

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"
#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------------
// DVI constants (standard CEA-861 VIC 1, 640x480@60) -- identical to
// firmware/testcard/src/main.c and the patched pico-examples HSTX demo.

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

#define MODE_V_TOTAL_LINES ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Video: single solid colour, full 640x480 active area, no framebuffer at
// all -- HSTX_CMD_TMDS_REPEAT pops one pixel word from the command list
// and repeats it TMDS-encoded for the full active width, same trick the
// sync levels above already use for RAW_REPEAT. vactive_line's own pixel
// word (last element) is the only thing mutated live, from the main loop,
// whenever the diagnostic state advances.

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

// A light border around the state-colour fill, so a genuinely-black/dark
// diagnostic state (e.g. STATE_BOOT's dark grey) can never be mistaken for
// "no signal" -- the border is always bright regardless of state. Inset
// MARGIN_PX/MARGIN_LINES from the true screen edge (rather than flush
// against it) since some monitors/capture chains crop or blank the very
// edge pixels.
#define MARGIN_PX    10
#define MARGIN_LINES 10
#define BORDER_PX    8
#define BORDER_LINES 8
#define BORDER_COLOUR_WORD (0xFFFFu | (0xFFFFu << 16))  // white, both packed pixels
#define BLACK_COLOUR_WORD  (0x0000u | (0x0000u << 16))

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS_REPEAT | MARGIN_PX,
    BLACK_COLOUR_WORD,  // left margin
    HSTX_CMD_TMDS_REPEAT | BORDER_PX,
    BORDER_COLOUR_WORD,  // left border
    HSTX_CMD_TMDS_REPEAT | (MODE_H_ACTIVE_PIXELS - 2 * MARGIN_PX - 2 * BORDER_PX),
    0x00000000u,  // mutated live: current diagnostic-state colour, packed as two RGB565 pixels
    HSTX_CMD_TMDS_REPEAT | BORDER_PX,
    BORDER_COLOUR_WORD,  // right border
    HSTX_CMD_TMDS_REPEAT | MARGIN_PX,
    BLACK_COLOUR_WORD,  // right margin
};
#define VACTIVE_LINE_COLOUR_IDX 11

// Solid bright row (inset by MARGIN_PX left/right), used for the
// BORDER_LINES rows above/below the fill to complete the border rectangle.
static uint32_t vactive_border_row[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS_REPEAT | MARGIN_PX,
    BLACK_COLOUR_WORD,
    HSTX_CMD_TMDS_REPEAT | (MODE_H_ACTIVE_PIXELS - 2 * MARGIN_PX),
    BORDER_COLOUR_WORD,
    HSTX_CMD_TMDS_REPEAT | MARGIN_PX,
    BLACK_COLOUR_WORD,
};

// Solid black row, used for the outermost MARGIN_LINES rows top/bottom.
static uint32_t vactive_margin_row[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS_REPEAT | MODE_H_ACTIVE_PIXELS,
    BLACK_COLOUR_WORD,
};

static void set_status_colour(uint16_t rgb565) {
    uint32_t word = (uint32_t)rgb565 | ((uint32_t)rgb565 << 16);
    vactive_line[VACTIVE_LINE_COLOUR_IDX] = word;
}

// ----------------------------------------------------------------------------
// DMA: two channels ping-ponging via chain_to -- one HSTX command list per
// scanline, unchanged structure from the plain pico-examples HSTX demo
// (this tool needs none of testcard's pillarbox/double-buffer complexity).

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;

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
    } else {
        uint v_active_start = MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH;
        uint rel = v_scanline - v_active_start;
        if (rel < MARGIN_LINES || rel >= MODE_V_ACTIVE_LINES - MARGIN_LINES) {
            ch->read_addr = (uintptr_t)vactive_margin_row;
            ch->transfer_count = count_of(vactive_margin_row);
        } else if (rel < MARGIN_LINES + BORDER_LINES ||
                   rel >= MODE_V_ACTIVE_LINES - MARGIN_LINES - BORDER_LINES) {
            ch->read_addr = (uintptr_t)vactive_border_row;
            ch->transfer_count = count_of(vactive_border_row);
        } else {
            ch->read_addr = (uintptr_t)vactive_line;
            ch->transfer_count = count_of(vactive_line);
        }
    }
    v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

static void hstx_dvi_init(void) {
    // Colour-correct RGB565 TMDS config, byte-for-byte from
    // firmware/testcard/src/main.c's own hstx_dvi_init() (proven on real
    // hardware -- see that file's comment for the full derivation).
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
// SPI0 slave: same pins/DMA-ring-drain technique firmware/testcard/src/main.c
// already proved reliable at high baudrate, but configured for hazanjon's
// REAL protocol -- SPI mode 0 (his master's hardcoded
// `spi_device_interface_config_t.mode = 0`), and scanning for his real
// 4-byte "TDHD" header instead of the old, unrelated 8-byte 0xA55A marker.

#define PIN_MOSI 20
#define PIN_CS   21
#define PIN_SCK  22
#define PIN_MISO 23

#define SPI_RING_BITS 12
#define SPI_RING_SIZE (1u << SPI_RING_BITS)
static uint8_t spi_rx_ring[SPI_RING_SIZE] __attribute__((aligned(SPI_RING_SIZE)));
static uint32_t spi_ring_read_pos = 0;
static uint spi_rx_dma_chan;

// "TDHD" as hazanjon's driver sends it (MSB-first per byte, T then D then
// H then D) packs into this 32-bit rolling window once all 4 bytes have
// shifted through -- see this file's own top comment for the byte-by-byte
// derivation.
#define TDHD_MARKER_VALUE 0x54444844u
static uint32_t marker_window = 0;

static volatile uint32_t total_bytes_seen = 0;
static volatile uint32_t header_match_count = 0;
static volatile uint32_t bytes_since_cs_low = 0;      // running count within the current/most-recent CS-low burst
static volatile uint32_t bytes_seen_last_frame = 0;   // snapshot of the above, taken at the CS rising edge
static volatile uint32_t header_matched_at_offset = 0xFFFFFFFFu;  // byte offset within the CS-low burst where TDHD last matched (0xFFFFFFFF = never, within the CURRENT burst)

static void spi0_slave_init(void) {
    spi_init(spi0, 10 * 1000 * 1000);  // hazanjon's real default baudrate; nominal only -- slave derives timing from the badge's own SCK
    spi_set_slave(spi0, true);
    // Mode 0 (CPOL=0, CPHA=0) -- deliberately matching hazanjon's own
    // hardcoded master config exactly, unlike testcard/src/main.c's
    // CPHA_1 (an empirical choice from the OLDER, different protocol's
    // sender -- see this file's own top comment).
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    // RP2350 GPIOs reset with a pull-down enabled by default, and
    // gpio_set_function() alone doesn't touch that (it's a separate PADS
    // register from the function-select mux). PIN_MOSI/PIN_CS are also the
    // board's STEMMA QT I2C0 SDA/SCL pins, which may carry an actual
    // on-board hardware pull-up in addition. A real push-pull master
    // shouldn't need any pull here -- disable them explicitly to rule
    // stray loading out entirely.
    gpio_disable_pulls(PIN_MOSI);
    gpio_disable_pulls(PIN_CS);
    gpio_disable_pulls(PIN_SCK);
    gpio_disable_pulls(PIN_MISO);

    spi_rx_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(spi_rx_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, false));  // false = RX
    channel_config_set_ring(&c, true, SPI_RING_BITS);
    // High priority: matches hstx_dvi_init()'s two video DMA channels,
    // which are also boosted and stream continuously. Tried in isolation as
    // a fix for the exactly-1-byte-per-frame symptom below -- on its own it
    // did NOT fix it (retested on real hardware, no change), but left in as
    // cheap insurance against a real, separate starvation risk while the
    // actual fix (the overrun clear right below) is verified.
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(
        spi_rx_dma_chan, &c,
        spi_rx_ring, &spi_get_hw(spi0)->dr,
        0xFFFFFFFFu,
        true
    );

    // Clear any latched receive-overrun/timeout condition before enabling
    // the DMA request line (2026-09-13 fix attempt): on real hardware this
    // firmware consistently captured EXACTLY one byte per CS-low burst,
    // every single frame, hundreds of frames in a row -- badge-side
    // ESP_LOG confirmed the full header+payload genuinely goes out over the
    // wire every time, so the truncation is on this (RP2350) side. That
    // signature -- one byte captured, then permanently stuck at one byte
    // forever after, not zero and not a live race each time -- matches a
    // classic SPI-slave gotcha: an unhandled RORIS (receive overrun) latched
    // very early (e.g. before this init function finishes arming the DMA
    // channel) can block the hardware FIFO from accepting further bytes
    // until explicitly cleared, even though DMA is otherwise configured
    // correctly. RORIC/RTIC are write-to-clear bits in SSPICR.
    hw_set_bits(&spi_get_hw(spi0)->icr, SPI_SSPICR_RORIC_BITS | SPI_SSPICR_RTIC_BITS);

    spi_get_hw(spi0)->dmacr = SPI_SSPDMACR_RXDMAE_BITS;
}

static void spi0_process_ring(void) {
    uint32_t write_addr = dma_channel_hw_addr(spi_rx_dma_chan)->write_addr;
    uint32_t write_pos = (uint32_t)(write_addr - (uintptr_t)spi_rx_ring) & (SPI_RING_SIZE - 1);

    while (spi_ring_read_pos != write_pos) {
        uint8_t b = spi_rx_ring[spi_ring_read_pos];
        spi_ring_read_pos = (spi_ring_read_pos + 1) & (SPI_RING_SIZE - 1);
        total_bytes_seen++;
        bytes_since_cs_low++;

        marker_window = (marker_window << 8) | b;
        if (marker_window == TDHD_MARKER_VALUE) {
            header_match_count++;
            if (header_matched_at_offset == 0xFFFFFFFFu) {
                header_matched_at_offset = bytes_since_cs_low - 4;  // offset of the "T", not the byte just consumed
            }
        }
    }
}

// ----------------------------------------------------------------------------
// CS edge GPIO IRQ: pure electrical frame-start/frame-end detection,
// independent of whether the SPI peripheral itself ever clocks in a
// single valid bit -- see this file's own top comment for why this
// matters as its own, separate diagnostic signal.

static volatile uint32_t frame_start_count = 0;
static volatile uint32_t frame_end_count = 0;

// Per-pin raw toggle counters (2026-09-13), added specifically to verify
// mirror_debug.c's own PIN_MOSI/PIN_CS/PIN_SCK/PIN_MISO assignment against
// the actual current badge-port-4 wiring -- three software-side fixes for
// the "exactly 1 byte per frame" mystery (DMA priority, SPI overrun clear,
// lower baudrate) all failed to change anything, so this checks the
// wiring/pin-role-assignment itself instead of guessing further in
// software. Works the same way the CS-edge counter always has: GPIO
// edge-detect monitors the raw pad state independent of GPIO_FUNC_SPI, so
// this doesn't require reconfiguring any pin's function.
static volatile uint32_t pin_toggle_count[4] = {0, 0, 0, 0};  // indexed by which of the 4 pins below

static void cs_edge_irq_handler(uint gpio, uint32_t events) {
    if (gpio == PIN_MOSI) pin_toggle_count[0]++;
    else if (gpio == PIN_CS) pin_toggle_count[1]++;
    else if (gpio == PIN_SCK) pin_toggle_count[2]++;
    else if (gpio == PIN_MISO) pin_toggle_count[3]++;

    if (gpio != PIN_CS) return;
    if (events & GPIO_IRQ_EDGE_FALL) {
        frame_start_count++;
        bytes_since_cs_low = 0;
        marker_window = 0;
        header_matched_at_offset = 0xFFFFFFFFu;
    }
    if (events & GPIO_IRQ_EDGE_RISE) {
        frame_end_count++;
        bytes_seen_last_frame = bytes_since_cs_low;
    }
}

// ----------------------------------------------------------------------------
// Diagnostic state -> monitor colour, monotonic (never regresses once a
// higher state is reached -- see this file's own top comment).

enum {
    STATE_BOOT = 0,
    STATE_CS_EDGE_SEEN,
    STATE_BYTES_SEEN,
    STATE_HEADER_MATCHED,
};
static volatile int best_state = STATE_BOOT;

static const uint16_t state_colours[] = {
    [STATE_BOOT]            = 0x2104,  // dark grey: nothing seen yet
    [STATE_CS_EDGE_SEEN]    = 0x001F,  // blue: CS toggled at least once
    [STATE_BYTES_SEEN]      = 0xFFE0,  // yellow: SPI0 peripheral clocked in at least one real byte
    [STATE_HEADER_MATCHED]  = 0x07E0,  // green: hazanjon's real "TDHD" header was seen
};

static void advance_state(int new_state) {
    if (new_state > best_state) {
        best_state = new_state;
        set_status_colour(state_colours[best_state]);
    }
}

// ----------------------------------------------------------------------------
// Heartbeat/activity LED via the Metro's onboard NeoPixel (GPIO25) --
// ported directly from testcard/src/main.c's own copy (see that file's
// comment). RED blinks at 1Hz on a real wall clock, independent of the
// on-monitor diagnostic colour -- this firmware never had a NeoPixel
// heartbeat at all until now, which made "did it actually crash" vs. "it's
// just showing a dark diagnostic colour" ambiguous from the monitor alone.
// GREEN briefly overrides it whenever the "TDHD" header is ever matched.
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
    sleep_ms(50);

    set_status_colour(state_colours[STATE_BOOT]);
    spi0_slave_init();

    // Nothing but the bare minimum happens between the vreg settle delay
    // and multicore_launch_core1() -- both the CS-edge GPIO IRQ and (as of
    // this build) neopixel_init()'s PIO setup are deferred until AFTER the
    // launch. On real hardware, enabling the GPIO IRQ here (before launch)
    // consistently left core1 stuck in the bootrom FIFO-handoff loop
    // (RP2350 core1-launch erratum, see testcard/src/main.c's own comment
    // on it) -- confirmed via GDB, core1's PC parked in an unresolved RAM
    // address outside any loaded symbol. testcard/src/main.c has no such
    // extra setup active on core0 during its own (reliable) core1 launch at
    // all. Suspected cause: anything that can preempt or briefly block core0
    // -- an IRQ, or PIO program loading/SM config -- disrupting
    // multicore_launch_core1()'s SIO-FIFO-based handoff sequence if it lands
    // mid-handshake.
    multicore_launch_core1(core1_video_entry);

    neopixel_init();
    gpio_set_irq_enabled_with_callback(PIN_CS, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &cs_edge_irq_handler);
    gpio_set_irq_enabled(PIN_MOSI, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(PIN_SCK, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(PIN_MISO, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);

    stdio_init_all();
    sleep_ms(200);
    printf("\nmirror_debug: entering diagnostic loop (SPI0 mode 0, TDHD header, CS-edge tracking)\n");

    uint32_t last_printed_fs = 0, last_printed_fe = 0, last_printed_hdr = 0;
    uint32_t last_hm_for_flash = 0;
    absolute_time_t next_periodic_print = make_timeout_time_ms(2000);
    bool heartbeat_red_on = false;
    absolute_time_t next_red_toggle = make_timeout_time_ms(500);
    absolute_time_t green_flash_until = nil_time;

    for (;;) {
        spi0_process_ring();

        uint32_t fs = frame_start_count, fe = frame_end_count, tb = total_bytes_seen, hm = header_match_count;

        if (fs > 0) advance_state(STATE_CS_EDGE_SEEN);
        if (tb > 0) advance_state(STATE_BYTES_SEEN);
        if (hm > 0) advance_state(STATE_HEADER_MATCHED);

        if (hm != last_hm_for_flash) {
            last_hm_for_flash = hm;
            green_flash_until = make_timeout_time_ms(80);
        }
        if (time_reached(next_red_toggle)) {
            heartbeat_red_on = !heartbeat_red_on;
            next_red_toggle = delayed_by_ms(next_red_toggle, 500);
        }
        if (!is_nil_time(green_flash_until) && !time_reached(green_flash_until)) {
            neopixel_put(0, 20, 0);
        } else {
            neopixel_put(heartbeat_red_on ? 20 : 0, 0, 0);
        }

        // Print immediately on any new CS edge or header match -- these
        // are rare, high-value events worth seeing the instant they
        // happen, not just on the periodic heartbeat below.
        if (fs != last_printed_fs || fe != last_printed_fe || hm != last_printed_hdr) {
            printf("EVENT frame_start=%lu frame_end=%lu total_bytes_seen=%lu header_match=%lu "
                   "bytes_last_frame=%lu marker_window=%08lx state=%d\n",
                   (unsigned long)fs, (unsigned long)fe, (unsigned long)tb, (unsigned long)hm,
                   (unsigned long)bytes_seen_last_frame, (unsigned long)marker_window, best_state);
            last_printed_fs = fs;
            last_printed_fe = fe;
            last_printed_hdr = hm;
        }

        if (time_reached(next_periodic_print)) {
            next_periodic_print = delayed_by_ms(next_periodic_print, 2000);
            printf("heartbeat frame_start=%lu frame_end=%lu total_bytes_seen=%lu header_match=%lu "
                   "bytes_last_frame=%lu bytes_since_cs_low=%lu state=%d "
                   "pin_toggles[MOSI=%d,CS=%d,SCK=%d,MISO/DC=%d]=%lu,%lu,%lu,%lu\n",
                   (unsigned long)fs, (unsigned long)fe, (unsigned long)tb, (unsigned long)hm,
                   (unsigned long)bytes_seen_last_frame, (unsigned long)bytes_since_cs_low, best_state,
                   PIN_MOSI, PIN_CS, PIN_SCK, PIN_MISO,
                   (unsigned long)pin_toggle_count[0], (unsigned long)pin_toggle_count[1],
                   (unsigned long)pin_toggle_count[2], (unsigned long)pin_toggle_count[3]);
        }

        sleep_us(200);
    }
}
