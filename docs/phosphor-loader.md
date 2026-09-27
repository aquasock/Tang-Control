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
core when necessary, and begins the selected WAV or playlist.

The Phosphor in-core menu provides **Load Audio / Playlist**, **Previous
Track**, **Next Track**, and **Stop Playback** controls.

## VLC M3U compatibility profile

Both `.m3u` and UTF-8 `.m3u8` files are accepted. The loader supports the VLC
extended-M3U form used by the project qualification fixture:

```text
#EXTM3U
#EXTINF:29,Track title
track.wav
```

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
- The current decoder milestone accepts WAV entries only. URLs, HLS playlists,
  nested playlists, missing files, and unsupported formats are rejected before
  playback begins.

Track changes are not gapless because every entry is an independent stream
session. Later FLAC, MP3, and Ogg support can reuse the same playlist layer once
their FPGA decoders are available.

## Verification

The host-side parser regression uses the same syntax emitted by VLC and checks
duplicate entries, byte-at-a-time input, relative path resolution, BOM/CRLF
handling, bounds, and unsupported inputs:

```sh
tests/run.sh
```
