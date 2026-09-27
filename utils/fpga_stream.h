#pragma once

#include <stddef.h>
#include <stdint.h>

enum : uint8_t {
    FPGA_STREAM_COMMAND = 0x11,
    FPGA_STREAM_VERSION = 1,
    FPGA_STREAM_START = 1u << 0,
    FPGA_STREAM_DATA = 1u << 1,
    FPGA_STREAM_END = 1u << 2,
    FPGA_STREAM_CANCEL = 1u << 3,
};

constexpr size_t FPGA_STREAM_MAX_DATA = 1024;

struct fpga_stream_result {
    uint8_t status;
    uint8_t flags;
    uint16_t stream_id;
    uint32_t next_offset;
    uint16_t credit;
};

void fpga_stream_init(void);
bool fpga_stream_send(uint8_t flags, uint16_t stream_id, uint32_t offset,
                      const uint8_t *data, uint16_t length,
                      fpga_stream_result *result, uint32_t timeout_ms = 1000);
void fpga_stream_handle_response(const uint8_t *payload, size_t length);
