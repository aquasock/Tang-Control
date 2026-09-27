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

Insert the SD card before boot, power the console normally, wait for the menu,
and then connect `DEBUG/OTG` to the PC. The development configuration
enumerates as USB VID:PID `ffff:6160`; `scripts/tangctl.py` finds the serial
port automatically. These IDs are not an assigned public product identity and
should be overridden for distributed builds.

```bash
python3 scripts/tangctl.py ping
python3 scripts/tangctl.py status
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

Acknowledgements
* JTAG FPGA programming logic based on [openFPGALoader](https://github.com/trabucayre/openFPGALoader)
* Gamepad support based on Till Harbaum's [FPGA-Companion](https://github.com/harbaum/FPGA-Companion)
