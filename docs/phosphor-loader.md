# Phosphor audio loader

Tang-Control provides the filesystem and playlist layer for Tang-Phosphor. The
BL616 reads independent files from the SD card and presents each audio track to
the FPGA as a separate acknowledged stream session. Audio files do not need to
be combined in a TAR archive.

## SD-card layout

Place the stable core image at:

```text
cores/console138k/tang-phosphor.bin
```

Place audio and playlists under `music/`. TangCore then shows **Phosphor** in
its main menu. Selecting it opens the filtered audio chooser, loads the FPGA
core when necessary, and begins the selected WAV, FLAC, or playlist.

The TangCore OSD intentionally provides only **Load Audio / Playlist** and
**Main Menu**. Opening or navigating the OSD does not stop playback. Playback
controls live in Phosphor's native 720p screen: **Start** pauses/resumes,
**Left/Right** select the previous/next playlist track, and **X** shows or hides
the screen. These controls are suppressed while the OSD is open.

Tang-Control owns playlist navigation and sends a complete bounded metadata
snapshot for each track. The FPGA publishes that snapshot atomically and owns
rendering, pause timing, progress, and exact elapsed/total clocks. Reaching the
last playlist track completes playback without looping.

The native information panel contains exactly three values: album, artist, and
track. Per-track FLAC `ALBUM`, `ALBUMARTIST` (preferred over `ARTIST`), `ARTIST`,
and `TITLE` comments override playlist-name and VLC `#EXTINF` fallbacks. WAV
files use standard RIFF `LIST/INFO` fields `IPRD`, `IART`, and `INAM` when
present. FLAC front-cover PICTURE blocks also override the no-art placeholder.
Baseline JPEG covers are center-fitted to 92x92 RGB332 on the BL616 and uploaded
to an inactive FPGA bank before one atomic artwork commit. Metadata and artwork
work runs independently of the audio stream.

Display metadata is decoded as UTF-8 and reduced to the FPGA's bounded ASCII
font. Common typographic quotes, apostrophes, dashes, nonbreaking spaces, and
ellipses are normalized to readable ASCII; any other unsupported code point
becomes one `?` display character.

## VLC M3U compatibility profile

Both `.m3u` and UTF-8 `.m3u8` files are accepted. The loader supports the VLC
extended-M3U form used by the project qualification fixture:

```text
#EXTM3U
#EXTINF:29,Track title
track.wav
```

Playlist entries may mix `.wav` and `.flac` tracks; extension matching is
case-insensitive. Both formats use the core's signed 16-bit stereo,
44.1/48 kHz CD-quality profile.

- Track paths are resolved relative to the playlist directory.
- Forward and backward slashes, `.` components, and bounded `..` components
  are normalized without allowing a path to escape the filesystem root.
- Repeated paths remain repeated playlist entries.
- Blank lines, comments, LF or CRLF endings, and an optional UTF-8 byte-order
  mark are accepted.
- `#EXTINF` supplies display metadata only. Track advancement waits for the
  FPGA player's actual completion state.
- A playlist may contain at most 255 tracks. A source line is limited to 512
  bytes and a resolved path to 255 bytes.
- URLs, HLS playlists, nested playlists, missing files, and unsupported formats
  are rejected before playback begins.

Every entry is an independent stream session, so sample-contiguous gapless
playback is not guaranteed. Tang-Phosphor keeps the boundary silent and retains
the previous native rate while the next track prefills. The current project
scope is intentionally limited to WAV and FLAC.

## Verification

The host-side regressions use the same syntax emitted by VLC and check duplicate
entries, byte-at-a-time input, relative path resolution, BOM/CRLF handling,
bounds, unsupported inputs, FLAC comments and cover discovery, WAV INFO tags,
and per-track metadata precedence:

```sh
tests/run.sh
```
