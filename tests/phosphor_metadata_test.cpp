#include "phosphor_metadata.h"

#include <assert.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

struct MemoryReader {
    std::vector<uint8_t> bytes;
    uint32_t offset = 0;
};

bool memory_read(void *context, uint8_t *data, size_t length)
{
    MemoryReader &reader = *static_cast<MemoryReader *>(context);
    if (length > reader.bytes.size() - reader.offset) return false;
    if (length != 0) memcpy(data, &reader.bytes[reader.offset], length);
    reader.offset += static_cast<uint32_t>(length);
    return true;
}

bool memory_seek(void *context, uint32_t offset)
{
    MemoryReader &reader = *static_cast<MemoryReader *>(context);
    if (offset > reader.bytes.size()) return false;
    reader.offset = offset;
    return true;
}

uint32_t memory_position(void *context)
{
    return static_cast<MemoryReader *>(context)->offset;
}

void append_le32(std::vector<uint8_t> &bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void append_be32(std::vector<uint8_t> &bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 24));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void append_text(std::vector<uint8_t> &bytes, const std::string &text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void append_flac_block(std::vector<uint8_t> &bytes, uint8_t type, bool last,
                       const std::vector<uint8_t> &payload)
{
    bytes.push_back(type | (last ? 0x80 : 0));
    bytes.push_back(static_cast<uint8_t>(payload.size() >> 16));
    bytes.push_back(static_cast<uint8_t>(payload.size() >> 8));
    bytes.push_back(static_cast<uint8_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

PhosphorMetadataReader interface_for(MemoryReader &reader)
{
    return {&reader, static_cast<uint32_t>(reader.bytes.size()), memory_read,
            memory_seek, memory_position};
}

void test_flac_tags_and_front_cover()
{
    MemoryReader reader;
    append_text(reader.bytes, "fLaC");

    std::vector<uint8_t> comments;
    append_le32(comments, 0);
    const std::vector<std::string> values = {
        "ALBUM=The Wall", "ARTIST=Pink Floyd", "ALBUMARTIST=Pink Floyd",
        std::string("TITLE=Don") + "\xe2\x80\x99" + "t Leave Me Now"};
    append_le32(comments, values.size());
    for (const std::string &value : values) {
        append_le32(comments, value.size());
        append_text(comments, value);
    }
    append_flac_block(reader.bytes, 4, false, comments);

    std::vector<uint8_t> picture;
    append_be32(picture, 3);
    append_be32(picture, 10);
    append_text(picture, "image/jpeg");
    append_be32(picture, 0);
    append_be32(picture, 500);
    append_be32(picture, 442);
    append_be32(picture, 24);
    append_be32(picture, 0);
    append_be32(picture, 4);
    const uint32_t expected_offset =
        static_cast<uint32_t>(reader.bytes.size() + 4 + picture.size());
    picture.insert(picture.end(), {0xff, 0xd8, 0xff, 0xd9});
    append_flac_block(reader.bytes, 6, true, picture);

    PhosphorMetadataReader source = interface_for(reader);
    PhosphorAudioMetadata metadata;
    assert(phosphor_parse_audio_metadata(source, metadata));
    assert(metadata.album == "The Wall");
    assert(metadata.artist == "Pink Floyd");
    assert(metadata.album_artist == "Pink Floyd");
    assert(metadata.title == std::string("Don") + "\xe2\x80\x99" +
                                 "t Leave Me Now");
    assert(metadata.picture.format == PhosphorPictureFormat::JPEG);
    assert(metadata.picture.type == 3);
    assert(metadata.picture.width == 500);
    assert(metadata.picture.height == 442);
    assert(metadata.picture.length == 4);
    assert(metadata.picture.offset == expected_offset);
}

void append_info_item(std::vector<uint8_t> &list, const char id[4],
                      const std::string &value)
{
    list.insert(list.end(), id, id + 4);
    append_le32(list, value.size() + 1);
    append_text(list, value);
    list.push_back(0);
    if (((value.size() + 1) & 1u) != 0) list.push_back(0);
}

void test_wave_info_tags()
{
    std::vector<uint8_t> list;
    append_text(list, "INFO");
    append_info_item(list, "INAM", "Wave Track");
    append_info_item(list, "IART", "Wave Artist");
    append_info_item(list, "IPRD", "Wave Album");

    MemoryReader reader;
    append_text(reader.bytes, "RIFF");
    append_le32(reader.bytes, 4 + 8 + list.size());
    append_text(reader.bytes, "WAVE");
    append_text(reader.bytes, "LIST");
    append_le32(reader.bytes, list.size());
    reader.bytes.insert(reader.bytes.end(), list.begin(), list.end());

    PhosphorMetadataReader source = interface_for(reader);
    PhosphorAudioMetadata metadata;
    assert(phosphor_parse_audio_metadata(source, metadata));
    assert(metadata.album == "Wave Album");
    assert(metadata.artist == "Wave Artist");
    assert(metadata.title == "Wave Track");
    assert(metadata.picture.format == PhosphorPictureFormat::NONE);
}

void test_malformed_block_is_rejected()
{
    MemoryReader reader;
    append_text(reader.bytes, "fLaC");
    reader.bytes.insert(reader.bytes.end(), {0x84, 0, 0, 20, 0, 0, 0, 0});
    PhosphorMetadataReader source = interface_for(reader);
    PhosphorAudioMetadata metadata;
    assert(!phosphor_parse_audio_metadata(source, metadata));
}

} // namespace

int main()
{
    test_flac_tags_and_front_cover();
    test_wave_info_tags();
    test_malformed_block_is_rejected();
    std::cout << "PASS: bounded FLAC/WAV metadata and cover discovery\n";
    return 0;
}
