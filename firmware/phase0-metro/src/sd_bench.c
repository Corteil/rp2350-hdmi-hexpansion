// Phase 0 B2 (microSD half) -- see ../../README.md section 8, "Phase 0 -
// Part B, B2". Uses carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (Apache-2.0)
// wrapping elm-chan's FatFs, rather than hand-rolling the SD-over-SPI
// protocol -- see third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico for the
// vendored library and ../hw_config.c for this board's pin wiring.
//
// Deliberately file-API only, never raw sectors: this runs against
// whatever card is actually inserted on the bench, which may have other
// data on it. A distinctly-named test file can't clobber that; a raw
// sector write could.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "f_util.h"

#include "sd_bench.h"

#define SD_TEST_FILENAME "phase0test.txt"
#define SD_BW_TEST_FILENAME "phase0bw.bin"
#define SD_BW_TEST_BYTES (256 * 1024)

static void report_capacity(FATFS *fs) {
    DWORD free_clusters;
    FATFS *fs_ptr = fs;
    FRESULT fr = f_getfree("", &free_clusters, &fs_ptr);
    if (fr != FR_OK) {
        printf("  mounted (f_getfree failed: %s (%d))\n", FRESULT_str(fr), fr);
        return;
    }
    double free_mb = (double)free_clusters * fs->csize * 512.0 / (1024 * 1024);
    double total_mb = (double)(fs->n_fatent - 2) * fs->csize * 512.0 / (1024 * 1024);
    printf("  mounted: %.1f MB total, %.1f MB free, FAT type %d\n",
           total_mb, free_mb, fs->fs_type);
}

static bool write_read_back_test(void) {
    const char *msg = "phase0-metro SD bring-up test\n";
    FIL fil;

    FRESULT fr = f_open(&fil, SD_TEST_FILENAME, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("  FAIL: f_open(%s, write) error: %s (%d)\n", SD_TEST_FILENAME, FRESULT_str(fr), fr);
        return false;
    }

    UINT written;
    fr = f_write(&fil, msg, strlen(msg), &written);
    f_close(&fil);
    if (fr != FR_OK || written != strlen(msg)) {
        printf("  FAIL: f_write error: %s (%d)\n", FRESULT_str(fr), fr);
        return false;
    }

    fr = f_open(&fil, SD_TEST_FILENAME, FA_READ);
    if (fr != FR_OK) {
        printf("  FAIL: f_open(%s, read) error: %s (%d)\n", SD_TEST_FILENAME, FRESULT_str(fr), fr);
        return false;
    }

    char readback[64] = {0};
    UINT bytes_read;
    fr = f_read(&fil, readback, sizeof(readback) - 1, &bytes_read);
    f_close(&fil);
    if (fr != FR_OK || bytes_read != strlen(msg) || memcmp(readback, msg, bytes_read) != 0) {
        printf("  FAIL: readback mismatch (fr=%d, read %u bytes)\n", fr, (unsigned)bytes_read);
        return false;
    }

    return true;
}

static void write_bandwidth_test(void) {
    static uint8_t bw_buf[4096];
    for (size_t i = 0; i < sizeof(bw_buf); i++) {
        bw_buf[i] = (uint8_t)i;
    }

    FIL fil;
    FRESULT fr = f_open(&fil, SD_BW_TEST_FILENAME, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("  bandwidth test skipped: f_open(%s) error: %s (%d)\n",
               SD_BW_TEST_FILENAME, FRESULT_str(fr), fr);
        return;
    }

    absolute_time_t start = get_absolute_time();
    size_t total_written = 0;
    while (total_written < SD_BW_TEST_BYTES) {
        UINT written;
        fr = f_write(&fil, bw_buf, sizeof(bw_buf), &written);
        if (fr != FR_OK || written != sizeof(bw_buf)) break;
        total_written += written;
    }
    f_close(&fil);
    int64_t us = absolute_time_diff_us(start, get_absolute_time());

    if (fr != FR_OK) {
        printf("  bandwidth test FAIL: f_write error: %s (%d) after %u bytes\n",
               FRESULT_str(fr), fr, (unsigned)total_written);
    } else {
        printf("  write bandwidth: %u bytes in %.2f ms (~%.2f MB/s)\n",
               (unsigned)total_written, us / 1000.0,
               (double)total_written / (us / 1e6) / (1024 * 1024));
    }

    f_unlink(SD_BW_TEST_FILENAME);
}

void sd_bench_run(void) {
    printf("\nphase0-metro: B2 microSD bring-up\n");

    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("  skipped: f_mount failed: %s (%d) -- no card inserted, or a "
               "wiring/format issue\n", FRESULT_str(fr), fr);
        return;
    }

    report_capacity(&fs);

    bool ok = write_read_back_test();
    printf("  write/read-back test (%s): %s\n", SD_TEST_FILENAME, ok ? "PASS" : "FAIL");

    if (ok) {
        write_bandwidth_test();
    } else {
        printf("  bandwidth test skipped (write/read-back test failed)\n");
    }

    f_unmount("");
}
