#pragma once

#include <stddef.h>
#include <stdint.h>

#include "phosphor_metadata.h"

constexpr size_t PHOSPHOR_ART_WIDTH = 92;
constexpr size_t PHOSPHOR_ART_HEIGHT = 92;
constexpr size_t PHOSPHOR_ART_BYTES = PHOSPHOR_ART_WIDTH * PHOSPHOR_ART_HEIGHT;

bool phosphor_read_file_metadata(const char *path,
                                 PhosphorAudioMetadata &metadata);

// Decodes a baseline JPEG picture into MiSTer-Phosphor-compatible RGB332.
bool phosphor_decode_artwork(const char *path, const PhosphorPicture &picture,
                            uint8_t *rgb332, size_t length);
