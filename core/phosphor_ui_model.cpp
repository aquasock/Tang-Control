#include "phosphor_ui_model.h"

#include <algorithm>
#include <stdio.h>

namespace {

constexpr uint32_t REPLACEMENT_CODE_POINT = 0xfffd;

uint32_t next_utf8_code_point(const std::string &text, size_t &offset)
{
    const uint8_t first = static_cast<uint8_t>(text[offset]);
    if (first < 0x80) {
        ++offset;
        return first;
    }

    size_t width = 0;
    uint32_t code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        width = 2;
        code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        width = 3;
        code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        width = 4;
        code_point = first & 0x07;
    } else {
        ++offset;
        return REPLACEMENT_CODE_POINT;
    }

    if (offset + width > text.size()) {
        ++offset;
        return REPLACEMENT_CODE_POINT;
    }
    for (size_t index = 1; index < width; ++index) {
        const uint8_t byte = static_cast<uint8_t>(text[offset + index]);
        if ((byte & 0xc0) != 0x80) {
            ++offset;
            return REPLACEMENT_CODE_POINT;
        }
        code_point = (code_point << 6) | (byte & 0x3f);
    }

    const bool overlong = (width == 2 && code_point < 0x80) ||
                          (width == 3 && code_point < 0x800) ||
                          (width == 4 && code_point < 0x10000);
    const bool surrogate = code_point >= 0xd800 && code_point <= 0xdfff;
    if (overlong || surrogate || code_point > 0x10ffff) {
        ++offset;
        return REPLACEMENT_CODE_POINT;
    }
    offset += width;
    return code_point;
}

std::string display_name(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        name.resize(dot);
    }
    return name;
}

void set_slot(PhosphorUiSnapshot &snapshot, size_t slot,
              const std::string &text)
{
    snapshot.text[slot].fill(0);
    snapshot.length[slot] = 0;
    size_t input_offset = 0;
    size_t output_offset = 0;
    const auto append = [&](uint8_t byte) {
        if (output_offset < PHOSPHOR_UI_SLOT_BYTES - 1) {
            snapshot.text[slot][output_offset++] = byte;
        }
    };

    while (input_offset < text.size() &&
           output_offset < PHOSPHOR_UI_SLOT_BYTES - 1) {
        const uint32_t code_point = next_utf8_code_point(text, input_offset);
        if (code_point >= 0x20 && code_point <= 0x7e) {
            append(static_cast<uint8_t>(code_point));
        } else {
            switch (code_point) {
            case 0x00a0: append(' '); break;
            case 0x2018:
            case 0x2019: append('\''); break;
            case 0x201c:
            case 0x201d: append('"'); break;
            case 0x2013:
            case 0x2014:
            case 0x2212: append('-'); break;
            case 0x2026:
                append('.'); append('.'); append('.');
                break;
            default: append('?'); break;
            }
        }
    }
    snapshot.length[slot] = static_cast<uint8_t>(output_offset);
}

std::string numbered_title(size_t number, const std::string &title)
{
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "%03u  ", static_cast<unsigned>(number));
    return std::string(prefix) + title;
}

void split_playlist_title(const std::string &text, std::string &artist,
                          std::string &title)
{
    const size_t separator = text.find(" - ");
    if (separator == std::string::npos) {
        title = text;
        return;
    }
    artist = text.substr(0, separator);
    title = text.substr(separator + 3);
}

} // namespace

PhosphorUiSnapshot phosphor_build_ui_snapshot(
    const std::string &selection_name, const std::vector<M3uEntry> &entries,
    size_t current, bool playlist)
{
    PhosphorUiSnapshot snapshot = {};
    snapshot.playlist = playlist && entries.size() > 1;
    if (entries.empty()) {
        return snapshot;
    }

    current = std::min(current, entries.size() - 1);
    snapshot.current_track = static_cast<uint8_t>(current + 1);
    snapshot.track_count = static_cast<uint8_t>(
        std::min(entries.size(), static_cast<size_t>(UINT8_MAX)));

    size_t window_start = 0;
    if (snapshot.playlist && entries.size() > 6) {
        if (current > 2) {
            window_start = current - 2;
        }
        if (window_start + 6 > entries.size()) {
            window_start = entries.size() - 6;
        }
    }
    snapshot.window_start = static_cast<uint8_t>(window_start + 1);

    std::string fallback_artist;
    std::string fallback_title;
    split_playlist_title(entries[current].title, fallback_artist,
                         fallback_title);
    if (!snapshot.playlist) {
        fallback_title = display_name(fallback_title);
    }
    set_slot(snapshot, 0, display_name(selection_name));
    set_slot(snapshot, 1, fallback_artist);
    set_slot(snapshot, 2, fallback_title);
    for (size_t row = 0; row < 6; ++row) {
        const size_t index = window_start + row;
        if (snapshot.playlist && index < entries.size()) {
            set_slot(snapshot, row + 3,
                     numbered_title(index + 1, entries[index].title));
        }
    }
    return snapshot;
}

void phosphor_apply_track_metadata(PhosphorUiSnapshot &snapshot,
                                   const PhosphorAudioMetadata &metadata)
{
    if (!metadata.album.empty()) {
        set_slot(snapshot, 0, metadata.album);
    }
    if (!metadata.album_artist.empty()) {
        set_slot(snapshot, 1, metadata.album_artist);
    } else if (!metadata.artist.empty()) {
        set_slot(snapshot, 1, metadata.artist);
    }
    if (!metadata.title.empty()) {
        set_slot(snapshot, 2, metadata.title);
    }
}
