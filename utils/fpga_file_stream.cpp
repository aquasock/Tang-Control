#include "fpga_file_stream.h"

#include <limits.h>
#include <string.h>

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "bflb_mtimer.h"
}

#include "fpga_debug.h"
#include "fpga_stream.h"
#include "init.h"
#include "utils.h"

namespace {

SemaphoreHandle_t file_stream_mutex;
uint16_t next_stream_id = 1;
USB_NOCACHE_RAM_SECTION uint8_t stream_buffer[FPGA_STREAM_MAX_DATA];

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length-- != 0) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

bool send_frame(uint8_t flags, uint16_t stream_id, uint32_t offset,
                const uint8_t *data, uint16_t length, uint32_t expected_next,
                fpga_file_stream_result &summary)
{
    fpga_stream_result response;
    if (!fpga_stream_send(flags, stream_id, offset, data, length, &response)) {
        summary.transport_status = 0xff;
        return false;
    }
    summary.transport_status = response.status;
    return response.status == 0 && response.next_offset == expected_next;
}

bool cancellation_requested(fpga_file_stream_cancel cancel, void *context)
{
    return cancel != nullptr && cancel(context);
}

} // namespace

void fpga_file_stream_init(void)
{
    file_stream_mutex = xSemaphoreCreateMutex();
}

fpga_file_stream_result fpga_file_stream(const char *path,
                                         fpga_file_stream_cancel cancel,
                                         void *cancel_context)
{
    fpga_file_stream_result summary = {};
    summary.status = fpga_file_stream_status::INVALID_ARGUMENT;
    summary.filesystem_status = FR_OK;
    if (path == nullptr || path[0] == '\0' || file_stream_mutex == nullptr) {
        return summary;
    }
    if (xSemaphoreTake(file_stream_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        summary.status = fpga_file_stream_status::BUSY;
        return summary;
    }

    FIL file;
    bool file_open = false;
    bool using_fast_baud = false;
    bool session_started = false;
    uint16_t stream_id = 0;
    uint32_t crc = 0xffffffffu;
    uint32_t offset = 0;
    const uint64_t started = bflb_mtimer_get_time_ms();

    fpga_debug_result capabilities;
    if (!fpga_debug_request(FPGA_EXT_CAPABILITIES, 0, 0, &capabilities) ||
        capabilities.status != 0 ||
        (capabilities.data & FPGA_EXT_CAP_STREAM) == 0) {
        summary.status = fpga_file_stream_status::CORE_UNAVAILABLE;
        goto finish;
    }

    summary.filesystem_status = f_open(&file, path, FA_READ);
    if (summary.filesystem_status != FR_OK) {
        summary.status = fpga_file_stream_status::OPEN_FAILED;
        goto finish;
    }
    file_open = true;
    if (f_size(&file) > UINT32_MAX) {
        summary.status = fpga_file_stream_status::FILE_TOO_LARGE;
        goto finish;
    }

    if ((capabilities.data & FPGA_EXT_CAP_BAUD_SWITCH) != 0) {
        if (!fpga_debug_set_baud(5000000)) {
            summary.status = fpga_file_stream_status::BAUD_FAILED;
            goto finish;
        }
        using_fast_baud = true;
    }

    stream_id = next_stream_id++;
    if (next_stream_id == 0) {
        next_stream_id = 1;
    }
    if (!send_frame(FPGA_STREAM_START, stream_id, 0, nullptr, 0, 0, summary)) {
        summary.status = fpga_file_stream_status::TRANSPORT_FAILED;
        goto finish;
    }
    session_started = true;

    while (!cancellation_requested(cancel, cancel_context)) {
        UINT count = 0;
        summary.filesystem_status =
            f_read(&file, stream_buffer, sizeof(stream_buffer), &count);
        if (summary.filesystem_status != FR_OK) {
            summary.status = fpga_file_stream_status::READ_FAILED;
            break;
        }
        if (count == 0) {
            summary.status = fpga_file_stream_status::OK;
            break;
        }
        crc = crc32_update(crc, stream_buffer, count);
        if (!send_frame(FPGA_STREAM_DATA, stream_id, offset, stream_buffer,
                        static_cast<uint16_t>(count), offset + count, summary)) {
            summary.status = fpga_file_stream_status::TRANSPORT_FAILED;
            break;
        }
        offset += count;
    }

    if (summary.status == fpga_file_stream_status::INVALID_ARGUMENT) {
        summary.status = fpga_file_stream_status::CANCELLED;
    }
    if (summary.status == fpga_file_stream_status::OK) {
        if (!send_frame(FPGA_STREAM_END, stream_id, offset, nullptr, 0, offset,
                        summary)) {
            summary.status = fpga_file_stream_status::TRANSPORT_FAILED;
        }
    } else if (session_started) {
        fpga_stream_result ignored;
        fpga_stream_send(FPGA_STREAM_CANCEL, stream_id, offset, nullptr, 0,
                         &ignored);
    }

finish:
    if (file_open) {
        const FRESULT close_status = f_close(&file);
        if (close_status != FR_OK &&
            summary.status == fpga_file_stream_status::OK) {
            summary.filesystem_status = close_status;
            summary.status = fpga_file_stream_status::CLOSE_FAILED;
        }
    }
    if (using_fast_baud && !fpga_debug_set_baud(2000000) &&
        summary.status == fpga_file_stream_status::OK) {
        summary.status = fpga_file_stream_status::RESTORE_BAUD_FAILED;
    }
    summary.bytes = offset;
    summary.crc32 = ~crc;
    summary.elapsed_ms = bflb_mtimer_get_time_ms() - started;
    xSemaphoreGive(file_stream_mutex);
    return summary;
}

const char *fpga_file_stream_status_text(fpga_file_stream_status status)
{
    switch (status) {
        case fpga_file_stream_status::OK: return "complete";
        case fpga_file_stream_status::INVALID_ARGUMENT: return "invalid path";
        case fpga_file_stream_status::BUSY: return "streamer busy";
        case fpga_file_stream_status::CORE_UNAVAILABLE: return "core unavailable";
        case fpga_file_stream_status::OPEN_FAILED: return "file open failed";
        case fpga_file_stream_status::FILE_TOO_LARGE: return "file exceeds 4 GiB";
        case fpga_file_stream_status::BAUD_FAILED: return "baud negotiation failed";
        case fpga_file_stream_status::READ_FAILED: return "file read failed";
        case fpga_file_stream_status::TRANSPORT_FAILED: return "FPGA stream failed";
        case fpga_file_stream_status::CANCELLED: return "cancelled";
        case fpga_file_stream_status::CLOSE_FAILED: return "file close failed";
        case fpga_file_stream_status::RESTORE_BAUD_FAILED: return "baud restore failed";
    }
    return "unknown error";
}
