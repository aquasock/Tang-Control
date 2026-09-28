#include "phosphor_artwork.h"

#include <algorithm>
#include <limits.h>
#include <string.h>

extern "C" {
#include "ff.h"
#include "tjpgd.h"
}

namespace {

constexpr size_t JPEG_WORK_BYTES = 4096;

struct FatFsReader {
    FIL file;
};

bool fatfs_read(void *context, uint8_t *data, size_t length)
{
    FatFsReader &reader = *static_cast<FatFsReader *>(context);
    while (length != 0) {
        UINT count = 0;
        const UINT request = static_cast<UINT>(
            std::min<size_t>(length, static_cast<size_t>(UINT_MAX)));
        if (f_read(&reader.file, data, request, &count) != FR_OK ||
            count != request) {
            return false;
        }
        data += count;
        length -= count;
    }
    return true;
}

bool fatfs_seek(void *context, uint32_t offset)
{
    FatFsReader &reader = *static_cast<FatFsReader *>(context);
    return f_lseek(&reader.file, offset) == FR_OK;
}

uint32_t fatfs_position(void *context)
{
    return static_cast<uint32_t>(f_tell(&static_cast<FatFsReader *>(context)->file));
}

struct JpegDecode {
    FIL file;
    uint32_t remaining;
    uint16_t scaled_width;
    uint16_t scaled_height;
    uint16_t source_x[PHOSPHOR_ART_WIDTH];
    uint16_t source_y[PHOSPHOR_ART_HEIGHT];
    uint8_t *output;
    bool io_error;
};

alignas(4) uint8_t jpeg_work[JPEG_WORK_BYTES];
JDEC jpeg_decoder;

size_t jpeg_input(JDEC *decoder, uint8_t *buffer, size_t length)
{
    JpegDecode &source = *static_cast<JpegDecode *>(decoder->device);
    length = std::min<size_t>(length, source.remaining);
    if (length == 0) return 0;
    if (buffer == nullptr) {
        const FSIZE_t next = f_tell(&source.file) + length;
        if (f_lseek(&source.file, next) != FR_OK) {
            source.io_error = true;
            return 0;
        }
        source.remaining -= static_cast<uint32_t>(length);
        return length;
    }
    UINT count = 0;
    if (f_read(&source.file, buffer, static_cast<UINT>(length), &count) != FR_OK) {
        source.io_error = true;
        return 0;
    }
    source.remaining -= count;
    return count;
}

uint8_t rgb332(const uint8_t *pixel)
{
    return static_cast<uint8_t>((pixel[0] & 0xe0) |
                                ((pixel[1] >> 3) & 0x1c) |
                                (pixel[2] >> 6));
}

int jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle)
{
    JpegDecode &target = *static_cast<JpegDecode *>(decoder->device);
    const uint16_t rectangle_width = rectangle->right - rectangle->left + 1;
    const uint8_t *pixels = static_cast<const uint8_t *>(bitmap);

    size_t first_y = 0;
    while (first_y < PHOSPHOR_ART_HEIGHT &&
           target.source_y[first_y] < rectangle->top) ++first_y;
    size_t end_y = first_y;
    while (end_y < PHOSPHOR_ART_HEIGHT &&
           target.source_y[end_y] <= rectangle->bottom) ++end_y;
    size_t first_x = 0;
    while (first_x < PHOSPHOR_ART_WIDTH &&
           target.source_x[first_x] < rectangle->left) ++first_x;
    size_t end_x = first_x;
    while (end_x < PHOSPHOR_ART_WIDTH &&
           target.source_x[end_x] <= rectangle->right) ++end_x;

    for (size_t y = first_y; y < end_y; ++y) {
        const size_t source_row = target.source_y[y] - rectangle->top;
        for (size_t x = first_x; x < end_x; ++x) {
            const size_t source_column = target.source_x[x] - rectangle->left;
            const uint8_t *pixel = pixels +
                (source_row * rectangle_width + source_column) * 3;
            target.output[y * PHOSPHOR_ART_WIDTH + x] = rgb332(pixel);
        }
    }
    return 1;
}

} // namespace

bool phosphor_read_file_metadata(const char *path,
                                 PhosphorAudioMetadata &metadata)
{
    if (path == nullptr || path[0] == '\0') return false;
    FatFsReader reader = {};
    if (f_open(&reader.file, path, FA_READ) != FR_OK) return false;
    const FSIZE_t file_size = f_size(&reader.file);
    bool parsed = false;
    if (file_size <= UINT32_MAX) {
        PhosphorMetadataReader source = {
            &reader, static_cast<uint32_t>(file_size), fatfs_read,
            fatfs_seek, fatfs_position};
        parsed = phosphor_parse_audio_metadata(source, metadata);
    }
    if (f_close(&reader.file) != FR_OK) parsed = false;
    return parsed;
}

bool phosphor_decode_artwork(const char *path, const PhosphorPicture &picture,
                            uint8_t *output, size_t length)
{
    if (path == nullptr || output == nullptr || length < PHOSPHOR_ART_BYTES ||
        picture.format != PhosphorPictureFormat::JPEG || picture.length == 0) {
        return false;
    }

    JpegDecode source = {};
    source.remaining = picture.length;
    source.output = output;
    if (f_open(&source.file, path, FA_READ) != FR_OK) return false;
    bool decoded = false;
    if (f_lseek(&source.file, picture.offset) == FR_OK &&
        jd_prepare(&jpeg_decoder, jpeg_input, jpeg_work, sizeof(jpeg_work),
                   &source) == JDR_OK) {
        uint8_t scale = 0;
        for (uint8_t candidate = 1; candidate <= 3; ++candidate) {
            const uint16_t width = static_cast<uint16_t>(
                jpeg_decoder.width >> candidate);
            const uint16_t height = static_cast<uint16_t>(
                jpeg_decoder.height >> candidate);
            if (width < PHOSPHOR_ART_WIDTH || height < PHOSPHOR_ART_HEIGHT)
                break;
            scale = candidate;
        }
        source.scaled_width = static_cast<uint16_t>(
            jpeg_decoder.width >> scale);
        source.scaled_height = static_cast<uint16_t>(
            jpeg_decoder.height >> scale);
        const uint16_t crop = std::min(source.scaled_width,
                                       source.scaled_height);
        const uint16_t crop_x = (source.scaled_width - crop) / 2;
        const uint16_t crop_y = (source.scaled_height - crop) / 2;
        for (size_t index = 0; index < PHOSPHOR_ART_WIDTH; ++index) {
            source.source_x[index] = static_cast<uint16_t>(
                crop_x + (index * crop) / PHOSPHOR_ART_WIDTH);
        }
        for (size_t index = 0; index < PHOSPHOR_ART_HEIGHT; ++index) {
            source.source_y[index] = static_cast<uint16_t>(
                crop_y + (index * crop) / PHOSPHOR_ART_HEIGHT);
        }
        memset(output, 0, PHOSPHOR_ART_BYTES);
        decoded = jd_decomp(&jpeg_decoder, jpeg_output, scale) == JDR_OK &&
                  !source.io_error;
    }
    if (f_close(&source.file) != FR_OK) decoded = false;
    return decoded;
}
