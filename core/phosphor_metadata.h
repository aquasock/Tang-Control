#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

enum class PhosphorPictureFormat : uint8_t {
    NONE,
    JPEG,
};

struct PhosphorPicture {
    PhosphorPictureFormat format = PhosphorPictureFormat::NONE;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t type = 0;
};

struct PhosphorAudioMetadata {
    std::string album;
    std::string album_artist;
    std::string artist;
    std::string title;
    PhosphorPicture picture;
};

struct PhosphorMetadataReader {
    void *context;
    uint32_t size;
    bool (*read)(void *context, uint8_t *data, size_t length);
    bool (*seek)(void *context, uint32_t offset);
    uint32_t (*position)(void *context);
};

// Parses only bounded display metadata. Audio decoding remains FPGA-owned.
// Unsupported or absent metadata is not an error; malformed containers fail.
bool phosphor_parse_audio_metadata(PhosphorMetadataReader &reader,
                                   PhosphorAudioMetadata &metadata);
