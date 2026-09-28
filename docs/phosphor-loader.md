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
- `#EXTINF` supplies display metadata only. Track advancement follows the
  FPGA player's reported state, never the tagged duration.
- A playlist may contain at most 255 tracks. A source line is limited to 512
  bytes and a resolved path to 255 bytes.
- URLs, HLS playlists, nested playlists, missing files, and unsupported formats
  are rejected before playback begins.

Every entry is its own stream session. When the core advertises gapless
append (core capability bit 7), the loader starts the next entry as soon as the
player reports that the current stream's final sample is queued (state `7`,
draining), instead of waiting for the FIFO to empty. The core plays the new
session's first sample on the sample period after the previous track's last, so
same-rate tracks are sample-contiguous. The last entry still waits for
completion. Older cores fall back to the completion handover.

Native FLAC tracks are sent as `fLaC`, STREAMINFO marked as the last metadata
block, and the unchanged audio frames. The core skips every other metadata
block, so large PICTURE or PADDING blocks no longer delay a track's first frame
on the FPGA UART. The display task still reads those blocks from the SD file.

A gapless successor is queued up to one PCM FIFO (about 0.4 s) ahead of its
audio. Its text and artwork are written to the inactive FPGA banks early and
committed once the core's audible-stream register (`0xa4`) reports the new
session, so the display changes with the sound. Left/Right pressed during that
short window act relative to the queued track. The current project scope is
intentionally limited to WAV and FLAC.

## Verification

The host-side regressions use the same syntax emitted by VLC and check duplicate
entries, byte-at-a-time input, relative path resolution, BOM/CRLF handling,
bounds, unsupported inputs, FLAC comments and cover discovery, WAV INFO tags,
and per-track metadata precedence:

```sh
tests/run.sh
```
