#pragma once

#include <stddef.h>
#include <stdint.h>

// The application image the BL616 is currently executing, measured from flash.
struct FwRunningImage {
    bool valid;
    // Flash offset of the image's boot header (XIP offset minus 0x1000).
    uint32_t offset;
    uint32_t size;
    uint8_t sha256[32];
};

// Computed once; the running image cannot change without a reset.
const FwRunningImage &fw_running_image(void);

// Validate a BL616 application image on the SD card, copy it to the staging
// region, and verify the staged copy against expected_sha256.  On success
// before_commit() runs, then the image is copied over the running application
// from RAM with interrupts disabled and the chip resets; this call does not
// return.  On failure it returns false with a reason in error.
bool fw_update_from_file(const char *fatfs_path, const uint8_t expected_sha256[32],
                         void (*before_commit)(void), char *error, size_t error_size);
