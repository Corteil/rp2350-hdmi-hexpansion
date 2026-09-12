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
#include "hardware/spi.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
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

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS_REPEAT | MODE_H_ACTIVE_PIXELS,
    0x00000000u,  // mutated live: current diagnostic-state colour, packed as two RGB565 pixels
};
#define VACTIVE_LINE_COLOUR_IDX 7

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
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
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
    spi_init(spi0, 1000 * 1000);  // nominal only -- slave derives timing from the badge's own SCK
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

    spi_rx_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(spi_rx_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, false));  // false = RX
    channel_config_set_ring(&c, true, SPI_RING_BITS);
    dma_channel_configure(
        spi_rx_dma_chan, &c,
        spi_rx_ring, &spi_get_hw(spi0)->dr,
        0xFFFFFFFFu,
        true
    );
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

static void cs_edge_irq_handler(uint gpio, uint32_t events) {
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

int main(void) {
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    set_sys_clock_khz(126000, true);

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);

    set_status_colour(state_colours[STATE_BOOT]);
    spi0_slave_init();

    gpio_set_irq_enabled_with_callback(PIN_CS, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &cs_edge_irq_handler);

    multicore_launch_core1(core1_video_entry);

    stdio_init_all();
    sleep_ms(200);
    printf("\nmirror_debug: entering diagnostic loop (SPI0 mode 0, TDHD header, CS-edge tracking)\n");

    uint32_t last_printed_fs = 0, last_printed_fe = 0, last_printed_hdr = 0;
    absolute_time_t next_periodic_print = make_timeout_time_ms(2000);

    for (;;) {
        spi0_process_ring();

        uint32_t fs = frame_start_count, fe = frame_end_count, tb = total_bytes_seen, hm = header_match_count;

        if (fs > 0) advance_state(STATE_CS_EDGE_SEEN);
        if (tb > 0) advance_state(STATE_BYTES_SEEN);
        if (hm > 0) advance_state(STATE_HEADER_MATCHED);

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
                   "bytes_last_frame=%lu bytes_since_cs_low=%lu state=%d\n",
                   (unsigned long)fs, (unsigned long)fe, (unsigned long)tb, (unsigned long)hm,
                   (unsigned long)bytes_seen_last_frame, (unsigned long)bytes_since_cs_low, best_state);
        }

        sleep_us(200);
    }
}
