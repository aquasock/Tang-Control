#include "phosphor_metadata.h"

#include <algorithm>
#include <ctype.h>
#include <string.h>

namespace {

constexpr uint32_t MAX_COMMENT_LENGTH = 4096;
constexpr uint32_t MAX_COMMENT_COUNT = 4096;

uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint32_t read_le32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[3]) << 24) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[1]) << 8) | data[0];
}

bool read_exact(PhosphorMetadataReader &reader, uint8_t *data, size_t length)
{
    return length == 0 || (data != nullptr && reader.read != nullptr &&
                           reader.read(reader.context, data, length));
}

bool seek_to(PhosphorMetadataReader &reader, uint32_t offset)
{
    return offset <= reader.size && reader.seek != nullptr &&
           reader.seek(reader.context, offset);
}

bool skip(PhosphorMetadataReader &reader, uint32_t length)
{
    if (reader.position == nullptr) {
        return false;
    }
    const uint32_t current = reader.position(reader.context);
    return length <= reader.size - std::min(current, reader.size) &&
           seek_to(reader, current + length);
}

std::string uppercase_ascii(const std::string &text)
{
    std::string result = text;
    for (char &byte : result) {
        byte = static_cast<char>(toupper(static_cast<unsigned char>(byte)));
    }
    return result;
}

std::string clean_text(const uint8_t *data, size_t length)
{
    while (length != 0 &&
           (data[length - 1] == 0 || data[length - 1] == ' ' ||
            data[length - 1] == '\t' || data[length - 1] == '\r' ||
            data[length - 1] == '\n')) {
        --length;
    }
    return std::string(reinterpret_cast<const char *>(data), length);
}

bool read_bounded_string(PhosphorMetadataReader &reader, uint32_t length,
                         std::string &text)
{
    if (length > MAX_COMMENT_LENGTH) {
        return skip(reader, length);
    }
    std::string value(length, '\0');
    if (!read_exact(reader, reinterpret_cast<uint8_t *>(&value[0]), length)) {
        return false;
    }
    text = clean_text(reinterpret_cast<const uint8_t *>(value.data()),
                      value.size());
    return true;
}

bool parse_vorbis_comment(PhosphorMetadataReader &reader, uint32_t length,
                          PhosphorAudioMetadata &metadata)
{
    const uint32_t end = reader.position(reader.context) + length;
    if (end < length || end > reader.size || length < 8) {
        return false;
    }
    uint8_t word[4];
    if (!read_exact(reader, word, sizeof(word))) {
        return false;
    }
    const uint32_t vendor_length = read_le32(word);
    if (vendor_length > end - reader.position(reader.context) ||
        !skip(reader, vendor_length) ||
        !read_exact(reader, word, sizeof(word))) {
        return false;
    }
    const uint32_t count = read_le32(word);
    if (count > MAX_COMMENT_COUNT) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (end - reader.position(reader.context) < 4 ||
            !read_exact(reader, word, sizeof(word))) {
            return false;
        }
        const uint32_t comment_length = read_le32(word);
        if (comment_length > end - reader.position(reader.context)) {
            return false;
        }
        std::string comment;
        if (!read_bounded_string(reader, comment_length, comment)) {
            return false;
        }
        if (comment_length > MAX_COMMENT_LENGTH) {
            continue;
        }
        const size_t equals = comment.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = uppercase_ascii(comment.substr(0, equals));
        const std::string value = comment.substr(equals + 1);
        if (key == "ALBUM" && metadata.album.empty()) {
            metadata.album = value;
        } else if (key == "ALBUMARTIST" && metadata.album_artist.empty()) {
            metadata.album_artist = value;
        } else if (key == "ARTIST" && metadata.artist.empty()) {
            metadata.artist = value;
        } else if (key == "TITLE" && metadata.title.empty()) {
            metadata.title = value;
        }
    }
    return seek_to(reader, end);
}

bool parse_picture(PhosphorMetadataReader &reader, uint32_t length,
                   PhosphorAudioMetadata &metadata)
{
    const uint32_t end = reader.position(reader.context) + length;
    if (end < length || end > reader.size || length < 32) {
        return false;
    }
    uint8_t word[4];
    if (!read_exact(reader, word, sizeof(word))) return false;
    const uint32_t type = read_be32(word);
    if (!read_exact(reader, word, sizeof(word))) return false;
    const uint32_t mime_length = read_be32(word);
    if (mime_length > end - reader.position(reader.context)) return false;
    std::string mime;
    if (!read_bounded_string(reader, mime_length, mime)) return false;
    if (mime_length > MAX_COMMENT_LENGTH) mime.clear();

    if (!read_exact(reader, word, sizeof(word))) return false;
    const uint32_t description_length = read_be32(word);
    if (description_length > end - reader.position(reader.context) ||
        !skip(reader, description_length)) return false;

    uint8_t dimensions[20];
    if (!read_exact(reader, dimensions, sizeof(dimensions))) return false;
    const uint32_t width = read_be32(&dimensions[0]);
    const uint32_t height = read_be32(&dimensions[4]);
    const uint32_t data_length = read_be32(&dimensions[16]);
    const uint32_t data_offset = reader.position(reader.context);
    if (data_length > end - data_offset) return false;

    const std::string normalized_mime = uppercase_ascii(mime);
    const bool jpeg = normalized_mime == "IMAGE/JPEG" ||
                      normalized_mime == "IMAGE/JPG";
    const bool preferred = metadata.picture.format == PhosphorPictureFormat::NONE ||
                           (type == 3 && metadata.picture.type != 3);
    if (jpeg && preferred && data_length != 0 && width != 0 && height != 0) {
        metadata.picture = {PhosphorPictureFormat::JPEG, data_offset,
                            data_length, width, height, type};
    }
    return seek_to(reader, end);
}

bool parse_flac(PhosphorMetadataReader &reader,
                PhosphorAudioMetadata &metadata)
{
    bool last = false;
    while (!last) {
        uint8_t header[4];
        if (!read_exact(reader, header, sizeof(header))) return false;
        last = (header[0] & 0x80) != 0;
        const uint8_t type = header[0] & 0x7f;
        const uint32_t length = (static_cast<uint32_t>(header[1]) << 16) |
                                (static_cast<uint32_t>(header[2]) << 8) |
                                header[3];
        if (length > reader.size - reader.position(reader.context)) return false;
        if (type == 4) {
            if (!parse_vorbis_comment(reader, length, metadata)) return false;
        } else if (type == 6) {
            if (!parse_picture(reader, length, metadata)) return false;
        } else if (!skip(reader, length)) {
            return false;
        }
    }
    return true;
}

bool parse_info_list(PhosphorMetadataReader &reader, uint32_t length,
                     PhosphorAudioMetadata &metadata)
{
    const uint32_t end = reader.position(reader.context) + length;
    if (end < length || end > reader.size || length < 4) return false;
    uint8_t kind[4];
    if (!read_exact(reader, kind, sizeof(kind))) return false;
    if (memcmp(kind, "INFO", 4) != 0) return seek_to(reader, end);

    while (reader.position(reader.context) + 8 <= end) {
        uint8_t header[8];
        if (!read_exact(reader, header, sizeof(header))) return false;
        const uint32_t item_length = read_le32(&header[4]);
        if (item_length > end - reader.position(reader.context)) return false;
        std::string value;
        if (!read_bounded_string(reader, item_length, value)) return false;
        if (item_length <= MAX_COMMENT_LENGTH) {
            if (memcmp(header, "INAM", 4) == 0 && metadata.title.empty())
                metadata.title = value;
            else if (memcmp(header, "IART", 4) == 0 && metadata.artist.empty())
                metadata.artist = value;
            else if (memcmp(header, "IPRD", 4) == 0 && metadata.album.empty())
                metadata.album = value;
        }
        if ((item_length & 1u) != 0 &&
            reader.position(reader.context) < end && !skip(reader, 1)) return false;
    }
    return seek_to(reader, end);
}

bool parse_wave(PhosphorMetadataReader &reader,
                PhosphorAudioMetadata &metadata)
{
    while (reader.position(reader.context) + 8 <= reader.size) {
        uint8_t header[8];
        if (!read_exact(reader, header, sizeof(header))) return false;
        const uint32_t length = read_le32(&header[4]);
        if (length > reader.size - reader.position(reader.context)) return false;
        if (memcmp(header, "LIST", 4) == 0) {
            if (!parse_info_list(reader, length, metadata)) return false;
        } else if (!skip(reader, length)) {
            return false;
        }
        if ((length & 1u) != 0 && reader.position(reader.context) < reader.size &&
            !skip(reader, 1)) return false;
    }
    return true;
}

} // namespace

bool phosphor_parse_audio_metadata(PhosphorMetadataReader &reader,
                                   PhosphorAudioMetadata &metadata)
{
    metadata = {};
    if (reader.read == nullptr || reader.seek == nullptr ||
        reader.position == nullptr || reader.size < 4 || !seek_to(reader, 0)) {
        return false;
    }
    uint8_t marker[12] = {};
    const size_t marker_length = std::min<size_t>(sizeof(marker), reader.size);
    if (!read_exact(reader, marker, marker_length)) return false;
    if (marker_length >= 4 && memcmp(marker, "fLaC", 4) == 0) {
        return seek_to(reader, 4) && parse_flac(reader, metadata);
    }
    if (marker_length >= 12 && memcmp(marker, "RIFF", 4) == 0 &&
        memcmp(&marker[8], "WAVE", 4) == 0) {
        return seek_to(reader, 12) && parse_wave(reader, metadata);
    }
    return false;
}
