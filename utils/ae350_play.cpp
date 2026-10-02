// Resident AE350 (Rockbox) playback over the FPGA stream loader, shared by
// the CDC "play" command and the TangCore Phosphor menu.

#include "ae350_play.h"

#include <string.h>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
}

#include "fpga_debug.h"
#include "fpga_file_stream.h"

namespace {

constexpr char PLAYER_TPI[] = "sd:/ae350/resident.tpi";

bool poke32(uint32_t address, uint32_t value)
{
    fpga_debug_result result;
    return fpga_debug_request(FPGA_EXT_WRITE32, address, value, &result) &&
           result.status == 0;
}

bool read32(uint32_t address, uint32_t *value)
{
    fpga_debug_result result;
    if (!fpga_debug_request(FPGA_EXT_READ32, address, 0, &result) ||
        result.status != 0) {
        return false;
    }
    *value = result.data;
    return true;
}

} // namespace

bool ae350_play_file(const char *full_path, const char **error_out)
{
    // Route the stream/debug to the AE350 and restart its loader.
    if (!poke32(0x000000c0u, 1u) || !poke32(0x000043f0u, 1u)) {
        if (error_out != nullptr)
            *error_out = "AE350 did not respond";
        return false;
    }

    // Wait for the loader to reach WAIT (state 0x01 at debug 0x4020).
    bool waited = false;
    for (int i = 0; i < 100; ++i) {
        uint32_t state = 0;
        if (!read32(0x00004020u, &state)) {
            if (error_out != nullptr)
                *error_out = "AE350 did not respond";
            return false;
        }
        if ((state & 0xffu) == 0x01u) {
            waited = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!waited) {
        poke32(0x000000c0u, 0u);
        if (error_out != nullptr)
            *error_out = "AE350 loader did not reach WAIT";
        return false;
    }

    const fpga_file_stream_result player = fpga_file_stream(PLAYER_TPI);
    if (player.status != fpga_file_stream_status::OK) {
        poke32(0x000000c0u, 0u);
        if (error_out != nullptr)
            *error_out = fpga_file_stream_status_text(player.status);
        return false;
    }

    const fpga_file_stream_result audio = fpga_file_stream(full_path);
    if (audio.status != fpga_file_stream_status::OK) {
        poke32(0x000000c0u, 0u);
        if (error_out != nullptr)
            *error_out = fpga_file_stream_status_text(audio.status);
        return false;
    }

    // Leave cpu_mode set so the AE350's decoded PCM reaches the pcm_sink.
    return true;
}
