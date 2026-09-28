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
#include "phosphor_artwork.h"
#include "phosphor_ui_model.h"
#include "utils.h"

namespace {

constexpr uint32_t PHOSPHOR_AUDIO_STATUS = 0x0000005c;
constexpr uint32_t PHOSPHOR_PLAYBACK_CONTROL = 0x00000078;
constexpr uint32_t PHOSPHOR_UI_CONTROL = 0x0000007c;
constexpr uint32_t PHOSPHOR_UI_PLAYLIST_STATE = 0x00000080;
constexpr uint32_t PHOSPHOR_UI_LENGTHS_0_3 = 0x00000084;
constexpr uint32_t PHOSPHOR_UI_LENGTHS_4_7 = 0x00000088;
constexpr uint32_t PHOSPHOR_UI_LENGTH_8 = 0x00000094;
constexpr uint32_t PHOSPHOR_UI_ART_CONTROL = 0x00000098;
constexpr uint32_t PHOSPHOR_UI_TEXT_BASE = 0x00000100;
constexpr uint32_t PHOSPHOR_UI_ART_BASE = 0x00001000;
constexpr uint32_t PHOSPHOR_UI_COMMIT = 0x80000000;
constexpr uint32_t PHOSPHOR_UI_WRITE_TIMEOUT_MS = 1500;
constexpr uint16_t BUTTON_START = 0x0008;
constexpr uint16_t BUTTON_LEFT = 0x0040;
constexpr uint16_t BUTTON_RIGHT = 0x0080;
constexpr uint16_t BUTTON_X = 0x0200;
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
    TaskHandle_t ui_task;
    TaskHandle_t control_task;
    player_status status;
    player_command command;
    uint32_t command_generation;
    std::string pending_path;
    std::string title;
    std::string message;
    uint16_t track;
    uint16_t track_count;
    bool paused;
    bool ui_visible;
    bool ui_playlist;
    uint32_t ui_generation;
    PhosphorUiSnapshot ui_snapshot;
    std::string ui_media_path;
};

player_shared_state player = {};
uint8_t artwork_pixels[PHOSPHOR_ART_BYTES];

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

bool write_core_register(uint32_t address, uint32_t value)
{
    fpga_debug_result result;
    // A backpressured 1 KiB audio frame may own the shared UART for up to one
    // second.  Stay queued through that bounded transaction instead of losing
    // a UI commit or controller edge at the generic 250 ms timeout.
    return fpga_debug_request(FPGA_EXT_WRITE32, address, value, &result,
                              PHOSPHOR_UI_WRITE_TIMEOUT_MS) &&
           result.status == 0;
}

void set_paused(bool paused)
{
    if (paused) {
        // Publish the software pause first.  The stream task checks this state
        // between acknowledged frames, so it cannot start another DATA frame
        // after the FPGA pause write releases the shared link.
        if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
            player.paused = true;
            xSemaphoreGive(player.mutex);
        }
        if (!write_core_register(PHOSPHOR_PLAYBACK_CONTROL, 1) &&
            xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
            player.paused = false;
            xSemaphoreGive(player.mutex);
        }
        return;
    }

    // Keep the software stream stopped until the FPGA is consuming PCM again.
    // Otherwise a DATA frame can win the link race while the FIFO is paused.
    if (write_core_register(PHOSPHOR_PLAYBACK_CONTROL, 0) &&
        xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
        player.paused = false;
        xSemaphoreGive(player.mutex);
    }
}

void queue_ui_snapshot(const std::string &selection_name,
                       const std::vector<M3uEntry> &entries, size_t current,
                       bool playlist, bool make_visible)
{
    const PhosphorUiSnapshot snapshot = phosphor_build_ui_snapshot(
        selection_name, entries, current, playlist);
    if (xSemaphoreTake(player.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    player.ui_snapshot = snapshot;
    player.ui_media_path = entries.empty() ? std::string() : entries[current].path;
    player.ui_playlist = snapshot.playlist;
    if (make_visible) {
        player.ui_visible = true;
    }
    ++player.ui_generation;
    xSemaphoreGive(player.mutex);
    xTaskNotifyGive(player.ui_task);
}

uint32_t pack_lengths(const PhosphorUiSnapshot &snapshot, size_t first)
{
    return (static_cast<uint32_t>(snapshot.length[first]) << 24) |
           (static_cast<uint32_t>(snapshot.length[first + 1]) << 16) |
           (static_cast<uint32_t>(snapshot.length[first + 2]) << 8) |
           snapshot.length[first + 3];
}

bool write_ui_snapshot(const PhosphorUiSnapshot &snapshot, uint32_t generation)
{
    for (size_t slot = 0; slot < PHOSPHOR_UI_SLOT_COUNT; ++slot) {
        for (size_t offset = 0; offset < PHOSPHOR_UI_SLOT_BYTES; offset += 4) {
            const uint32_t value =
                (static_cast<uint32_t>(snapshot.text[slot][offset]) << 24) |
                (static_cast<uint32_t>(snapshot.text[slot][offset + 1]) << 16) |
                (static_cast<uint32_t>(snapshot.text[slot][offset + 2]) << 8) |
                snapshot.text[slot][offset + 3];
            const uint32_t address = PHOSPHOR_UI_TEXT_BASE +
                static_cast<uint32_t>(slot * PHOSPHOR_UI_SLOT_BYTES + offset);
            if (!write_core_register(address, value)) {
                return false;
            }
        }
    }
    const uint32_t playlist_state =
        (static_cast<uint32_t>(snapshot.window_start) << 16) |
        (static_cast<uint32_t>(snapshot.track_count) << 8) |
        snapshot.current_track;
    if (!write_core_register(PHOSPHOR_UI_PLAYLIST_STATE, playlist_state) ||
        !write_core_register(PHOSPHOR_UI_LENGTHS_0_3,
                             pack_lengths(snapshot, 0)) ||
        !write_core_register(PHOSPHOR_UI_LENGTHS_4_7,
                             pack_lengths(snapshot, 4)) ||
        !write_core_register(PHOSPHOR_UI_LENGTH_8,
                             static_cast<uint32_t>(snapshot.length[8]) << 24)) {
        return false;
    }

    bool visible;
    bool latest;
    if (xSemaphoreTake(player.mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    visible = player.ui_visible;
    latest = player.ui_generation == generation;
    xSemaphoreGive(player.mutex);
    if (!latest) {
        return true;
    }
    return write_core_register(PHOSPHOR_UI_CONTROL, PHOSPHOR_UI_COMMIT |
        (snapshot.playlist ? 2u : 0u) | (visible ? 1u : 0u));
}

bool ui_generation_current(uint32_t generation)
{
    bool current = false;
    if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
        current = generation == player.ui_generation;
        xSemaphoreGive(player.mutex);
    }
    return current;
}

bool write_artwork(uint32_t generation)
{
    for (size_t offset = 0; offset < PHOSPHOR_ART_BYTES; offset += 4) {
        if ((offset & 0x7f) == 0 && !ui_generation_current(generation)) {
            return true;
        }
        const uint32_t value =
            (static_cast<uint32_t>(artwork_pixels[offset]) << 24) |
            (static_cast<uint32_t>(artwork_pixels[offset + 1]) << 16) |
            (static_cast<uint32_t>(artwork_pixels[offset + 2]) << 8) |
            artwork_pixels[offset + 3];
        if (!write_core_register(PHOSPHOR_UI_ART_BASE + offset, value)) {
            return false;
        }
        if ((offset & 0x1ff) == 0x1fc) {
            vTaskDelay(1);
        }
    }
    if (!ui_generation_current(generation)) return true;
    return write_core_register(PHOSPHOR_UI_ART_CONTROL,
                               PHOSPHOR_UI_COMMIT | 1u);
}

void ui_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            PhosphorUiSnapshot snapshot;
            std::string media_path;
            uint32_t generation;
            if (xSemaphoreTake(player.mutex, portMAX_DELAY) != pdTRUE) {
                break;
            }
            snapshot = player.ui_snapshot;
            media_path = player.ui_media_path;
            generation = player.ui_generation;
            xSemaphoreGive(player.mutex);

            bool written = write_core_register(PHOSPHOR_UI_ART_CONTROL, 0);
            if (written && ui_generation_current(generation)) {
                written = write_ui_snapshot(snapshot, generation);
            }
            PhosphorAudioMetadata metadata;
            if (written && ui_generation_current(generation) &&
                phosphor_read_file_metadata(media_path.c_str(), metadata)) {
                phosphor_apply_track_metadata(snapshot, metadata);
                written = write_ui_snapshot(snapshot, generation);
                if (written && ui_generation_current(generation) &&
                    metadata.picture.format == PhosphorPictureFormat::JPEG &&
                    phosphor_decode_artwork(media_path.c_str(), metadata.picture,
                                            artwork_pixels,
                                            sizeof(artwork_pixels))) {
                    written = write_artwork(generation);
                }
            }
            if (!written) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            bool current;
            if (xSemaphoreTake(player.mutex, portMAX_DELAY) != pdTRUE) {
                break;
            }
            current = generation == player.ui_generation;
            xSemaphoreGive(player.mutex);
            if (current && written) {
                break;
            }
        }
    }
}

void control_task(void *)
{
    uint16_t previous = 0;
    for (;;) {
        uint16_t joy1 = 0, joy2 = 0, hid1 = 0, hid2 = 0;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        const uint16_t buttons = joy1 | joy2 | hid1 | hid2;
        const bool osd_chord = joy1 == OSD_KEY_CODE || joy2 == OSD_KEY_CODE ||
                               hid1 == OSD_KEY_CODE || hid2 == OSD_KEY_CODE;
        if (overlay_on() || osd_chord) {
            previous = buttons;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const uint16_t pressed = buttons & ~previous;
        previous = buttons;
        bool active = false;
        bool paused = false;
        bool visible = false;
        bool playlist = false;
        uint16_t track = 0;
        uint16_t count = 0;
        if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
            active = player.status == player_status::PLAYING ||
                     player.status == player_status::LOADING;
            paused = player.paused;
            visible = player.ui_visible;
            playlist = player.ui_playlist;
            track = player.track;
            count = player.track_count;
            xSemaphoreGive(player.mutex);
        }

        if (active && (pressed & BUTTON_START)) {
            set_paused(!paused);
        }
        if (active && playlist && track > 1 && (pressed & BUTTON_LEFT)) {
            submit_command(player_command::PREVIOUS);
        }
        if (active && playlist && track < count && (pressed & BUTTON_RIGHT)) {
            submit_command(player_command::NEXT);
        }
        if (count != 0 && (pressed & BUTTON_X)) {
            const bool next_visible = !visible;
            if (write_core_register(PHOSPHOR_UI_CONTROL,
                    (playlist ? 2u : 0u) | (next_visible ? 1u : 0u))) {
                if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
                    player.ui_visible = next_visible;
                    xSemaphoreGive(player.mutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

bool stream_cancel_requested(void *context)
{
    const uint32_t generation = *static_cast<const uint32_t *>(context);
    for (;;) {
        bool pending = false;
        bool paused = false;
        if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
            pending = player.command != player_command::NONE ||
                      player.command_generation != generation;
            paused = player.paused;
            xSemaphoreGive(player.mutex);
        }
        if (pending || !paused) {
            return pending;
        }
        // Controller and UI tasks remain runnable, so Start can unpause and a
        // track command can cancel this stream without filling the FPGA FIFO.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
    uint64_t started = bflb_mtimer_get_time_ms();
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
        bool paused = false;
        if (xSemaphoreTake(player.mutex, portMAX_DELAY) == pdTRUE) {
            paused = player.paused;
            xSemaphoreGive(player.mutex);
        }
        const uint64_t now = bflb_mtimer_get_time_ms();
        if (paused) {
            started = now;
        } else if (now - started > PLAYER_COMPLETE_TIMEOUT_MS) {
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
    std::string selection_name;
    bool playlist_mode = false;
    for (;;) {
        bool make_ui_visible = false;
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
            selection_name = command_path;
            playlist_mode = m3u_path_has_extension(command_path, ".m3u") ||
                            m3u_path_has_extension(command_path, ".m3u8");
            current = 0;
            set_paused(false);
            make_ui_visible = true;
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

        set_paused(false);
        queue_ui_snapshot(selection_name, playlist, current, playlist_mode,
                          make_ui_visible);

        const M3uEntry entry = playlist[current];
        update_state(player_status::PLAYING, entry.title, {}, current + 1,
                     playlist.size());
        const fpga_file_stream_result stream =
            fpga_file_stream(entry.path.c_str(), stream_cancel_requested,
                             &generation);
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
        overlay_clear();
        overlay_cursor(0, 8);
        overlay_printf("       --- Phosphor ---");
        overlay_cursor(0, 12);
        overlay_printf("  Load Audio / Playlist");
        overlay_cursor(0, 14);
        overlay_printf("  << Main Menu");
    }

    std::vector<int> get_options() override
    {
        return {12, 14};
    }

    bool on_choose(int index) override
    {
        if (index == 0) {
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
    player.paused = false;
    player.ui_visible = false;
    player.ui_playlist = false;
    player.ui_generation = 0;
    xTaskCreate(player_task, "phosphor_player", 4096, nullptr, 2, &player.task);
    // Keep metadata work at the player's priority so it receives time slices
    // during a continuous file transfer.  The shared FPGA-link mutex prevents
    // its register transactions from overlapping acknowledged audio frames.
    xTaskCreate(ui_task, "phosphor_ui", 4096, nullptr, 2, &player.ui_task);
    xTaskCreate(control_task, "phosphor_controls", 2048, nullptr, 2,
                &player.control_task);
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
