#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "m3u_playlist.h"
#include "phosphor_metadata.h"

constexpr size_t PHOSPHOR_UI_SLOT_COUNT = 9;
constexpr size_t PHOSPHOR_UI_SLOT_BYTES = 32;

struct PhosphorUiSnapshot {
    bool playlist;
    uint8_t current_track;
    uint8_t track_count;
    uint8_t window_start;
    std::array<std::array<uint8_t, PHOSPHOR_UI_SLOT_BYTES>,
               PHOSPHOR_UI_SLOT_COUNT> text;
    std::array<uint8_t, PHOSPHOR_UI_SLOT_COUNT> length;
};

PhosphorUiSnapshot phosphor_build_ui_snapshot(
    const std::string &selection_name, const std::vector<M3uEntry> &entries,
    size_t current, bool playlist);

void phosphor_apply_track_metadata(PhosphorUiSnapshot &snapshot,
                                   const PhosphorAudioMetadata &metadata);
