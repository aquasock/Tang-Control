#pragma once

#include <stddef.h>
#include <stdint.h>

// Version 1 extended control packets use legacy TangCore frame type 0x10.
// Every packet carries a sequence number and a CRC-16/CCITT-FALSE checksum.
enum : uint8_t {
    FPGA_EXT_VERSION = 1,
    FPGA_EXT_COMMAND = 0x10,
    FPGA_EXT_CAPABILITIES = 0x00,
    FPGA_EXT_READ32 = 0x01,
    FPGA_EXT_WRITE32 = 0x02,
    FPGA_EXT_SET_BAUD = 0x03,
};

enum : uint32_t {
    FPGA_EXT_CAP_READ32 = 1u << 0,
    FPGA_EXT_CAP_WRITE32 = 1u << 1,
    // Reserved for the compatible streaming and baud-switch extensions.
    FPGA_EXT_CAP_STREAM = 1u << 2,
    FPGA_EXT_CAP_BAUD_SWITCH = 1u << 3,
};

struct fpga_debug_result {
    uint8_t status;
    uint8_t opcode;
    uint16_t sequence;
    uint32_t address;
    uint32_t data;
};

struct fpga_debug_stats {
    uint32_t requests;
    uint32_t responses;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t malformed;
    uint32_t unexpected;
};

void fpga_debug_init(void);
// Serialize complete request/response transactions across every protocol that
// shares the FPGA UART.  A packet sender must hold this lock until its matching
// response arrives; protecting only the transmit bytes permits another packet
// to overtake the outstanding response.
bool fpga_link_acquire(uint32_t timeout_ms);
void fpga_link_release(void);
bool fpga_debug_request(uint8_t opcode, uint32_t address, uint32_t data,
                        fpga_debug_result *result, uint32_t timeout_ms = 250);
void fpga_debug_handle_response(const uint8_t *payload, size_t length);
void fpga_debug_get_stats(fpga_debug_stats *stats);
bool fpga_debug_set_baud(uint32_t baudrate);
