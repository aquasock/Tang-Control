#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

constexpr size_t M3U_MAX_TRACKS = 255;
constexpr size_t M3U_MAX_LINE_LENGTH = 512;
constexpr size_t M3U_MAX_PATH_LENGTH = 255;

struct M3uEntry {
    std::string path;
    std::string title;
    int32_t duration_seconds;
};

class M3uParser {
public:
    explicit M3uParser(const std::string &playlist_path);

    bool feed(const uint8_t *data, size_t length);
    bool finish();

    const std::vector<M3uEntry> &entries() const;
    const std::string &error() const;

private:
    bool finish_line();
    bool resolve_path(const std::string &reference, std::string &resolved);
    void fail(const char *message);

    std::string drive_;
    std::string base_directory_;
    std::string line_;
    std::string pending_title_;
    int32_t pending_duration_;
    std::vector<M3uEntry> entries_;
    std::string error_;
    bool first_line_;
    bool previous_was_cr_;
    bool finished_;
};

bool m3u_path_has_extension(const std::string &path, const char *extension);
