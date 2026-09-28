#include "phosphor_ui_model.h"

#include <assert.h>

#include <iostream>
#include <string>

namespace {

std::string slot(const PhosphorUiSnapshot &snapshot, size_t index)
{
    return std::string(snapshot.text[index].begin(),
                       snapshot.text[index].begin() + snapshot.length[index]);
}

void test_playlist_window()
{
    std::vector<M3uEntry> entries;
    for (unsigned index = 1; index <= 10; ++index) {
        entries.push_back({"sd:track.flac", "Track " + std::to_string(index), 60});
    }
    const PhosphorUiSnapshot snapshot = phosphor_build_ui_snapshot(
        "sd:albums/The Wall.m3u8", entries, 8, true);
    assert(snapshot.playlist);
    assert(snapshot.current_track == 9);
    assert(snapshot.track_count == 10);
    assert(snapshot.window_start == 5);
    assert(slot(snapshot, 0) == "The Wall");
    assert(slot(snapshot, 1).empty());
    assert(slot(snapshot, 2) == "Track 9");
    assert(slot(snapshot, 3) == "005  Track 5");
    assert(slot(snapshot, 7) == "009  Track 9");
    assert(slot(snapshot, 8) == "010  Track 10");
}

void test_standalone_and_bounded_ascii()
{
    std::vector<M3uEntry> entries = {{
        "sd:music/a.flac",
        std::string("Long ") + "title with UTF-8 \xc3\xa9 and excess characters",
        -1}};
    const PhosphorUiSnapshot snapshot = phosphor_build_ui_snapshot(
        "sd:music/a.flac", entries, 0, false);
    assert(!snapshot.playlist);
    assert(snapshot.current_track == 1);
    assert(snapshot.track_count == 1);
    assert(snapshot.length[2] == PHOSPHOR_UI_SLOT_BYTES - 1);
    assert(slot(snapshot, 2).find("UTF-8 ?") != std::string::npos);
    for (size_t row = 3; row < PHOSPHOR_UI_SLOT_COUNT; ++row) {
        assert(snapshot.length[row] == 0);
    }
}

void test_utf8_typography_normalization()
{
    const std::string title = std::string("Don") + "\xe2\x80\x99" +
                              "t `stop`";
    std::vector<M3uEntry> entries = {{"sd:music/test.flac", title, -1}};
    PhosphorUiSnapshot snapshot = phosphor_build_ui_snapshot(
        "sd:music/test.flac", entries, 0, false);
    assert(slot(snapshot, 2) == "Don't `stop`");

    entries[0].title = std::string("A ") + "\xe2\x80\x98" + "B" +
        "\xe2\x80\x99" + " " + "\xe2\x80\x9c" + "C" +
        "\xe2\x80\x9d" + " " + "\xe2\x80\x93" + " " +
        "\xe2\x80\x94" + " " + "\xe2\x80\xa6";
    snapshot = phosphor_build_ui_snapshot(
        "sd:music/test.flac", entries, 0, false);
    assert(slot(snapshot, 2) == "A 'B' \"C\" - - ...");
}

void test_metadata_precedence()
{
    std::vector<M3uEntry> entries = {
        {"sd:albums/track.flac", "Playlist Artist - Playlist Track", 60},
        {"sd:albums/second.flac", "Playlist Artist - Second Track", 60}};
    PhosphorUiSnapshot snapshot = phosphor_build_ui_snapshot(
        "sd:albums/Playlist Album.m3u8", entries, 0, true);
    assert(slot(snapshot, 0) == "Playlist Album");
    assert(slot(snapshot, 1) == "Playlist Artist");
    assert(slot(snapshot, 2) == "Playlist Track");

    PhosphorAudioMetadata embedded;
    embedded.album = "Embedded Album";
    embedded.artist = "Track Artist";
    embedded.album_artist = "Album Artist";
    embedded.title = "Embedded Track";
    phosphor_apply_track_metadata(snapshot, embedded);
    assert(slot(snapshot, 0) == "Embedded Album");
    assert(slot(snapshot, 1) == "Album Artist");
    assert(slot(snapshot, 2) == "Embedded Track");
    assert(slot(snapshot, 3) == "001  Playlist Artist - Playlist");
}

} // namespace

int main()
{
    test_playlist_window();
    test_standalone_and_bounded_ascii();
    test_utf8_typography_normalization();
    test_metadata_precedence();
    std::cout << "PASS: bounded Phosphor album UI metadata and playlist window\n";
    return 0;
}
