# Tang-Control — TangCore firmware + host client (User Guide)

This repo is a **fork of nand2mario's [firmware-bl616](https://github.com/nand2mario/tangcore)** —
the TangCore firmware that runs on the BL616 MCU of a Sipeed **Tang Console**
board — plus the PC-side client that talks to it over USB CDC.

The interesting work lives on the **`feature/usb-cdc-file-transfer`** branch.

---

## What our fork adds over stock

The stock firmware (`master`) loads cores, shows the menu, and does its own
debug over UART. Our branch adds a proper **USB CDC command console** and the
transport protocols behind it:

- `usb/usb_cdc_console.cpp` — the command interpreter (the "tangctl" protocol)
- `utils/fpga_debug.cpp` / `fpga_ext_frame.h` — extended debug (`0x10`) for `peek`/`poke`/`caps`/`baud`
- `utils/fpga_stream.cpp` / `fpga_file_stream.cpp` — stream protocol (`0x11`)
- `utils/firmware_image.h` — no-BOOT-mode firmware update
- FPGA UART RX rework — interrupt-driven RX + a TX mutex (fixes gamepad/OSD stutter)

In short: **every `tangctl.py` command is an addition in this branch.** It does
not exist in stock firmware.

---

## Build the firmware

```bash
export PATH=/home/vash/.cache/tangcore-dev/toolchain/bin:$PATH
cmake --build build/ -j8
```

This produces `build/build_out/tangcore_bl616.bin`.

---

## Flash the firmware

There are two cases.

### A. Update an already-running Tang-Control (no BOOT button)

Patch the boot header (length @ `0x84`, CRC @ `0xFC`) and flash over CDC:

```bash
python3 - <<'EOF'
import zlib
b = bytearray(open('build/build_out/tangcore_bl616.bin','rb').read())
b[0x84:0x88] = (len(b) - 0x1000).to_bytes(4, 'little')
b[0xFC:0x100] = (zlib.crc32(bytes(b[:0xFC])) & 0xffffffff).to_bytes(4, 'little')
open('build/build_out/tangcore_bl616_flash.bin','wb').write(b)
EOF

python3 scripts/tangctl.py firmware build/build_out/tangcore_bl616_flash.bin
```

Then **power-cycle** the board. Verify with:

```bash
python3 scripts/tangctl.py status     # app_sha256 should match the new build
```

### B. First flash onto a stock board

Follow the stock `README.md` `make flash` procedure (BOOT button + USB), then
switch to path A for subsequent updates. `scripts/jtag.py` and
`tdi_compare.py` are the JTAG-side helpers for that flow.

---

## The host client: `tangctl.py`

Run from this repo: `python3 scripts/tangctl.py <command>`.
Requires the **two-wire** setup (power + CDC cable) and **this firmware**.

| Command | What it does |
|---------|--------------|
| `ping` | verify the command channel |
| `status` | board + loader state |
| `rxstats [--reset]` | FPGA UART RX health counters |
| `caps` | FPGA transport capabilities *(needs a Phosphor core)* |
| `peek <addr> [n]` | read debug register(s) *(needs a Phosphor core)* |
| `poke <addr> <val>` | write a debug register *(needs a Phosphor core)* |
| `baud <2\|5>` | switch FPGA UART rate *(needs a Phosphor core)* |
| `stream <sd-path>` | stream an SD file to the active core |
| `bench [--size N]` | throughput benchmark |
| `put <local> <remote>` | upload a file to the SD card |
| `get <remote> <local>` | download a file from the SD card |
| `ls [path]` | list an SD directory |
| `rm <path>` | remove an SD file/directory |
| `mkdir <path>` | create an SD directory |
| `firmware <image>` | install a BL616 firmware image |

There is no `rename`; rename = `get` + `put` (new name) + `rm`.

---

## Debug scripts

- `liveuart.py` / `liveuart_draw.py` — decode/visualize the BL616↔FPGA UART traffic
- `print_uart.py` — raw UART dump
- `jtag.py` + `tdi_compare.py` + `crc16.sh` — JTAG-programming verification helpers
- `fs.py` — convert Gowin `.fs` → `.bin`

---

## Relationship to Tang-Phosphor

- **This repo** is the *firmware* (BL616) and its generic two-wire client. It is
  core-agnostic and has no dependency on Tang-Phosphor.
- **Tang-Phosphor** is a specific core and its one-wire (direct-FPGA) tools; it
  *imports* `tangctl.py` from here for its two-wire access.

So the dependency is one-way: Tang-Phosphor → Tang-Control, never the reverse.

---

## Gotchas

1. **Two-wire requires this firmware.** Stock nand2mario firmware has no CDC
   command console; `tangctl.py` cannot talk to it.
2. **`caps`/`peek`/`poke`/`baud` need a Phosphor core loaded** — those speak the
   extended `0x10` protocol, which only the Phosphor core implements.
3. **After a `firmware` update, power-cycle** — the BL616 resets into its vendor
   loader and TangCore only returns on power-on.
