// C4 / A2: emulated hexpansion identification EEPROM over I2C0 target
// mode -- see eeprom_i2c.h and ../../README.md sections 1.4/5.
//
// I2C slave ISR technique (raw register access, ISR runs from RAM,
// 2-byte address state machine) is adapted from sammachin/rp2040-hexpansion
// (MIT), a real working THEX-header EEPROM emulator whose header layout
// independently cross-validates this design's own section 1.4 exactly.
// Not a verbatim copy -- ported from Arduino/RP2040 to bare pico-sdk C
// targeting RP2350's I2C0, using pico-sdk's own gpio_set_function()/
// i2c_init() for setup rather than hand-rolled register pokes for that
// part (only the slave-mode con/sar/enable overrides and the ISR itself
// need raw register access, since pico-sdk's own API has no built-in
// slave/target mode).

#include "eeprom_i2c.h"

#include <string.h>
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/i2c.h"
#include "pico/unique_id.h"

// Wired for this bench test only (see ../../README.md's A2 section) --
// the real product design keeps I2C0 target on GPIO24/25 (section 4.1);
// GPIO4/5 here are a stand-in chosen for this specific Metro board's
// pin accessibility, not a design change.
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define I2C_HW i2c0

#define HEX_I2C_ADDR   0x50
#define HEX_EEPROM_SIZE (64 * 1024)
#define HEX_FS_OFFSET   64
#define HEX_PAGE_SIZE   64

static uint8_t eeprom[HEX_EEPROM_SIZE];
static volatile uint16_t addr = 0;
static volatile uint8_t addr_hi = 0;
static volatile enum { S_ADDR_HI, S_ADDR_LO, S_DATA } rx_state = S_ADDR_HI;

static volatile uint32_t read_count = 0;
static volatile uint32_t write_count = 0;
static volatile bool header_read = false;

static uint8_t compute_checksum(const uint8_t *hdr) {
    uint8_t s = 0x55;
    for (int i = 1; i <= 30; i++) {
        s ^= hdr[i];
    }
    return s;
}

// Header layout per README section 1.4 (`<4s4sHHIHHH9s` + 1 checksum
// byte). vid/pid are placeholders (0x0000) -- section 1.4 flags
// requesting real values from the badge team ("UHB-IF") as a "do this
// early" item; not done yet, so this board will not present a real
// assigned identity until that happens.
static void build_header(void) {
    uint8_t *h = eeprom;
    memset(h, 0xFF, 32);

    memcpy(&h[0], "THEX", 4);
    memcpy(&h[4], "2026", 4);

    h[8]  = HEX_FS_OFFSET & 0xFF;
    h[9]  = (HEX_FS_OFFSET >> 8) & 0xFF;

    h[10] = HEX_PAGE_SIZE & 0xFF;
    h[11] = (HEX_PAGE_SIZE >> 8) & 0xFF;

    uint32_t total = HEX_EEPROM_SIZE;
    h[12] = total & 0xFF;
    h[13] = (total >> 8) & 0xFF;
    h[14] = (total >> 16) & 0xFF;
    h[15] = (total >> 24) & 0xFF;

    // Placeholder VID/PID -- see comment above.
    uint16_t vid = 0x0000;
    uint16_t pid = 0x0000;
    h[16] = vid & 0xFF;
    h[17] = (vid >> 8) & 0xFF;
    h[18] = pid & 0xFF;
    h[19] = (pid >> 8) & 0xFF;

    // unique_id: low 16 bits of the RP2350's own factory-programmed
    // 64-bit unique ID (via pico_unique_id), so distinct boards don't
    // collide even with placeholder VID/PID.
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint16_t unique_id = ((uint16_t)board_id.id[6] << 8) | board_id.id[7];
    h[20] = unique_id & 0xFF;
    h[21] = (unique_id >> 8) & 0xFF;

    memset(&h[22], 0, 9);
    strncpy((char *)&h[22], "Hexi-GFX", 9);

    h[31] = compute_checksum(h);
}

static void __not_in_flash_func(i2c_slave_isr)(void) {
    i2c_hw_t *hw = I2C_HW->hw;
    uint32_t stat = hw->intr_stat;

    if (stat & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
        uint8_t b = (uint8_t)hw->data_cmd;
        if (rx_state == S_ADDR_HI) {
            addr_hi = b;
            rx_state = S_ADDR_LO;
        } else if (rx_state == S_ADDR_LO) {
            addr = ((uint16_t)addr_hi << 8) | b;
            if (addr >= HEX_EEPROM_SIZE) addr = 0;
            rx_state = S_DATA;
        } else {
            if (addr < HEX_EEPROM_SIZE) {
                eeprom[addr++] = b;
                if (addr >= HEX_EEPROM_SIZE) addr = 0;
                write_count++;
            }
        }
    }

    if (stat & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
        hw->clr_rd_req;
        if (addr == 0) header_read = true;
        hw->data_cmd = eeprom[addr];
        addr = (addr + 1) % HEX_EEPROM_SIZE;
        read_count++;
    }

    if (stat & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
        hw->clr_stop_det;
        rx_state = S_ADDR_HI;
    }

    if (stat & I2C_IC_INTR_STAT_R_RESTART_DET_BITS) {
        hw->clr_restart_det;
        rx_state = S_ADDR_HI;
    }
}

static void start_i2c_slave(void) {
    gpio_init(I2C_SDA_PIN);
    gpio_init(I2C_SCL_PIN);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    i2c_init(I2C_HW, 100000);

    // Switch from pico-sdk's default (master) config into slave/target
    // mode -- pico-sdk has no built-in slave-mode API, so this part is
    // necessarily raw register access.
    I2C_HW->hw->enable = 0;
    I2C_HW->hw->con = I2C_IC_CON_SPEED_VALUE_FAST << I2C_IC_CON_SPEED_LSB
                     | I2C_IC_CON_IC_RESTART_EN_BITS
                     | I2C_IC_CON_TX_EMPTY_CTRL_BITS;
    I2C_HW->hw->sar = HEX_I2C_ADDR;
    I2C_HW->hw->enable = 1;

    I2C_HW->hw->intr_mask = I2C_IC_INTR_MASK_M_RX_FULL_BITS
                           | I2C_IC_INTR_MASK_M_RD_REQ_BITS
                           | I2C_IC_INTR_MASK_M_STOP_DET_BITS
                           | I2C_IC_INTR_MASK_M_RESTART_DET_BITS;

    irq_set_exclusive_handler(I2C0_IRQ, i2c_slave_isr);
    irq_set_enabled(I2C0_IRQ, true);
}

void eeprom_i2c_start(void) {
    memset(eeprom, 0xFF, HEX_EEPROM_SIZE);
    build_header();
    start_i2c_slave();
}

uint32_t eeprom_i2c_read_count(void) { return read_count; }
uint32_t eeprom_i2c_write_count(void) { return write_count; }
bool eeprom_i2c_header_was_read(void) { return header_read; }
