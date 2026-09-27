#pragma once

#include <stdint.h>

extern "C" {
#include "ff.h"
}

enum class fpga_file_stream_status : uint8_t {
    OK,
    INVALID_ARGUMENT,
    BUSY,
    CORE_UNAVAILABLE,
    OPEN_FAILED,
    FILE_TOO_LARGE,
    BAUD_FAILED,
    READ_FAILED,
    TRANSPORT_FAILED,
    CANCELLED,
    CLOSE_FAILED,
    RESTORE_BAUD_FAILED,
};

struct fpga_file_stream_result {
    fpga_file_stream_status status;
    FRESULT filesystem_status;
    uint8_t transport_status;
    uint32_t bytes;
    uint32_t crc32;
    uint64_t elapsed_ms;
};

using fpga_file_stream_cancel = bool (*)(void *context);

void fpga_file_stream_init(void);
fpga_file_stream_result fpga_file_stream(
    const char *path, fpga_file_stream_cancel cancel = nullptr,
    void *cancel_context = nullptr);
const char *fpga_file_stream_status_text(fpga_file_stream_status status);
