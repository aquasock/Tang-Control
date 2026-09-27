#include "m3u_playlist.h"

#include <assert.h>
#include <stdint.h>

#include <iostream>
#include <fstream>
#include <iterator>
#include <string>

namespace {

M3uParser parse(const std::string &path, const std::string &text,
                size_t chunk_size = 0)
{
    M3uParser parser(path);
    if (chunk_size == 0) {
        chunk_size = text.size();
    }
    for (size_t offset = 0; offset < text.size(); offset += chunk_size) {
        const size_t length = std::min(chunk_size, text.size() - offset);
        if (!parser.feed(reinterpret_cast<const uint8_t *>(text.data() + offset),
                         length)) {
            break;
        }
    }
    parser.finish();
    return parser;
}

void test_vlc_fixture(const char *fixture_path)
{
    std::ifstream fixture(fixture_path, std::ios::binary);
    assert(fixture.good());
    const std::string text((std::istreambuf_iterator<char>(fixture)),
                           std::istreambuf_iterator<char>());
    M3uParser parser = parse("sd:music/test.m3u8", text, 1);
    assert(parser.error().empty());
    assert(parser.entries().size() == 4);
    for (const M3uEntry &entry : parser.entries()) {
        assert(entry.path == "sd:music/file_example_WAV_5MG.wav");
        assert(entry.title == "file_example_WAV_5MG.wav");
        assert(entry.duration_seconds == 29);
    }
}

void test_bom_crlf_and_paths()
{
    const std::string text =
        "\xef\xbb\xbf#EXTM3U\r\n"
        "# a comment\r\n"
        "\r\n"
        "#EXTINF:-1,First title\r\n"
        "disc1\\first.wav\r\n"
        "#EXTINF:42,Second title\r\n"
        "../shared/second.WAV";
    M3uParser parser = parse("sd:music/lists/test.m3u8", text, 7);
    assert(parser.error().empty());
    assert(parser.entries().size() == 2);
    assert(parser.entries()[0].path == "sd:music/lists/disc1/first.wav");
    assert(parser.entries()[0].title == "First title");
    assert(parser.entries()[0].duration_seconds == -1);
    assert(parser.entries()[1].path == "sd:music/shared/second.WAV");
    assert(parser.entries()[1].title == "Second title");
    assert(parser.entries()[1].duration_seconds == 42);
}

void test_plain_m3u_and_root_path()
{
    M3uParser parser = parse("sd:music/list.m3u",
                             "one.wav\n/music/two.flac\n");
    assert(parser.error().empty());
    assert(parser.entries().size() == 2);
    assert(parser.entries()[0].title == "one");
    assert(parser.entries()[1].path == "sd:music/two.flac");
    assert(parser.entries()[1].title == "two");
}

void test_audio_extensions()
{
    assert(m3u_path_has_extension("sd:music/track.wav", ".wav"));
    assert(m3u_path_has_extension("sd:music/track.FLAC", ".flac"));
    assert(!m3u_path_has_extension("sd:music/track.flac.txt", ".flac"));
}

void test_rejections()
{
    M3uParser url = parse("sd:music/list.m3u8", "https://example/test.wav\n");
    assert(!url.error().empty());

    M3uParser hls = parse("sd:music/list.m3u8",
                          "#EXTM3U\n#EXT-X-TARGETDURATION:10\ntrack.ts\n");
    assert(!hls.error().empty());

    M3uParser escape = parse("sd:list.m3u8", "../outside.wav\n");
    assert(!escape.error().empty());

    M3uParser dangling = parse("sd:music/list.m3u8", "#EXTINF:2,title\n");
    assert(!dangling.error().empty());

    std::string too_long(M3U_MAX_LINE_LENGTH + 1, 'a');
    M3uParser long_line = parse("sd:music/list.m3u8", too_long + "\n");
    assert(!long_line.error().empty());
}

} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_vlc_fixture(argv[1]);
    test_bom_crlf_and_paths();
    test_plain_m3u_and_root_path();
    test_audio_extensions();
    test_rejections();
    std::cout << "PASS: VLC M3U8 parsing, WAV/FLAC paths, duplicates, and bounds\n";
    return 0;
}
