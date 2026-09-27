#include "fpga_debug.h"

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
}

#include "utils.h"
#include "init.h"

namespace {

constexpr size_t REQUEST_PAYLOAD_LENGTH = 14;
constexpr size_t RESPONSE_PAYLOAD_LENGTH = 15;

SemaphoreHandle_t request_mutex;
SemaphoreHandle_t response_ready;
volatile bool pending;
volatile uint8_t pending_opcode;
volatile uint16_t pending_sequence;
volatile uint32_t pending_address;
fpga_debug_result response;
fpga_debug_stats counters;
uint16_t next_sequence = 1;

uint16_t crc16_byte(uint16_t crc, uint8_t byte)
{
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                              : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

uint16_t packet_crc(uint8_t command, const uint8_t *payload, size_t length)
{
    uint16_t crc = 0xffffu;
    crc = crc16_byte(crc, command);
    for (size_t i = 0; i < length; ++i) {
        crc = crc16_byte(crc, payload[i]);
    }
    return crc;
}

uint16_t read_be16(const uint8_t *data)
{
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

} // namespace

void fpga_debug_init(void)
{
    request_mutex = xSemaphoreCreateMutex();
    response_ready = xSemaphoreCreateBinary();
    pending = false;
    counters = {};
}

bool fpga_debug_request(uint8_t opcode, uint32_t address, uint32_t data,
                        fpga_debug_result *result, uint32_t timeout_ms)
{
    if (request_mutex == nullptr || response_ready == nullptr || result == nullptr) {
        return false;
    }
    if (xSemaphoreTake(request_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }

    while (xSemaphoreTake(response_ready, 0) == pdTRUE) {
    }

    uint8_t payload[REQUEST_PAYLOAD_LENGTH];
    const uint16_t sequence = next_sequence++;
    if (next_sequence == 0) {
        next_sequence = 1;
    }
    payload[0] = FPGA_EXT_VERSION;
    payload[1] = opcode;
    write_be16(&payload[2], sequence);
    write_be32(&payload[4], address);
    write_be32(&payload[8], data);
    const uint16_t crc = packet_crc(FPGA_EXT_COMMAND, payload, 12);
    write_be16(&payload[12], crc);

    taskENTER_CRITICAL();
    pending_opcode = opcode;
    pending_sequence = sequence;
    pending_address = address;
    pending = true;
    ++counters.requests;
    fpga_tx_header(FPGA_EXT_COMMAND, REQUEST_PAYLOAD_LENGTH + 1);
    for (uint8_t byte : payload) {
        fpga_tx_byte(byte);
    }
    taskEXIT_CRITICAL();

    const BaseType_t received =
        xSemaphoreTake(response_ready, pdMS_TO_TICKS(timeout_ms));
    if (received == pdTRUE) {
        taskENTER_CRITICAL();
        *result = response;
        taskEXIT_CRITICAL();
    } else {
        taskENTER_CRITICAL();
        pending = false;
        ++counters.timeouts;
        taskEXIT_CRITICAL();
    }
    xSemaphoreGive(request_mutex);
    return received == pdTRUE;
}

void fpga_debug_handle_response(const uint8_t *payload, size_t length)
{
    if (length != RESPONSE_PAYLOAD_LENGTH || payload[0] != FPGA_EXT_VERSION) {
        ++counters.malformed;
        return;
    }

    const uint16_t expected_crc = read_be16(&payload[13]);
    if (packet_crc(FPGA_EXT_COMMAND, payload, 13) != expected_crc) {
        ++counters.crc_errors;
        return;
    }

    const uint8_t opcode = payload[1] & 0x7fu;
    const uint16_t sequence = read_be16(&payload[3]);
    const uint32_t address = read_be32(&payload[5]);
    if (!pending || sequence != pending_sequence || opcode != pending_opcode ||
        address != pending_address) {
        ++counters.unexpected;
        return;
    }

    response.status = payload[2];
    response.opcode = opcode;
    response.sequence = sequence;
    response.address = address;
    response.data = read_be32(&payload[9]);
    pending = false;
    ++counters.responses;
    xSemaphoreGive(response_ready);
}

void fpga_debug_get_stats(fpga_debug_stats *stats)
{
    if (stats == nullptr) {
        return;
    }
    taskENTER_CRITICAL();
    *stats = counters;
    taskEXIT_CRITICAL();
}

bool fpga_debug_set_baud(uint32_t baudrate)
{
    if (baudrate != 2000000 && baudrate != 5000000) {
        return false;
    }
    if (fpga_uart_get_baud() == baudrate) {
        return true;
    }
    fpga_debug_result result;
    if (!fpga_debug_request(FPGA_EXT_SET_BAUD, 0, baudrate, &result) ||
        result.status != 0 || result.data != baudrate) {
        return false;
    }
    // The FPGA changes rate only after its final response stop bit.
    vTaskDelay(pdMS_TO_TICKS(2));
    return fpga_uart_set_baud(baudrate);
}
