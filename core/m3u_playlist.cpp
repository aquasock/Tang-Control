#include "m3u_playlist.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>

namespace {

std::string trim_ascii(const std::string &text)
{
    size_t first = 0;
    while (first < text.size() &&
           (text[first] == ' ' || text[first] == '\t')) {
        ++first;
    }
    size_t last = text.size();
    while (last > first &&
           (text[last - 1] == ' ' || text[last - 1] == '\t')) {
        --last;
    }
    return text.substr(first, last - first);
}

bool starts_with_case_insensitive(const std::string &text, const char *prefix)
{
    for (size_t index = 0; prefix[index] != '\0'; ++index) {
        if (index >= text.size() ||
            tolower(static_cast<unsigned char>(text[index])) !=
                tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

std::string filename_title(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        name.resize(dot);
    }
    return name;
}

} // namespace

M3uParser::M3uParser(const std::string &playlist_path)
    : pending_duration_(-1), first_line_(true), previous_was_cr_(false),
      finished_(false)
{
    const size_t colon = playlist_path.find(':');
    if (colon == std::string::npos) {
        fail("playlist path has no filesystem drive");
        return;
    }
    drive_ = playlist_path.substr(0, colon + 1);
    const size_t slash = playlist_path.find_last_of('/');
    base_directory_ = slash == std::string::npos
                          ? drive_
                          : playlist_path.substr(0, slash);
}

void M3uParser::fail(const char *message)
{
    if (error_.empty()) {
        error_ = message;
    }
}

bool M3uParser::feed(const uint8_t *data, size_t length)
{
    if (!error_.empty() || finished_ || (data == nullptr && length != 0)) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const uint8_t byte = data[index];
        if (byte == '\n') {
            if (!previous_was_cr_ && !finish_line()) {
                return false;
            }
            previous_was_cr_ = false;
        } else if (byte == '\r') {
            if (!finish_line()) {
                return false;
            }
            previous_was_cr_ = true;
        } else {
            previous_was_cr_ = false;
            if (line_.size() >= M3U_MAX_LINE_LENGTH) {
                fail("playlist line exceeds 512 bytes");
                return false;
            }
            line_.push_back(static_cast<char>(byte));
        }
    }
    return true;
}

bool M3uParser::finish_line()
{
    std::string text = line_;
    line_.clear();

    if (first_line_) {
        first_line_ = false;
        if (text.size() >= 3 &&
            static_cast<uint8_t>(text[0]) == 0xef &&
            static_cast<uint8_t>(text[1]) == 0xbb &&
            static_cast<uint8_t>(text[2]) == 0xbf) {
            text.erase(0, 3);
        }
    }

    text = trim_ascii(text);
    if (text.empty()) {
        return true;
    }
    if (text[0] == '#') {
        if (starts_with_case_insensitive(text, "#EXT-X-")) {
            fail("HLS playlists are not supported");
            return false;
        }
        if (starts_with_case_insensitive(text, "#EXTINF:")) {
            const size_t comma = text.find(',', 8);
            if (comma == std::string::npos) {
                fail("EXTINF line is missing its title separator");
                return false;
            }
            const std::string duration_text = trim_ascii(text.substr(8, comma - 8));
            char *end = nullptr;
            const long duration = strtol(duration_text.c_str(), &end, 10);
            if (duration_text.empty() || end == duration_text.c_str() ||
                *end != '\0' || duration < -1 || duration > INT32_MAX) {
                fail("EXTINF duration is invalid");
                return false;
            }
            pending_duration_ = static_cast<int32_t>(duration);
            pending_title_ = trim_ascii(text.substr(comma + 1));
        }
        return true;
    }

    if (entries_.size() >= M3U_MAX_TRACKS) {
        fail("playlist exceeds 255 tracks");
        return false;
    }
    std::string resolved;
    if (!resolve_path(text, resolved)) {
        return false;
    }
    entries_.push_back({resolved,
                        pending_title_.empty() ? filename_title(resolved)
                                               : pending_title_,
                        pending_duration_});
    pending_title_.clear();
    pending_duration_ = -1;
    return true;
}

bool M3uParser::resolve_path(const std::string &reference,
                             std::string &resolved)
{
    if (reference.find("://") != std::string::npos ||
        reference.find(':') != std::string::npos) {
        fail("playlist URLs and drive-qualified paths are not supported");
        return false;
    }

    std::string normalized = reference;
    for (char &byte : normalized) {
        if (byte == '\\') {
            byte = '/';
        }
    }

    std::string combined;
    if (!normalized.empty() && normalized[0] == '/') {
        combined = drive_ + normalized.substr(1);
    } else {
        combined = base_directory_;
        if (combined.size() > drive_.size()) {
            combined.push_back('/');
        }
        combined += normalized;
    }

    const std::string relative = combined.substr(drive_.size());
    std::vector<std::string> components;
    size_t position = 0;
    while (position <= relative.size()) {
        const size_t slash = relative.find('/', position);
        const size_t end = slash == std::string::npos ? relative.size() : slash;
        const std::string component = relative.substr(position, end - position);
        if (component.empty() || component == ".") {
            // Repeated separators and current-directory components are harmless.
        } else if (component == "..") {
            if (components.empty()) {
                fail("playlist path escapes the filesystem root");
                return false;
            }
            components.pop_back();
        } else {
            components.push_back(component);
        }
        if (slash == std::string::npos) {
            break;
        }
        position = slash + 1;
    }

    resolved = drive_;
    for (size_t index = 0; index < components.size(); ++index) {
        if (index != 0) {
            resolved.push_back('/');
        }
        resolved += components[index];
    }
    if (resolved.size() > M3U_MAX_PATH_LENGTH || components.empty()) {
        fail(components.empty() ? "playlist track path is empty"
                                : "playlist track path exceeds 255 bytes");
        return false;
    }
    return true;
}

bool M3uParser::finish()
{
    if (!error_.empty() || finished_) {
        return false;
    }
    if (!line_.empty() && !finish_line()) {
        return false;
    }
    finished_ = true;
    if (!pending_title_.empty() || pending_duration_ != -1) {
        fail("EXTINF entry has no following track path");
    } else if (entries_.empty()) {
        fail("playlist contains no tracks");
    }
    return error_.empty();
}

const std::vector<M3uEntry> &M3uParser::entries() const
{
    return entries_;
}

const std::string &M3uParser::error() const
{
    return error_;
}

bool m3u_path_has_extension(const std::string &path, const char *extension)
{
    if (extension == nullptr) {
        return false;
    }
    const size_t extension_length = std::char_traits<char>::length(extension);
    if (path.size() < extension_length) {
        return false;
    }
    const size_t offset = path.size() - extension_length;
    for (size_t index = 0; index < extension_length; ++index) {
        if (tolower(static_cast<unsigned char>(path[offset + index])) !=
            tolower(static_cast<unsigned char>(extension[index]))) {
            return false;
        }
    }
    return true;
}
