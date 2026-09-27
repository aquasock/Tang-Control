/*
 * USB CDC console and SD-card transfer protocol for TangCore.
 *
 * Copyright 2026 aquasock
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_cdc_console.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_mtimer.h"
#include "ff.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
}

#include "usb_config.h"
#include "fpga_debug.h"
#include "fpga_stream.h"
#include "fpga_file_stream.h"
#include "init.h"

extern const char *BOARD_NAME;
extern int16_t active_core;
extern bool core_running;
extern const char *drv;

namespace {

bool valid_remote_path(const char *path);
bool make_sd_path(char *destination, size_t capacity, const char *path,
                  const char *suffix = "");

constexpr uint8_t CDC_IN_EP = 0x81;
constexpr uint8_t CDC_OUT_EP = 0x02;
constexpr uint8_t CDC_INT_EP = 0x83;
constexpr uint16_t USB_VID = TANG_USB_CDC_VID;
constexpr uint16_t USB_PID = TANG_USB_CDC_PID;
constexpr uint16_t USB_LANGID = 0x0409;
constexpr uint32_t CDC_DMA_SIZE = 16 * 1024;
constexpr uint32_t RX_RING_SIZE = 64 * 1024;
constexpr uint32_t RX_RING_MASK = RX_RING_SIZE - 1;
constexpr uint32_t FILE_IO_SIZE = 16 * 1024;
constexpr uint32_t MAX_UPLOAD_SIZE = 256 * 1024 * 1024;
constexpr size_t MAX_REMOTE_PATH = 180;

static_assert((RX_RING_SIZE & RX_RING_MASK) == 0,
              "RX ring size must be a power of two");

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

const uint8_t cdc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               USB_VID, USB_PID, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01,
                               USB_CONFIG_BUS_POWERED, 100),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP,
                            512, 0x02),
    USB_LANGID_INIT(USB_LANGID),
    0x12, USB_DESCRIPTOR_TYPE_STRING,
    'T', 0, 'a', 0, 'n', 0, 'g', 0, 'C', 0, 'o', 0, 'r', 0, 'e', 0,
    0x22, USB_DESCRIPTOR_TYPE_STRING,
    'T', 0, 'a', 0, 'n', 0, 'g', 0, 'C', 0, 'o', 0, 'r', 0, 'e', 0,
    ' ', 0, 'U', 0, 'S', 0, 'B', 0, ' ', 0, 'C', 0, 'D', 0, 'C', 0,
    0x0a, USB_DESCRIPTOR_TYPE_STRING,
    '0', 0, '0', 0, '0', 0, '1', 0,
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x02, 0x02, 0x01, 0x40, 0x01, 0x00,
    0x00
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t rx_dma[CDC_DMA_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t tx_dma[2048];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t rx_ring[RX_RING_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t file_io_buffer[FILE_IO_SIZE];

volatile uint32_t rx_head = 0;
volatile uint32_t rx_tail = 0;
volatile uint32_t rx_dropped = 0;
volatile uint64_t rx_total = 0;
volatile bool usb_configured = false;
volatile bool dtr_enabled = false;
volatile bool tx_busy = false;
volatile bool rx_armed = false;
TaskHandle_t cdc_task_handle = nullptr;

struct usbd_interface cdc_control_intf;
struct usbd_interface cdc_data_intf;

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    while (len--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

void wake_console_from_isr()
{
    if (cdc_task_handle == nullptr) {
        return;
    }
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(cdc_task_handle, &wake);
    portYIELD_FROM_ISR(wake);
}

void ring_push(const uint8_t *data, uint32_t len)
{
    uint32_t head = rx_head;
    const uint32_t tail = rx_tail;
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t next = (head + 1) & RX_RING_MASK;
        if (next == tail) {
            rx_dropped += len - i;
            break;
        }
        rx_ring[head] = data[i];
        head = next;
    }
    rx_head = head;
    rx_total += len;
}

bool ring_pop(uint8_t &byte)
{
    const uint32_t tail = rx_tail;
    if (tail == rx_head) {
        return false;
    }
    byte = rx_ring[tail];
    rx_tail = (tail + 1) & RX_RING_MASK;
    return true;
}

bool arm_rx_read()
{
    if (!usb_configured || rx_armed) {
        return usb_configured;
    }

    rx_armed = true;
    if (usbd_ep_start_read(CDC_OUT_EP, rx_dma, sizeof(rx_dma)) != 0) {
        rx_armed = false;
        return false;
    }
    return true;
}

size_t ring_read(uint8_t *destination, size_t maximum)
{
    const uint32_t tail = rx_tail;
    const uint32_t head = rx_head;
    size_t available = (head - tail) & RX_RING_MASK;
    if (available > maximum) {
        available = maximum;
    }
    if (available == 0) {
        return 0;
    }

    size_t first = RX_RING_SIZE - tail;
    if (first > available) {
        first = available;
    }
    memcpy(destination, &rx_ring[tail], first);
    if (first != available) {
        memcpy(destination + first, rx_ring, available - first);
    }
    rx_tail = (tail + available) & RX_RING_MASK;
    return available;
}

size_t ring_crc(uint32_t &crc, size_t maximum)
{
    const uint32_t tail = rx_tail;
    const uint32_t head = rx_head;
    size_t available = (head - tail) & RX_RING_MASK;
    if (available > maximum) {
        available = maximum;
    }
    if (available == 0) {
        return 0;
    }

    size_t first = RX_RING_SIZE - tail;
    if (first > available) {
        first = available;
    }
    crc = crc32_update(crc, &rx_ring[tail], first);
    if (first != available) {
        crc = crc32_update(crc, rx_ring, available - first);
    }
    rx_tail = (tail + available) & RX_RING_MASK;
    return available;
}

bool cdc_write(const uint8_t *data, size_t len)
{
    if (!usb_configured || !dtr_enabled) {
        return false;
    }

    while (len != 0) {
        // Avoid a max-packet multiple so the endpoint callback does not need a
        // separate zero-length-packet state.
        const size_t chunk = len > sizeof(tx_dma) - 1 ? sizeof(tx_dma) - 1 : len;
        const uint64_t deadline = bflb_mtimer_get_time_ms() + 2000;
        while (tx_busy && usb_configured) {
            if (bflb_mtimer_get_time_ms() >= deadline) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!usb_configured) {
            return false;
        }

        memcpy(tx_dma, data, chunk);
        tx_busy = true;
        if (usbd_ep_start_write(CDC_IN_EP, tx_dma, chunk) != 0) {
            tx_busy = false;
            return false;
        }
        data += chunk;
        len -= chunk;
    }
    return true;
}

void cdc_print(const char *text)
{
    cdc_write(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

void cdc_printf(const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (count <= 0) {
        return;
    }
    const size_t length = static_cast<size_t>(count) < sizeof(buffer)
                              ? static_cast<size_t>(count)
                              : sizeof(buffer) - 1;
    cdc_write(reinterpret_cast<const uint8_t *>(buffer), length);
}

void print_banner()
{
    cdc_print("\r\nTangCore USB console\r\n"
              "Type 'help' for commands.\r\n> ");
}

void print_status()
{
    cdc_printf("board: %s\r\n", BOARD_NAME);
    cdc_printf("uptime_ms: %llu\r\n",
               static_cast<unsigned long long>(bflb_mtimer_get_time_ms()));
    cdc_printf("active_core: %d\r\n", active_core);
    cdc_printf("core_running: %s\r\n", core_running ? "yes" : "no");
    cdc_printf("fpga_uart_baud: %u\r\n",
               static_cast<unsigned>(fpga_uart_get_baud()));
    cdc_printf("usb_rx_bytes: %llu\r\n",
               static_cast<unsigned long long>(rx_total));
    cdc_printf("usb_rx_dropped: %u\r\n", static_cast<unsigned>(rx_dropped));
    fpga_debug_stats stats;
    fpga_debug_get_stats(&stats);
    cdc_printf("fpga_requests: %u\r\n", static_cast<unsigned>(stats.requests));
    cdc_printf("fpga_responses: %u\r\n", static_cast<unsigned>(stats.responses));
    cdc_printf("fpga_timeouts: %u\r\n", static_cast<unsigned>(stats.timeouts));
    cdc_printf("fpga_crc_errors: %u\r\n", static_cast<unsigned>(stats.crc_errors));
    cdc_printf("fpga_malformed: %u\r\n", static_cast<unsigned>(stats.malformed));
    cdc_printf("fpga_unexpected: %u\r\n", static_cast<unsigned>(stats.unexpected));
    cdc_print("OK\r\n");
}

bool parse_u32(const char *text, uint32_t &value, const char **end_out = nullptr)
{
    if (text == nullptr || *text == '\0' || *text == '-') {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || parsed > 0xfffffffful) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    if (end_out != nullptr) {
        *end_out = end;
    } else if (*end != '\0') {
        return false;
    }
    return true;
}

bool run_fpga_request(uint8_t opcode, uint32_t address, uint32_t data,
                      fpga_debug_result &result)
{
    if (!fpga_debug_request(opcode, address, data, &result)) {
        cdc_print("ERR FPGA debug request timed out\r\n");
        return false;
    }
    if (result.status != 0) {
        cdc_printf("ERR FPGA debug status=%u\r\n",
                   static_cast<unsigned>(result.status));
        return false;
    }
    return true;
}

void run_capabilities()
{
    fpga_debug_result result;
    if (!run_fpga_request(FPGA_EXT_CAPABILITIES, 0, 0, result)) {
        return;
    }
    cdc_printf("FPGA protocol=%u capabilities=0x%08x\r\nOK\r\n",
               FPGA_EXT_VERSION, static_cast<unsigned>(result.data));
}

void run_peek(const char *arguments)
{
    uint32_t address;
    const char *end = nullptr;
    if (!parse_u32(arguments, address, &end)) {
        cdc_print("ERR usage: peek <address> [count]\r\n");
        return;
    }
    uint32_t count = 1;
    if (*end != '\0') {
        while (*end == ' ') {
            ++end;
        }
        if (!parse_u32(end, count) || count == 0 || count > 64) {
            cdc_print("ERR count must be 1..64\r\n");
            return;
        }
    }
    if ((address & 3u) != 0 || address > 0xffffffffu - (count - 1u) * 4u) {
        cdc_print("ERR address must be aligned and range must not wrap\r\n");
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        fpga_debug_result result;
        const uint32_t current = address + index * 4u;
        if (!run_fpga_request(FPGA_EXT_READ32, current, 0, result)) {
            return;
        }
        cdc_printf("0x%08x: 0x%08x\r\n", static_cast<unsigned>(current),
                   static_cast<unsigned>(result.data));
    }
    cdc_print("OK\r\n");
}

void run_poke(const char *arguments)
{
    uint32_t address;
    uint32_t value;
    const char *end = nullptr;
    if (!parse_u32(arguments, address, &end)) {
        cdc_print("ERR usage: poke <address> <value>\r\n");
        return;
    }
    while (*end == ' ') {
        ++end;
    }
    if (!parse_u32(end, value) || (address & 3u) != 0) {
        cdc_print("ERR usage: poke <aligned-address> <value>\r\n");
        return;
    }
    fpga_debug_result result;
    if (!run_fpga_request(FPGA_EXT_WRITE32, address, value, result)) {
        return;
    }
    cdc_printf("WROTE 0x%08x: 0x%08x\r\nOK\r\n",
               static_cast<unsigned>(address), static_cast<unsigned>(value));
}

void run_baud(const char *argument)
{
    uint32_t rate;
    if (!parse_u32(argument, rate) || (rate != 2 && rate != 5)) {
        cdc_print("ERR usage: baud <2|5>\r\n");
        return;
    }
    rate *= 1000000u;
    if (!fpga_debug_set_baud(rate)) {
        cdc_print("ERR FPGA baud negotiation failed\r\n");
        return;
    }
    cdc_printf("BAUD %u\r\nOK\r\n", static_cast<unsigned>(rate));
}

void run_stream(const char *path)
{
    if (!valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (strcmp(drv, "sd:") != 0) {
        cdc_print("ERR SD card is not mounted\r\n");
        return;
    }

    char full_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path)) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }
    const fpga_file_stream_result result = fpga_file_stream(full_path);
    if (result.status != fpga_file_stream_status::OK) {
        cdc_printf("ERR stream %s fatfs=%u transport=%u\r\n",
                   fpga_file_stream_status_text(result.status),
                   static_cast<unsigned>(result.filesystem_status),
                   static_cast<unsigned>(result.transport_status));
        return;
    }
    cdc_printf("STREAM bytes=%u ms=%llu crc32=%08x\r\nOK\r\n",
               static_cast<unsigned>(result.bytes),
               static_cast<unsigned long long>(result.elapsed_ms),
               static_cast<unsigned>(result.crc32));
}

void run_benchmark(uint32_t expected)
{
    if (expected == 0 || expected > 128u * 1024u * 1024u) {
        cdc_print("ERR size must be 1..134217728\r\n");
        return;
    }

    cdc_printf("READY %u\r\n", static_cast<unsigned>(expected));
    arm_rx_read();
    uint32_t received = 0;
    uint32_t crc = 0xffffffffu;
    const uint32_t dropped_at_start = rx_dropped;
    const uint64_t start = bflb_mtimer_get_time_ms();
    uint64_t last_data = start;

    while (received < expected && usb_configured) {
        const size_t consumed = ring_crc(crc, expected - received);
        received += consumed;
        arm_rx_read();
        if (consumed != 0) {
            last_data = bflb_mtimer_get_time_ms();
        } else {
            if (bflb_mtimer_get_time_ms() - last_data > 5000) {
                cdc_printf("ERR timeout received=%u expected=%u\r\n",
                           static_cast<unsigned>(received),
                           static_cast<unsigned>(expected));
                return;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
    }

    const uint64_t elapsed = bflb_mtimer_get_time_ms() - start;
    cdc_printf("BENCH bytes=%u ms=%llu crc32=%08x dropped=%u\r\nOK\r\n",
               static_cast<unsigned>(received),
               static_cast<unsigned long long>(elapsed),
               static_cast<unsigned>(~crc),
               static_cast<unsigned>(rx_dropped - dropped_at_start));
}

bool valid_remote_path(const char *path)
{
    const size_t length = strlen(path);
    if (length == 0 || length > MAX_REMOTE_PATH || path[0] == '/' ||
        strstr(path, "..") != nullptr || strchr(path, ':') != nullptr ||
        strchr(path, '\\') != nullptr) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(path[i]);
        if (byte < 0x20 || byte >= 0x7f) {
            return false;
        }
    }
    return true;
}

bool make_sd_path(char *destination, size_t capacity, const char *path,
                  const char *suffix)
{
    const int count = snprintf(destination, capacity, "sd:/%s%s", path, suffix);
    return count > 0 && static_cast<size_t>(count) < capacity;
}

bool file_operations_ready()
{
    if (strcmp(drv, "sd:") != 0) {
        cdc_print("ERR SD card is not mounted\r\n");
        return false;
    }
    if (active_core != 0 || core_running) {
        cdc_print("ERR return to the TangCore main menu first\r\n");
        return false;
    }
    return true;
}

void run_file_crc(const char *path)
{
    if (!valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (!file_operations_ready()) {
        return;
    }

    char full_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path)) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }

    FIL file;
    FRESULT result = f_open(&file, full_path, FA_READ);
    if (result != FR_OK) {
        cdc_printf("ERR open failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }

    uint32_t crc = 0xffffffffu;
    uint32_t total = 0;
    for (;;) {
        UINT count = 0;
        result = f_read(&file, file_io_buffer, sizeof(file_io_buffer), &count);
        if (result != FR_OK || count == 0) {
            break;
        }
        crc = crc32_update(crc, file_io_buffer, count);
        total += count;
    }
    f_close(&file);
    if (result != FR_OK) {
        cdc_printf("ERR read failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }
    cdc_printf("FILE bytes=%u crc32=%08x\r\nOK\r\n",
               static_cast<unsigned>(total), static_cast<unsigned>(~crc));
}

void run_download(const char *path)
{
    if (!valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (!file_operations_ready()) {
        return;
    }

    char full_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path)) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }

    FIL file;
    FRESULT result = f_open(&file, full_path, FA_READ);
    if (result != FR_OK) {
        cdc_printf("ERR open failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }

    const FSIZE_t expected = f_size(&file);
    cdc_printf("READY %llu\r\n", static_cast<unsigned long long>(expected));
    uint64_t total = 0;
    uint32_t crc = 0xffffffffu;
    const uint64_t start = bflb_mtimer_get_time_ms();
    bool send_ok = true;

    while (send_ok) {
        UINT count = 0;
        result = f_read(&file, file_io_buffer, sizeof(file_io_buffer), &count);
        if (result != FR_OK || count == 0) {
            break;
        }
        crc = crc32_update(crc, file_io_buffer, count);
        total += count;
        send_ok = cdc_write(file_io_buffer, count);
    }
    const FRESULT close_result = f_close(&file);
    if (!send_ok || !usb_configured) {
        return;
    }
    if (result != FR_OK || close_result != FR_OK || total != expected) {
        cdc_printf("ERR read failed fatfs=%u bytes=%llu expected=%llu\r\n",
                   static_cast<unsigned>(result != FR_OK ? result : close_result),
                   static_cast<unsigned long long>(total),
                   static_cast<unsigned long long>(expected));
        return;
    }

    const uint64_t elapsed = bflb_mtimer_get_time_ms() - start;
    cdc_printf("GET bytes=%llu ms=%llu crc32=%08x\r\nOK\r\n",
               static_cast<unsigned long long>(total),
               static_cast<unsigned long long>(elapsed),
               static_cast<unsigned>(~crc));
}

void run_list(const char *path)
{
    if (*path != '\0' && !valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (!file_operations_ready()) {
        return;
    }

    char full_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path)) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }

    DIR directory;
    FRESULT result = f_opendir(&directory, full_path);
    if (result != FR_OK) {
        cdc_printf("ERR open directory failed fatfs=%u\r\n",
                   static_cast<unsigned>(result));
        return;
    }

    FILINFO info;
    for (;;) {
        result = f_readdir(&directory, &info);
        if (result != FR_OK || info.fname[0] == '\0') {
            break;
        }
        cdc_printf("%c %llu ", (info.fattrib & AM_DIR) ? 'D' : 'F',
                   static_cast<unsigned long long>(info.fsize));
        cdc_write(reinterpret_cast<const uint8_t *>(info.fname),
                  strlen(info.fname));
        cdc_print("\r\n");
    }
    const FRESULT close_result = f_closedir(&directory);
    if (result != FR_OK || close_result != FR_OK) {
        cdc_printf("ERR read directory failed fatfs=%u\r\n",
                   static_cast<unsigned>(result != FR_OK ? result : close_result));
        return;
    }
    cdc_print("OK\r\n");
}

void run_remove(const char *path)
{
    if (!valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (!file_operations_ready()) {
        return;
    }

    char full_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path)) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }
    const FRESULT result = f_unlink(full_path);
    if (result != FR_OK) {
        cdc_printf("ERR remove failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }
    cdc_print("REMOVED\r\nOK\r\n");
}

void run_mkdir(const char *path)
{
    if (!valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (!file_operations_ready()) {
        return;
    }

    char full_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path)) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }
    const FRESULT result = f_mkdir(full_path);
    if (result != FR_OK) {
        cdc_printf("ERR mkdir failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }
    cdc_print("CREATED\r\nOK\r\n");
}

void run_upload(uint32_t expected, uint32_t expected_crc, const char *path)
{
    if (expected == 0 || expected > MAX_UPLOAD_SIZE) {
        cdc_print("ERR size must be 1..268435456\r\n");
        return;
    }
    if (!valid_remote_path(path)) {
        cdc_print("ERR invalid remote path\r\n");
        return;
    }
    if (!file_operations_ready()) {
        return;
    }

    char full_path[192];
    char temporary_path[192];
    char backup_path[192];
    if (!make_sd_path(full_path, sizeof(full_path), path) ||
        !make_sd_path(temporary_path, sizeof(temporary_path), path, ".tangpart") ||
        !make_sd_path(backup_path, sizeof(backup_path), path, ".tangbak")) {
        cdc_print("ERR remote path is too long\r\n");
        return;
    }

    f_unlink(temporary_path);
    FIL file;
    FRESULT result = f_open(&file, temporary_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) {
        cdc_printf("ERR open failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }

    cdc_printf("READY %u\r\n", static_cast<unsigned>(expected));
    arm_rx_read();
    uint32_t received = 0;
    uint32_t crc = 0xffffffffu;
    const uint32_t dropped_at_start = rx_dropped;
    const uint64_t start = bflb_mtimer_get_time_ms();
    uint64_t last_data = start;
    bool write_ok = true;

    while (received < expected && usb_configured) {
        const size_t count = ring_read(file_io_buffer, expected - received);
        arm_rx_read();
        if (count != 0) {
            crc = crc32_update(crc, file_io_buffer, count);
            received += count;
            last_data = bflb_mtimer_get_time_ms();
            if (write_ok) {
                UINT written = 0;
                result = f_write(&file, file_io_buffer, count, &written);
                if (result != FR_OK || written != count) {
                    write_ok = false;
                }
            }
        } else {
            if (bflb_mtimer_get_time_ms() - last_data > 10000) {
                write_ok = false;
                break;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
    }

    if (write_ok) {
        result = f_sync(&file);
        write_ok = result == FR_OK;
    }
    const FRESULT close_result = f_close(&file);
    if (close_result != FR_OK) {
        write_ok = false;
        result = close_result;
    }

    const uint32_t actual_crc = ~crc;
    const uint64_t elapsed = bflb_mtimer_get_time_ms() - start;
    if (received != expected) {
        f_unlink(temporary_path);
        cdc_printf("ERR timeout received=%u expected=%u\r\n",
                   static_cast<unsigned>(received), static_cast<unsigned>(expected));
        return;
    }
    if (!write_ok) {
        f_unlink(temporary_path);
        cdc_printf("ERR write failed fatfs=%u\r\n", static_cast<unsigned>(result));
        return;
    }
    if (actual_crc != expected_crc || rx_dropped != dropped_at_start) {
        f_unlink(temporary_path);
        cdc_printf("ERR integrity crc32=%08x dropped=%u\r\n",
                   static_cast<unsigned>(actual_crc),
                   static_cast<unsigned>(rx_dropped - dropped_at_start));
        return;
    }

    FILINFO info;
    const bool had_original = f_stat(full_path, &info) == FR_OK;
    if (had_original) {
        f_unlink(backup_path);
        result = f_rename(full_path, backup_path);
        if (result != FR_OK) {
            f_unlink(temporary_path);
            cdc_printf("ERR backup rename failed fatfs=%u\r\n",
                       static_cast<unsigned>(result));
            return;
        }
    }

    result = f_rename(temporary_path, full_path);
    if (result != FR_OK) {
        if (had_original) {
            f_rename(backup_path, full_path);
        }
        cdc_printf("ERR final rename failed fatfs=%u\r\n",
                   static_cast<unsigned>(result));
        return;
    }
    if (had_original) {
        f_unlink(backup_path);
    }

    cdc_printf("PUT bytes=%u ms=%llu crc32=%08x dropped=0\r\nOK\r\n",
               static_cast<unsigned>(received),
               static_cast<unsigned long long>(elapsed),
               static_cast<unsigned>(actual_crc));
}

void execute_command(char *line)
{
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    char *end = line + strlen(line);
    while (end != line && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }

    if (*line == '\0') {
        return;
    }
    if (strcmp(line, "help") == 0) {
        cdc_print("help              show commands\r\n"
                  "ping              verify command channel\r\n"
                  "status            show TangCore state\r\n"
                  "caps              query FPGA transport capabilities\r\n"
                  "peek <addr> [n]   read one or more FPGA debug words\r\n"
                  "poke <addr> <val> write an FPGA debug word\r\n"
                  "baud <2|5>        negotiate FPGA UART rate in Mbps\r\n"
                  "stream <path>     stream an SD file to the active core\r\n"
                  "bench <bytes>     receive raw data and report speed/CRC\r\n"
                  "put <size> <crc> <path>  upload a file to SD\r\n"
                  "get <path>        download a file from SD\r\n"
                  "ls [path]         list an SD directory\r\n"
                  "rm <path>         remove an SD file or empty directory\r\n"
                  "mkdir <path>      create an SD directory\r\n"
                  "crc <path>        checksum a file on SD\r\n"
                  "OK\r\n");
    } else if (strcmp(line, "ping") == 0) {
        cdc_print("PONG\r\nOK\r\n");
    } else if (strcmp(line, "status") == 0) {
        print_status();
    } else if (strcmp(line, "caps") == 0) {
        run_capabilities();
    } else if (strncmp(line, "peek ", 5) == 0) {
        run_peek(line + 5);
    } else if (strncmp(line, "poke ", 5) == 0) {
        run_poke(line + 5);
    } else if (strncmp(line, "baud ", 5) == 0) {
        run_baud(line + 5);
    } else if (strncmp(line, "stream ", 7) == 0) {
        run_stream(line + 7);
    } else if (strncmp(line, "bench ", 6) == 0) {
        char *parse_end = nullptr;
        const unsigned long size = strtoul(line + 6, &parse_end, 0);
        if (parse_end == line + 6 || *parse_end != '\0') {
            cdc_print("ERR invalid size\r\n");
        } else {
            run_benchmark(static_cast<uint32_t>(size));
        }
    } else if (strncmp(line, "crc ", 4) == 0) {
        run_file_crc(line + 4);
    } else if (strncmp(line, "get ", 4) == 0) {
        run_download(line + 4);
    } else if (strcmp(line, "ls") == 0) {
        run_list("");
    } else if (strncmp(line, "ls ", 3) == 0) {
        run_list(line + 3);
    } else if (strncmp(line, "rm ", 3) == 0) {
        run_remove(line + 3);
    } else if (strncmp(line, "mkdir ", 6) == 0) {
        run_mkdir(line + 6);
    } else if (strncmp(line, "put ", 4) == 0) {
        char *cursor = line + 4;
        char *parse_end = nullptr;
        const unsigned long size = strtoul(cursor, &parse_end, 0);
        if (parse_end == cursor || *parse_end != ' ') {
            cdc_print("ERR usage: put <size> <crc32> <path>\r\n");
            return;
        }
        cursor = parse_end + 1;
        const unsigned long crc = strtoul(cursor, &parse_end, 16);
        if (parse_end == cursor || *parse_end != ' ') {
            cdc_print("ERR usage: put <size> <crc32> <path>\r\n");
            return;
        }
        while (*parse_end == ' ') {
            ++parse_end;
        }
        run_upload(static_cast<uint32_t>(size), static_cast<uint32_t>(crc),
                   parse_end);
    } else {
        cdc_print("ERR unknown command\r\n");
    }
}

void cdc_console_task(void *)
{
    cdc_task_handle = xTaskGetCurrentTaskHandle();
    bool announced = false;
    char line[256];
    size_t line_length = 0;

    for (;;) {
        if (!usb_configured || !dtr_enabled) {
            announced = false;
            line_length = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (!announced) {
            print_banner();
            announced = true;
        }

        uint8_t byte;
        bool made_progress = false;
        while (ring_pop(byte)) {
            made_progress = true;
            if (byte == '\r' || byte == '\n') {
                if (line_length != 0) {
                    line[line_length] = '\0';
                    execute_command(line);
                    line_length = 0;
                    cdc_print("> ");
                }
            } else if ((byte == '\b' || byte == 0x7f) && line_length != 0) {
                --line_length;
            } else if (byte >= 0x20 && byte < 0x7f) {
                if (line_length + 1 < sizeof(line)) {
                    line[line_length++] = static_cast<char>(byte);
                }
            }
        }
        arm_rx_read();
        if (!made_progress) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
    }
}

} // namespace

extern "C" void usbd_event_handler(uint8_t event)
{
    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        usb_configured = false;
        dtr_enabled = false;
        tx_busy = false;
        rx_armed = false;
        break;
    case USBD_EVENT_CONFIGURED:
        rx_head = 0;
        rx_tail = 0;
        usb_configured = true;
        rx_armed = true;
        if (usbd_ep_start_read(CDC_OUT_EP, rx_dma, sizeof(rx_dma)) != 0) {
            rx_armed = false;
        }
        wake_console_from_isr();
        break;
    default:
        break;
    }
}

extern "C" void usbd_cdc_acm_bulk_out(uint8_t, uint32_t nbytes)
{
    rx_armed = false;
    ring_push(rx_dma, nbytes);
    wake_console_from_isr();
}

extern "C" void usbd_cdc_acm_bulk_in(uint8_t, uint32_t)
{
    tx_busy = false;
    wake_console_from_isr();
}

extern "C" void usbd_cdc_acm_set_dtr(uint8_t, bool dtr)
{
    dtr_enabled = dtr;
    wake_console_from_isr();
}

struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out,
};

struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in,
};

void usb_cdc_console_init(void)
{
    usbd_desc_register(cdc_descriptor);
    usbd_add_interface(usbd_cdc_acm_init_intf(&cdc_control_intf));
    usbd_add_interface(usbd_cdc_acm_init_intf(&cdc_data_intf));
    usbd_add_endpoint(&cdc_out_ep);
    usbd_add_endpoint(&cdc_in_ep);
    usbd_initialize();
}

void usb_cdc_console_start_task(void)
{
    xTaskCreate(cdc_console_task, "usb_cdc", 1536, nullptr, 4,
                &cdc_task_handle);
}
