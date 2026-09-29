// Tang-PSX disc service: serves CD image sectors from the SD card to the
// Tang-PSX Gate 1 core (core ID low byte 0x51).
//
// The core's firmware writes a byte offset and length into a debug-register
// mailbox and then advances a sequence number. This task polls the mailbox,
// and answers every new sequence with one FPGA stream session carrying that
// byte range of the disc image. The disc is the first .cue file in the SD
// root; its size in 2352-byte sectors is published to the core.

#include <ctype.h>
#include <string.h>

#include <string>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
}

#include "ff.h"
#include "fpga_debug.h"
#include "fpga_file_stream.h"
#include "utils.h"

extern int16_t active_core;
extern const char *drv;

namespace {

constexpr int16_t CORE_ID = 0x51;
constexpr uint32_t REG_MAGIC = 0x000;
constexpr uint32_t REG_ABI = 0x004;
constexpr uint32_t REG_SEQUENCE = 0x200;
constexpr uint32_t REG_OFFSET = 0x204;
constexpr uint32_t REG_LENGTH = 0x208;
constexpr uint32_t REG_SECTORS = 0x20c;
constexpr uint32_t MAGIC = 0x54505831;          // "TPX1"
constexpr uint32_t DISC_ABI = 0x00020002;
constexpr uint32_t SECTOR_BYTES = 2352;
constexpr uint32_t MAX_REQUEST = 256 * SECTOR_BYTES;

struct DiscState {
    std::string bin;
    uint32_t sectors = 0;
    uint32_t served = 0;
    uint32_t failed = 0;
    uint32_t bytes = 0;
    uint32_t last_sequence = 0;
    uint8_t last_error = 0;
    bool published = false;
} disc;

bool read_reg(uint32_t address, uint32_t &value)
{
    fpga_debug_result result;
    if (!fpga_debug_request(FPGA_EXT_READ32, address, 0, &result) ||
        result.status != 0) {
        return false;
    }
    value = result.data;
    return true;
}

bool write_reg(uint32_t address, uint32_t value)
{
    fpga_debug_result result;
    return fpga_debug_request(FPGA_EXT_WRITE32, address, value, &result) &&
           result.status == 0;
}

bool ends_with_cue(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && name[length - 4] == '.' &&
           tolower(static_cast<unsigned char>(name[length - 3])) == 'c' &&
           tolower(static_cast<unsigned char>(name[length - 2])) == 'u' &&
           tolower(static_cast<unsigned char>(name[length - 1])) == 'e';
}

// Finds the first .cue in the SD root and the .bin named by its FILE line.
bool find_disc(void)
{
    DIR dir;
    FILINFO info;
    std::string cue;
    if (f_opendir(&dir, drv) != FR_OK) {
        return false;
    }
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
        if (!(info.fattrib & AM_DIR) && ends_with_cue(info.fname)) {
            cue = std::string(drv) + info.fname;
            break;
        }
    }
    f_closedir(&dir);
    if (cue.empty()) {
        return false;
    }

    FIL file;
    char text[512];
    UINT count = 0;
    if (f_open(&file, cue.c_str(), FA_READ) != FR_OK) {
        return false;
    }
    const FRESULT status = f_read(&file, text, sizeof(text) - 1, &count);
    f_close(&file);
    if (status != FR_OK) {
        return false;
    }
    text[count] = '\0';
    const char *line = strstr(text, "FILE \"");
    const char *end = line ? strchr(line + 6, '"') : nullptr;
    if (!end) {
        return false;
    }
    disc.bin = std::string(drv) + std::string(line + 6, end);
    if (f_stat(disc.bin.c_str(), &info) != FR_OK) {
        return false;
    }
    disc.sectors = static_cast<uint32_t>(info.fsize / SECTOR_BYTES);
    return disc.sectors != 0;
}

bool gate1_ready(void)
{
    uint32_t magic = 0;
    uint32_t abi = 0;
    return read_reg(REG_MAGIC, magic) && magic == MAGIC &&
           read_reg(REG_ABI, abi) && abi >= DISC_ABI;
}

void serve(uint32_t sequence, uint32_t offset, uint32_t length)
{
    fpga_file_stream_options options;
    options.offset = offset;
    options.length = length;
    const fpga_file_stream_result result =
        fpga_file_stream(disc.bin.c_str(), nullptr, nullptr, options);
    (void)sequence;
    ++disc.served;
    disc.bytes += result.bytes;
    if (result.status != fpga_file_stream_status::OK) {
        ++disc.failed;
        disc.last_error = static_cast<uint8_t>(result.status);
    }
}

void disc_task(void *)
{
    for (;;) {
        if (active_core != CORE_ID) {
            disc.published = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!disc.published) {
            // Publish the disc size whenever Gate 1 (re)appears.
            if (!gate1_ready() || (disc.sectors == 0 && !find_disc()) ||
                !write_reg(REG_SECTORS, disc.sectors) ||
                !read_reg(REG_SEQUENCE, disc.last_sequence)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            disc.published = true;
        }

        uint32_t sequence = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
        uint32_t check = 0;
        uint32_t sectors = 0;
        if (!read_reg(REG_SEQUENCE, sequence)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (sequence == disc.last_sequence) {
            // A reloaded core forgets the disc size: publish it again.
            if (read_reg(REG_SECTORS, sectors) && sectors != disc.sectors) {
                disc.published = false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        // The mailbox crosses clock domains unsynchronized; accept it only
        // when the sequence reads the same before and after its fields.
        if (read_reg(REG_OFFSET, offset) && read_reg(REG_LENGTH, length) &&
            read_reg(REG_SEQUENCE, check) && check == sequence) {
            disc.last_sequence = sequence;
            if (length != 0 && length <= MAX_REQUEST) {
                serve(sequence, offset, length);
            }
        }
    }
}

} // namespace

// One line per field for the USB console's status command.
void tangpsx_disc_status(void (*print)(const char *format, ...))
{
    print("psx_disc: %s\r\n", disc.bin.empty() ? "(none)" : disc.bin.c_str());
    print("psx_disc_sectors: %u\r\n", static_cast<unsigned>(disc.sectors));
    print("psx_disc_published: %s\r\n", disc.published ? "yes" : "no");
    print("psx_disc_requests: %u\r\n", static_cast<unsigned>(disc.served));
    print("psx_disc_failed: %u (last %u)\r\n",
          static_cast<unsigned>(disc.failed),
          static_cast<unsigned>(disc.last_error));
    print("psx_disc_bytes: %u\r\n", static_cast<unsigned>(disc.bytes));
}

void tangpsx_disc_init(void)
{
    xTaskCreate(disc_task, "tangpsx_disc", 4096, nullptr, 2, nullptr);
}
