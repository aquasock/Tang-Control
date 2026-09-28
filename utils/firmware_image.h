#pragma once

#include <stddef.h>
#include <stdint.h>

// Console 138K BL616 flash layout, measured from a full 4 MiB readback: the
// vendor loader occupies offset 0 and starts the TangCore application at
// FW_APP_BASE, and a vendor data record sits at 0x200000.  Staging uses an
// erased region between them.
constexpr uint32_t FW_APP_BASE = 0x00040000;
constexpr uint32_t FW_APP_MAX_SIZE = 0x00080000;
constexpr uint32_t FW_STAGING_BASE = 0x00100000;
constexpr uint32_t FW_SECTOR_SIZE = 0x1000;
// The BL616 boot header is 256 bytes with a CRC-32 of bytes 0-251 at 252; its
// image-length field at 0x84 counts bytes after the 4 KiB header region.
constexpr uint32_t FW_BOOT_HEADER_SIZE = 0x100;
constexpr uint32_t FW_HEADER_REGION = 0x1000;

static_assert(FW_APP_BASE + FW_APP_MAX_SIZE <= FW_STAGING_BASE,
              "staging must not overlap the application region");
static_assert(FW_STAGING_BASE + FW_APP_MAX_SIZE <= 0x00200000,
              "staging must stay below the vendor data record");

enum class FwImageError : uint8_t {
    NONE,
    MAGIC,
    HEADER_CRC,
    LENGTH,
};

inline const char *fw_image_error_text(FwImageError error)
{
    switch (error) {
    case FwImageError::NONE: return "ok";
    case FwImageError::MAGIC: return "not a BL616 boot image";
    case FwImageError::HEADER_CRC: return "boot header CRC mismatch";
    case FwImageError::LENGTH: return "image length out of range";
    }
    return "unknown";
}

inline uint32_t fw_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
        }
    }
    return ~crc;
}

inline uint32_t fw_read_le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

// Validate a boot header and report the complete image size it describes.
inline FwImageError fw_check_boot_header(const uint8_t header[FW_BOOT_HEADER_SIZE],
                                         uint32_t *image_size)
{
    if (header[0] != 'B' || header[1] != 'F' || header[2] != 'N' || header[3] != 'P' ||
        header[8] != 'F' || header[9] != 'C' || header[10] != 'F' || header[11] != 'G' ||
        header[0x64] != 'P' || header[0x65] != 'C' || header[0x66] != 'F' ||
        header[0x67] != 'G') {
        return FwImageError::MAGIC;
    }
    if (fw_crc32(header, FW_BOOT_HEADER_SIZE - 4) !=
        fw_read_le32(&header[FW_BOOT_HEADER_SIZE - 4])) {
        return FwImageError::HEADER_CRC;
    }
    const uint32_t body = fw_read_le32(&header[0x84]);
    if (body == 0 || body > FW_APP_MAX_SIZE - FW_HEADER_REGION) {
        return FwImageError::LENGTH;
    }
    *image_size = FW_HEADER_REGION + body;
    return FwImageError::NONE;
}
