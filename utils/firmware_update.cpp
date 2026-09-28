#include "firmware_update.h"

#include <stdio.h>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_flash.h"
#include "bflb_irq.h"
#include "bl616_glb.h"
#include "ff.h"
}

#include "firmware_image.h"
#include "sha256.h"

namespace {

uint8_t sector_buffer[FW_SECTOR_SIZE];
uint8_t verify_buffer[FW_SECTOR_SIZE];
FwRunningImage running_image;
bool running_image_measured;

uint32_t round_to_sector(uint32_t size)
{
    return (size + FW_SECTOR_SIZE - 1) & ~(FW_SECTOR_SIZE - 1);
}

bool digests_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t difference = 0;
    for (unsigned i = 0; i < 32; ++i) {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}

void fail(char *error, size_t error_size, const char *reason)
{
    snprintf(error, error_size, "%s", reason);
}

bool hash_flash(uint32_t address, uint32_t size, uint8_t digest[32])
{
    Sha256 sha;
    sha256_init(sha);
    for (uint32_t offset = 0; offset < size; offset += FW_SECTOR_SIZE) {
        const uint32_t count = size - offset < FW_SECTOR_SIZE ? size - offset : FW_SECTOR_SIZE;
        if (bflb_flash_read(address + offset, sector_buffer, count) != 0) {
            return false;
        }
        sha256_update(sha, sector_buffer, count);
    }
    sha256_final(sha, digest);
    return true;
}

// Runs entirely from TCM and ROM: once the first application sector is erased
// no instruction may be fetched from the application's XIP flash.  Interrupts
// stay disabled until the reset, and the loops are kept free of library calls.
__attribute__((noinline, noreturn, optimize("no-tree-loop-distribute-patterns")))
ATTR_TCM_SECTION void commit_staged_image(uint32_t new_size, uint32_t old_size)
{
    (void)bflb_irq_save();
    const uint32_t span = new_size > old_size ? new_size : old_size;
    for (uint32_t offset = 0; offset < span; offset += FW_SECTOR_SIZE) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            bflb_flash_erase(FW_APP_BASE + offset, FW_SECTOR_SIZE);
            if (offset >= new_size) {
                // Clear the tail of a larger previous image.
                break;
            }
            bflb_flash_read(FW_STAGING_BASE + offset, sector_buffer, FW_SECTOR_SIZE);
            bflb_flash_write(FW_APP_BASE + offset, sector_buffer, FW_SECTOR_SIZE);
            bflb_flash_read(FW_APP_BASE + offset, verify_buffer, FW_SECTOR_SIZE);
            uint8_t difference = 0;
            for (uint32_t i = 0; i < FW_SECTOR_SIZE; ++i) {
                difference |= sector_buffer[i] ^ verify_buffer[i];
            }
            if (difference == 0) {
                break;
            }
        }
    }
    GLB_SW_POR_Reset();
    for (;;) {
    }
}

} // namespace

const FwRunningImage &fw_running_image(void)
{
    if (!running_image_measured) {
        running_image_measured = true;
        running_image = {};
        // The XIP offset maps code after the 4 KiB header region, so the
        // image and its boot header begin one header region earlier.
        const uint32_t xip_offset = bflb_flash_get_image_offset();
        running_image.offset = xip_offset >= FW_HEADER_REGION ?
                               xip_offset - FW_HEADER_REGION : 0;
        uint8_t header[FW_BOOT_HEADER_SIZE];
        uint32_t size = 0;
        if (xip_offset >= FW_HEADER_REGION &&
            bflb_flash_read(running_image.offset, header, sizeof(header)) == 0 &&
            fw_check_boot_header(header, &size) == FwImageError::NONE &&
            hash_flash(running_image.offset, size, running_image.sha256)) {
            running_image.size = size;
            running_image.valid = true;
        }
    }
    return running_image;
}

bool fw_update_from_file(const char *fatfs_path, const uint8_t expected_sha256[32],
                         void (*before_commit)(void), char *error, size_t error_size)
{
    const FwRunningImage &running = fw_running_image();
    if (!running.valid || running.offset != FW_APP_BASE) {
        fail(error, error_size, "running image is not at the expected application offset");
        return false;
    }

    FIL file;
    if (f_open(&file, fatfs_path, FA_READ) != FR_OK) {
        fail(error, error_size, "cannot open image");
        return false;
    }
    const FSIZE_t file_size = f_size(&file);
    UINT count = 0;
    uint32_t image_size = 0;
    FwImageError image_error = FwImageError::LENGTH;
    if (file_size >= FW_BOOT_HEADER_SIZE &&
        f_read(&file, sector_buffer, FW_BOOT_HEADER_SIZE, &count) == FR_OK &&
        count == FW_BOOT_HEADER_SIZE) {
        image_error = fw_check_boot_header(sector_buffer, &image_size);
    }
    if (image_error != FwImageError::NONE || image_size != file_size) {
        f_close(&file);
        fail(error, error_size, image_error != FwImageError::NONE ?
             fw_image_error_text(image_error) : "file size differs from boot header");
        return false;
    }

    // Erase one sector at a time: each erase runs with interrupts masked, so a
    // single large erase would stall USB and the scheduler for seconds.
    for (uint32_t offset = 0; offset < round_to_sector(image_size);
         offset += FW_SECTOR_SIZE) {
        if (bflb_flash_erase(FW_STAGING_BASE + offset, FW_SECTOR_SIZE) != 0) {
            f_close(&file);
            fail(error, error_size, "cannot erase staging region");
            return false;
        }
        taskYIELD();
    }
    // Stage the image while hashing exactly the bytes read from the card.
    if (f_lseek(&file, 0) != FR_OK) {
        f_close(&file);
        fail(error, error_size, "cannot rewind image");
        return false;
    }
    Sha256 sha;
    sha256_init(sha);
    for (uint32_t offset = 0; offset < image_size; offset += FW_SECTOR_SIZE) {
        const uint32_t want = image_size - offset < FW_SECTOR_SIZE ?
                              image_size - offset : FW_SECTOR_SIZE;
        if (f_read(&file, sector_buffer, want, &count) != FR_OK || count != want ||
            bflb_flash_write(FW_STAGING_BASE + offset, sector_buffer, want) != 0) {
            f_close(&file);
            fail(error, error_size, "staging write failed");
            return false;
        }
        sha256_update(sha, sector_buffer, want);
        taskYIELD();
    }
    f_close(&file);
    uint8_t digest[32];
    sha256_final(sha, digest);
    if (!digests_equal(digest, expected_sha256)) {
        fail(error, error_size, "SD image SHA-256 does not match");
        return false;
    }
    if (!hash_flash(FW_STAGING_BASE, image_size, digest) ||
        !digests_equal(digest, expected_sha256)) {
        fail(error, error_size, "staged image SHA-256 does not match");
        return false;
    }

    if (before_commit != nullptr) {
        before_commit();
    }
    vTaskSuspendAll();
    commit_staged_image(image_size, running.size);
}
