# TangCore firmware for BL616  

This is TangCore firmware for the on-board BL616 of Tang Console.

See [this document](https://github.com/nand2mario/tangcore/blob/main/doc/dev.md) for how the firmware works with cores.

## Build instructions

I'm building on Windows. Linux should also work. 

First download Bouffalo toolchain,

```bash
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_windows.git

# for Linux, clone: https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git
```

Add `toolchain_gcc_t-head_linux/bin` to your path.

Then download a patched version of Bouffalo SDK.

```bash
git clone --recurse-submodules https://github.com/nand2mario/bouffalo_sdk.git

# point BL_SDK_BASE to its location
set BL_SDK_BASE=<sdk_dir>
```

Then it should build OK.

```
make
make flash COMX=com5
```

The 2nd line flashes the firmware to BL616 (The required `bl616_fpga_partner_60kConsole.bin` file is [here](https://dl.sipeed.com/shareURL/TANG/Console/09_MCU_FW)). Before executing that, press and hold the "BOOT" button on the Tang Console board (bottom left corner, close to one of the USB-C port), then plug in the USB cable. This enters the BL616 into the programming mode.

For the USB drive, You need an OTG dongle to turn the connector from a "device" one to a "host" one, and provide power at the same time.

## USB CDC console and SD transfer (Console 138K)

The Console 138K build dedicates the bottom-left `DEBUG/OTG` USB-C port to a
PC-facing CDC serial link. The onboard SD interface and FPGA-side controller
ports continue to be used normally.

Install pyserial and the included Linux udev rule once:

```bash
python3 -m pip install pyserial
sudo install -m 0644 99-tangcore-usb.rules /etc/udev/rules.d/99-tangcore-usb.rules
sudo udevadm control --reload-rules
```

The rule covers both Tang-Control's normal `ffff:6160` CDC console and the
BL616 ROM bootloader's `349b:6160` programming port. After installing it, hold
**BOOT**, tap **RESET**, and release **BOOT** to enter programming mode without
disconnecting power.

Insert the SD card before boot, power the console normally, wait for the menu,
and then connect `DEBUG/OTG` to the PC. The development configuration
enumerates as USB VID:PID `ffff:6160`; `scripts/tangctl.py` finds the serial
port automatically. These IDs are not an assigned public product identity and
should be overridden for distributed builds.

```bash
python3 scripts/tangctl.py ping
python3 scripts/tangctl.py status
python3 scripts/tangctl.py rxstats --reset
python3 scripts/tangctl.py caps
python3 scripts/tangctl.py peek 0x00000000 8
python3 scripts/tangctl.py poke 0x00000020 0x12345678
python3 scripts/tangctl.py baud 5
python3 scripts/tangctl.py baud 2
python3 scripts/tangctl.py stream music/test.wav
python3 scripts/tangctl.py bench --size 8388608
python3 scripts/tangctl.py put build/my-core.bin cores/console138k/my-core.bin
python3 scripts/tangctl.py get cores/console138k/my-core.bin ./my-core.bin
python3 scripts/tangctl.py ls cores/console138k
python3 scripts/tangctl.py mkdir cores/console138k/testing
python3 scripts/tangctl.py rm cores/console138k/testing
```

Uploads are accepted only while the TangCore main menu is active. Leave the
controller idle during a transfer. The device writes to a temporary file,
checks the stream CRC, replaces the destination, and rereads the final file;
the client fails if the final size or CRC differs from the local file. Downloads
likewise use a temporary local file and replace the destination only after the
device-reported size and CRC match. `rm` can remove files or empty directories;
it does not recursively delete directory trees.

`rxstats` reports the health of the BL616's FPGA UART receive path: bytes and
joypad frames received, hardware RX FIFO overflows and high-water mark (the FIFO
holds 32 bytes), bytes skipped while resynchronizing, unknown frame types, and
the longest gap between FIFO polls. `--reset` zeroes the counters after
printing, so a reset before a test isolates its results. `status` includes the
same counters. A rising overflow count means FPGA replies or controller input
were lost.

To build this variant:

```bash
make TANG_BOARD=console138k USB_CDC_CONSOLE=1 -j$(nproc)
```

CDC mode is opt-in and currently limited to Console 138K. Without
`USB_CDC_CONSOLE=1`, the existing USB-host behavior is unchanged. A distributor
can override the development VID/PID, for example:

```bash
make TANG_BOARD=console138k USB_CDC_CONSOLE=1 \
    USB_CDC_VID=0x1234 USB_CDC_PID=0x5678 -j$(nproc)
```

When using assigned IDs, update the VID/PID in `99-tangcore-usb.rules` and pass
the same values to the client when relying on auto-detection:

```bash
python3 scripts/tangctl.py --vid 0x1234 --pid 0x5678 status
```

`flash_usb_console138k.ini` programs only the TangCore application at flash
offset `0x40000`; it intentionally leaves the board-specific first-stage image
at offset zero untouched.

The optional FPGA development channel is documented in
[`docs/extended-control-protocol.md`](docs/extended-control-protocol.md).
`peek` and `poke` only work with a core that implements that protocol; their
address map belongs to the core rather than Tang-Control.

Phosphor cover-art support uses ChaN's TJpgDec R0.03 from the required
Bouffalo SDK. TJpgDec permits personal, non-profit, and commercial use and
redistribution when its copyright notice is retained; the SDK source retains
that notice. JPEG decoding runs on the BL616 and sends only a 92x92 RGB332
image to the FPGA.
`stream` reads from the console's SD card and uses acknowledged 1024-byte
frames. When supported, it temporarily negotiates 5 Mbps and restores the safe
2 Mbps rate afterward.

## Tang-Phosphor audio loader

Core ID `0x50` has an integrated SD-card loader for standalone WAV/FLAC files and
VLC-style M3U/M3U8 playlists. It supports relative files as independent stream
sessions, automatic track advancement, duplicate entries, and in-core
controller navigation without packaging the files in a TAR archive. TangCore's
OSD only opens the audio chooser or returns to the main menu. In the native
Phosphor screen, Start pauses/resumes, Left/Right select the previous/next
playlist track, and X shows or hides the screen.
Setup, compatibility limits, and the deterministic parser test are documented
in [`docs/phosphor-loader.md`](docs/phosphor-loader.md).

Acknowledgements
* JTAG FPGA programming logic based on [openFPGALoader](https://github.com/trabucayre/openFPGALoader)
* Gamepad support based on Till Harbaum's [FPGA-Companion](https://github.com/harbaum/FPGA-Companion)
