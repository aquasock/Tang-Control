#include "cores.h"

#include <stdio.h>

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "ff.h"
}

#include "bflb_mtimer.h"
#include "file_chooser.h"
#include "fpga_debug.h"
#include "fpga_file_stream.h"
#include "m3u_playlist.h"
#include "overlay.h"
#include "utils.h"

namespace {

constexpr uint32_t PHOSPHOR_AUDIO_STATUS = 0x0000005c;
constexpr uint8_t PHOSPHOR_STATE_COMPLETE = 4;
constexpr uint8_t PHOSPHOR_STATE_ERROR = 5;
constexpr uint8_t PHOSPHOR_STATE_CANCELLED = 6;
constexpr size_t PLAYLIST_READ_SIZE = 512;
constexpr uint32_t PLAYER_COMPLETE_TIMEOUT_MS = 5000;

enum class player_status : uint8_t {
    IDLE,
    LOADING,
    PLAYING,
    STOPPED,
    COMPLETE,
    ERROR,
};

enum class player_command : uint8_t {
    NONE,
    LOAD,
    PREVIOUS,
    NEXT,
    STOP,
};

struct player_shared_state {
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    player_status status;
    player_command command;
    uint32_t command_generation;
    std::string pending_path;
    std::string title;
    std::string message;
    uint16_t track;
    uint16_t track_count;
};

player_shared_state player = {};

const char *status_text(player_status status)
{
    switch (status) {
        case player_status::IDLE: return "Idle";
        case player_status::LOADING: return "Loading";
        case player_status::PLAYING: return "Playing";
        case player_status::STOPPED: return "Stopped";
        case player_status::COMPLETE: return "Complete";
        case player_status::ERROR: return "Error";
    }
    return "Unknown";
}

std::string basename_of(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void update_state(player_status status, const std::string &title,
                  const std::string &message, size_t track, size_t track_count)
{
    if (xSemaphoreTake(player.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    player.status = status;
    player.title = title;
    player.message = message;
    player.track = static_cast<uint16_t>(track);
    player.track_count = static_cast<uint16_t>(track_count);
    xSemaphoreGive(player.mutex);
}

void submit_command(player_command command, const std::string &path = {})
{
    if (xSemaphoreTake(player.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    player.command = command;
    if (command == player_command::LOAD) {
        player.pending_path = path;
    }
    ++player.command_generation;
    xSemaphoreGive(player.mutex);
    xTaskNotifyGive(player.task);
}

player_command take_command(std::string &path, uint32_t &generation)
{
    player_command command = player_command::NONE;
    if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
        command = player.command;
        player.command = player_command::NONE;
        if (command == player_command::LOAD) {
            path = player.pending_path;
        }
        generation = player.command_generation;
        xSemaphoreGive(player.mutex);
    }
    return command;
}

bool command_pending(void *context)
{
    const uint32_t generation = *static_cast<const uint32_t *>(context);
    bool pending = false;
    if (xSemaphoreTake(player.mutex, 0) == pdTRUE) {
        pending = player.command != player_command::NONE ||
                  player.command_generation != generation;
        xSemaphoreGive(player.mutex);
    }
    return pending;
}

bool parse_playlist(const std::string &path, std::vector<M3uEntry> &entries,
                    std::string &error)
{
    FIL file;
    FRESULT result = f_open(&file, path.c_str(), FA_READ);
    if (result != FR_OK) {
        error = "Cannot open playlist";
        return false;
    }

    M3uParser parser(path);
    uint8_t buffer[PLAYLIST_READ_SIZE];
    while (parser.error().empty()) {
        UINT count = 0;
        result = f_read(&file, buffer, sizeof(buffer), &count);
        if (result != FR_OK) {
            error = "Cannot read playlist";
            break;
        }
        if (count == 0) {
            if (!parser.finish()) {
                error = parser.error();
            }
            break;
        }
        if (!parser.feed(buffer, count)) {
            error = parser.error();
            break;
        }
    }
    const FRESULT close_result = f_close(&file);
    if (error.empty() && close_result != FR_OK) {
        error = "Cannot close playlist";
    }
    if (!error.empty()) {
        return false;
    }

    entries = parser.entries();
    for (const M3uEntry &entry : entries) {
        if (!m3u_path_has_extension(entry.path, ".wav") &&
            !m3u_path_has_extension(entry.path, ".flac")) {
            error = "Playlist contains an unsupported track";
            return false;
        }
        FILINFO info;
        if (f_stat(entry.path.c_str(), &info) != FR_OK ||
            (info.fattrib & AM_DIR) != 0 || info.fsize == 0) {
            error = "Playlist track is missing or empty";
            return false;
        }
    }
    return true;
}

bool load_selection(const std::string &path, std::vector<M3uEntry> &entries,
                    std::string &error)
{
    entries.clear();
    if (m3u_path_has_extension(path, ".wav") ||
        m3u_path_has_extension(path, ".flac")) {
        FILINFO info;
        if (f_stat(path.c_str(), &info) != FR_OK ||
            (info.fattrib & AM_DIR) != 0 || info.fsize == 0) {
            error = "Audio file is missing or empty";
            return false;
        }
        entries.push_back({path, basename_of(path), -1});
        return true;
    }
    if (m3u_path_has_extension(path, ".m3u") ||
        m3u_path_has_extension(path, ".m3u8")) {
        return parse_playlist(path, entries, error);
    }
    error = "Choose WAV, FLAC, M3U, or M3U8";
    return false;
}

bool wait_for_player_complete(uint32_t generation, std::string &error)
{
    const uint64_t started = bflb_mtimer_get_time_ms();
    while (!command_pending(&generation)) {
        fpga_debug_result result;
        if (!fpga_debug_request(FPGA_EXT_READ32, PHOSPHOR_AUDIO_STATUS, 0,
                                &result) || result.status != 0) {
            error = "Playback status timed out";
            return false;
        }
        const uint8_t state = static_cast<uint8_t>(result.data & 0x0f);
        if (state == PHOSPHOR_STATE_COMPLETE) {
            return true;
        }
        if (state == PHOSPHOR_STATE_ERROR) {
            const uint8_t code = static_cast<uint8_t>((result.data >> 6) & 0xff);
            char message[40];
            snprintf(message, sizeof(message), "Audio decoder error %u",
                     static_cast<unsigned>(code));
            error = message;
            return false;
        }
        if (state == PHOSPHOR_STATE_CANCELLED) {
            error = "Playback cancelled by core";
            return false;
        }
        if (bflb_mtimer_get_time_ms() - started > PLAYER_COMPLETE_TIMEOUT_MS) {
            error = "Playback completion timed out";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

void report_error(const std::string &message, size_t track, size_t count)
{
    update_state(player_status::ERROR, {}, message, track, count);
    overlay_status("Phosphor: %s", message.c_str());
}

void player_task(void *)
{
    std::vector<M3uEntry> playlist;
    size_t current = 0;
    for (;;) {
        if (playlist.empty()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        std::string command_path;
        uint32_t generation = 0;
        player_command command = take_command(command_path, generation);
        if (command == player_command::LOAD) {
            update_state(player_status::LOADING, basename_of(command_path), {}, 0, 0);
            std::string error;
            if (!load_selection(command_path, playlist, error)) {
                playlist.clear();
                report_error(error, 0, 0);
                continue;
            }
            current = 0;
        } else if (command == player_command::STOP) {
            playlist.clear();
            update_state(player_status::STOPPED, {}, "Playback stopped", 0, 0);
            continue;
        } else if (command == player_command::NEXT) {
            if (!playlist.empty() && current + 1 < playlist.size()) {
                ++current;
            } else {
                playlist.clear();
                update_state(player_status::COMPLETE, {}, "End of playlist", 0, 0);
                continue;
            }
        } else if (command == player_command::PREVIOUS) {
            if (!playlist.empty() && current > 0) {
                --current;
            }
        } else if (playlist.empty()) {
            continue;
        }

        const M3uEntry entry = playlist[current];
        update_state(player_status::PLAYING, entry.title, {}, current + 1,
                     playlist.size());
        const fpga_file_stream_result stream =
            fpga_file_stream(entry.path.c_str(), command_pending, &generation);
        if (stream.status == fpga_file_stream_status::CANCELLED) {
            continue;
        }
        if (stream.status != fpga_file_stream_status::OK) {
            const std::string error = fpga_file_stream_status_text(stream.status);
            playlist.clear();
            report_error(error, current + 1, 0);
            continue;
        }

        std::string error;
        if (!wait_for_player_complete(generation, error)) {
            if (command_pending(&generation)) {
                continue;
            }
            playlist.clear();
            report_error(error, current + 1, 0);
            continue;
        }

        if (current + 1 < playlist.size()) {
            ++current;
        } else {
            const size_t count = playlist.size();
            const std::string final_title = entry.title;
            playlist.clear();
            update_state(player_status::COMPLETE, final_title,
                         "Playlist complete", count, count);
        }
    }
}

struct PhosphorMenu : Menu {
    explicit PhosphorMenu(const char *directory) : directory_(directory) {}

    void render() override
    {
        player_status status;
        std::string title;
        std::string message;
        uint16_t track;
        uint16_t count;
        if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
            status = player.status;
            title = player.title;
            message = player.message;
            track = player.track;
            count = player.track_count;
            xSemaphoreGive(player.mutex);
        } else {
            status = player_status::ERROR;
            message = "State unavailable";
            track = count = 0;
        }

        overlay_clear();
        overlay_cursor(0, 6);
        overlay_printf("       --- Phosphor ---");
        overlay_cursor(0, 8);
        overlay_printf("Status: %s", status_text(status));
        overlay_cursor(0, 9);
        if (count != 0)
            overlay_printf("Track: %u/%u", static_cast<unsigned>(track),
                           static_cast<unsigned>(count));
        overlay_cursor(0, 10);
        overlay_printf("%.31s", title.empty() ? message.c_str() : title.c_str());
        overlay_cursor(0, 12);
        overlay_printf("  Load Audio / Playlist");
        overlay_cursor(0, 13);
        overlay_printf("  Previous Track");
        overlay_cursor(0, 14);
        overlay_printf("  Next Track");
        overlay_cursor(0, 15);
        overlay_printf("  Stop Playback");
        overlay_cursor(0, 17);
        overlay_printf("  << Main Menu");
    }

    std::vector<int> get_options() override
    {
        return {12, 13, 14, 15, 17};
    }

    bool on_choose(int index) override
    {
        if (index == 0) {
            submit_command(player_command::STOP);
            const uint64_t deadline = bflb_mtimer_get_time_ms() + 2000;
            while (bflb_mtimer_get_time_ms() < deadline) {
                player_status status;
                if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
                    status = player.status;
                    xSemaphoreGive(player.mutex);
                } else {
                    break;
                }
                if (status != player_status::PLAYING &&
                    status != player_status::LOADING) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            FileChooser chooser;
            chooser.rootdir = directory_;
            chooser.curdir = directory_;
            chooser.msg_return = "<< Cancel";
            chooser.extensions = {".wav", ".flac", ".m3u", ".m3u8"};
            std::string path;
            const bool selected = chooser.choose_file(path);
            do_redraw();
            if (selected) {
                submit_command(player_command::LOAD, path);
                overlay(0);
                return true;
            }
            return false;
        }
        if (index == 1) {
            submit_command(player_command::PREVIOUS);
            overlay(0);
            return true;
        }
        if (index == 2) {
            submit_command(player_command::NEXT);
            overlay(0);
            return true;
        }
        if (index == 3) {
            submit_command(player_command::STOP);
            do_redraw();
            return false;
        }
        if (index == 4) {
            submit_command(player_command::STOP);
            return true;
        }
        return false;
    }

private:
    std::string directory_;
};

} // namespace

void phosphor_player_init(void)
{
    player.mutex = xSemaphoreCreateMutex();
    player.status = player_status::IDLE;
    player.command = player_command::NONE;
    player.command_generation = 0;
    xTaskCreate(player_task, "phosphor_player", 4096, nullptr, 2, &player.task);
}

int loadphosphor(const char *filename)
{
    if (filename == nullptr) {
        return -1;
    }
    submit_command(player_command::LOAD, filename);
    overlay(0);
    return 0;
}

Menu *create_phosphor_menu(const char *directory)
{
    return new PhosphorMenu(directory);
}
