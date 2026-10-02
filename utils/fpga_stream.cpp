#include "fpga_stream.h"

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
}
#include "fpga_debug.h"
#include "utils.h"

namespace {

constexpr size_t RESPONSE_PAYLOAD_LENGTH = 13;

SemaphoreHandle_t stream_mutex;
SemaphoreHandle_t response_ready;
volatile bool pending;
volatile uint8_t pending_flags;
volatile uint16_t pending_id;
fpga_stream_result response;

uint16_t crc16_byte(uint16_t crc, uint8_t byte)
{
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                              : static_cast<uint16_t>(crc << 1);
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

void send_crc_byte(uint8_t byte, uint16_t &crc)
{
    fpga_tx_byte(byte);
    crc = crc16_byte(crc, byte);
}

} // namespace

void fpga_stream_init(void)
{
    stream_mutex = xSemaphoreCreateMutex();
    response_ready = xSemaphoreCreateBinary();
    pending = false;
}

bool fpga_stream_send(uint8_t flags, uint16_t stream_id, uint32_t offset,
                      const uint8_t *data, uint16_t length,
                      fpga_stream_result *result, uint32_t timeout_ms)
{
    if (stream_mutex == nullptr || response_ready == nullptr || result == nullptr ||
        length > FPGA_STREAM_MAX_DATA || (length != 0 && data == nullptr)) {
        return false;
    }
    if (xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    if (!fpga_link_acquire(timeout_ms)) {
        xSemaphoreGive(stream_mutex);
        return false;
    }
    while (xSemaphoreTake(response_ready, 0) == pdTRUE) {
    }

    pending_flags = flags;
    pending_id = stream_id;
    pending = true;

    fpga_tx_lock();
    fpga_tx_header(FPGA_STREAM_COMMAND, static_cast<int>(length) + 13);
    uint16_t crc = crc16_byte(0xffffu, FPGA_STREAM_COMMAND);
    send_crc_byte(FPGA_STREAM_VERSION, crc);
    send_crc_byte(flags, crc);
    send_crc_byte(static_cast<uint8_t>(stream_id >> 8), crc);
    send_crc_byte(static_cast<uint8_t>(stream_id), crc);
    send_crc_byte(static_cast<uint8_t>(offset >> 24), crc);
    send_crc_byte(static_cast<uint8_t>(offset >> 16), crc);
    send_crc_byte(static_cast<uint8_t>(offset >> 8), crc);
    send_crc_byte(static_cast<uint8_t>(offset), crc);
    send_crc_byte(static_cast<uint8_t>(length >> 8), crc);
    send_crc_byte(static_cast<uint8_t>(length), crc);
    for (uint16_t i = 0; i < length; ++i) {
        send_crc_byte(data[i], crc);
    }
    fpga_tx_byte(static_cast<uint8_t>(crc >> 8));
    fpga_tx_byte(static_cast<uint8_t>(crc));
    fpga_tx_unlock();

    const BaseType_t received =
        xSemaphoreTake(response_ready, pdMS_TO_TICKS(timeout_ms));
    if (received == pdTRUE) {
        taskENTER_CRITICAL();
        *result = response;
        taskEXIT_CRITICAL();
    } else {
        pending = false;
    }
    // Keep the shared link locked through the matching response.  The FPGA
    // transport has one response channel and cannot accept an unrelated debug
    // request while this stream frame is outstanding.
    fpga_link_release();
    xSemaphoreGive(stream_mutex);
    // A stream can otherwise reacquire the link immediately and starve the
    // equal-priority metadata and playback-control tasks indefinitely.
    taskYIELD();
    return received == pdTRUE;
}

void fpga_stream_handle_response(const uint8_t *payload, size_t length)
{
    if (length != RESPONSE_PAYLOAD_LENGTH || payload[0] != FPGA_STREAM_VERSION) {
        return;
    }
    uint16_t crc = crc16_byte(0xffffu, FPGA_STREAM_COMMAND);
    for (size_t i = 0; i < 11; ++i) {
        crc = crc16_byte(crc, payload[i]);
    }
    if (crc != read_be16(&payload[11])) {
        return;
    }
    const uint16_t stream_id = read_be16(&payload[3]);
    if (!pending || payload[2] != pending_flags || stream_id != pending_id) {
        return;
    }
    response.status = payload[1];
    response.flags = payload[2];
    response.stream_id = stream_id;
    response.next_offset = read_be32(&payload[5]);
    response.credit = read_be16(&payload[9]);
    pending = false;
    xSemaphoreGive(response_ready);
}
