#include "fpga_debug.h"
#include "fpga_ext_frame.h"

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
SemaphoreHandle_t link_mutex;
SemaphoreHandle_t response_ready;
volatile bool pending;
volatile uint8_t pending_opcode;
volatile uint16_t pending_sequence;
volatile uint32_t pending_address;
fpga_debug_result response;
fpga_debug_stats counters;
uint16_t next_sequence = 1;

uint16_t packet_crc(uint8_t command, const uint8_t *payload, size_t length)
{
    return fpga_ext_packet_crc(command, payload, length);
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

void write_be32(uint8_t *data, uint32_t value)
{
    fpga_ext_write_be32(data, value);
}

} // namespace

namespace {

// Send one request whose payload starts with version, opcode, a sequence
// placeholder, and the address, then wait for the matching 0x10 response.
// The payload buffer must have two spare bytes for the CRC.
bool transaction_locked(uint8_t command, uint8_t *payload, size_t length,
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

    const uint16_t sequence = next_sequence++;
    if (next_sequence == 0) {
        next_sequence = 1;
    }
    const size_t sealed_length = fpga_ext_seal_request(command, payload, length, sequence);

    taskENTER_CRITICAL();
    pending_opcode = payload[1];
    pending_sequence = sequence;
    pending_address = read_be32(&payload[4]);
    pending = true;
    ++counters.requests;
    fpga_tx_header(command, static_cast<int>(sealed_length) + 1);
    for (size_t i = 0; i < sealed_length; ++i) {
        fpga_tx_byte(payload[i]);
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

bool request_locked(uint8_t opcode, uint32_t address, uint32_t data,
                    fpga_debug_result *result, uint32_t timeout_ms)
{
    uint8_t payload[REQUEST_PAYLOAD_LENGTH];
    payload[0] = FPGA_EXT_VERSION;
    payload[1] = opcode;
    write_be32(&payload[4], address);
    write_be32(&payload[8], data);
    return transaction_locked(FPGA_EXT_COMMAND, payload, REQUEST_PAYLOAD_LENGTH - 2,
                              result, timeout_ms);
}

} // namespace

void fpga_debug_init(void)
{
    request_mutex = xSemaphoreCreateMutex();
    link_mutex = xSemaphoreCreateMutex();
    response_ready = xSemaphoreCreateBinary();
    pending = false;
    counters = {};
}

bool fpga_link_acquire(uint32_t timeout_ms)
{
    return link_mutex != nullptr &&
           xSemaphoreTake(link_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void fpga_link_release(void)
{
    if (link_mutex != nullptr) {
        xSemaphoreGive(link_mutex);
    }
}

bool fpga_debug_request(uint8_t opcode, uint32_t address, uint32_t data,
                        fpga_debug_result *result, uint32_t timeout_ms)
{
    if (!fpga_link_acquire(timeout_ms)) {
        return false;
    }
    const bool received = request_locked(opcode, address, data, result, timeout_ms);
    fpga_link_release();
    // Let an equal-priority stream sender that was waiting on the shared link
    // run before this task can submit another register transaction.
    taskYIELD();
    return received;
}

bool fpga_debug_write_block(uint32_t address, const uint32_t *words, size_t count,
                            fpga_debug_result *result, uint32_t timeout_ms)
{
    if (words == nullptr || count == 0 || count > FPGA_EXT_BLOCK_MAX_WORDS ||
        (address & 3u) != 0) {
        return false;
    }
    uint8_t payload[FPGA_EXT_BLOCK_HEADER_LENGTH + 4 * FPGA_EXT_BLOCK_MAX_WORDS + 2];
    const size_t length = fpga_ext_block_payload(payload, FPGA_EXT_VERSION,
                                                 FPGA_EXT_WRITE_BLOCK, address,
                                                 words, count);
    if (!fpga_link_acquire(timeout_ms)) {
        return false;
    }
    const bool received = transaction_locked(FPGA_BLOCK_COMMAND, payload, length,
                                             result, timeout_ms);
    fpga_link_release();
    taskYIELD();
    return received;
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
    if (!fpga_link_acquire(1000)) {
        return false;
    }
    fpga_debug_result result;
    if (!request_locked(FPGA_EXT_SET_BAUD, 0, baudrate, &result, 1000) ||
        result.status != 0 || result.data != baudrate) {
        fpga_link_release();
        return false;
    }
    // The FPGA changes rate only after its final response stop bit.
    vTaskDelay(pdMS_TO_TICKS(2));
    const bool changed = fpga_uart_set_baud(baudrate);
    fpga_link_release();
    taskYIELD();
    return changed;
}
