#!/usr/bin/env python3
# Copyright 2026 aquasock
# SPDX-License-Identifier: Apache-2.0

"""TangCore USB CDC console and transfer benchmark client."""

import argparse
import os
import re
import sys
import tempfile
import time
import zlib

import serial
from serial.tools import list_ports


USB_VID = 0xFFFF
USB_PID = 0x6160


def find_port(vid=USB_VID, pid=USB_PID):
    matches = [
        port.device
        for port in list_ports.comports()
        if port.vid == vid and port.pid == pid
    ]
    if not matches:
        raise RuntimeError("TangCore USB CDC device not found")
    if len(matches) != 1:
        raise RuntimeError(f"multiple TangCore USB CDC devices found: {matches}")
    return matches[0]


def open_port(path):
    port = serial.Serial(
        path, 115200, timeout=2, write_timeout=5, exclusive=True
    )
    port.reset_input_buffer()
    port.reset_output_buffer()
    time.sleep(0.25)
    port.reset_input_buffer()
    synchronize_port(port)
    return port


def read_line(port, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline()
        if line:
            text = line.decode("utf-8", errors="replace").strip()
            if text.startswith("> "):
                text = text[2:]
            return text
    raise TimeoutError("timed out waiting for TangCore")


def synchronize_port(port):
    """Clear a partial startup command and verify the command channel."""
    for _attempt in range(3):
        port.reset_input_buffer()
        port.write(b"\nping\n")
        deadline = time.monotonic() + 5
        saw_pong = False
        while time.monotonic() < deadline:
            try:
                line = read_line(port, timeout=1)
            except TimeoutError:
                break
            if line == "PONG":
                saw_pong = True
            elif line == "OK" and saw_pong:
                return
        time.sleep(0.05)
    raise RuntimeError("could not synchronize with the TangCore command channel")


def run_command(port, command):
    lines = []
    port.write(command.encode("ascii") + b"\n")
    while True:
        line = read_line(port)
        if line == "OK":
            return lines
        if line.startswith("ERR"):
            raise RuntimeError(line)
        if line and line != ">":
            lines.append(line)
            print(line)


def run_benchmark(port, size):
    port.write(f"bench {size}\n".encode("ascii"))
    while True:
        line = read_line(port)
        if line.startswith("READY "):
            break
        if line.startswith("ERR"):
            raise RuntimeError(line)

    pattern = bytes(range(256)) * 64
    remaining = size
    crc = 0
    started = time.monotonic()
    while remaining:
        chunk = pattern[: min(remaining, len(pattern))]
        port.write(chunk)
        crc = zlib.crc32(chunk, crc)
        remaining -= len(chunk)
    port.flush()
    elapsed = time.monotonic() - started

    device_result = None
    while True:
        line = read_line(port, timeout=10)
        if line.startswith("BENCH "):
            device_result = line
        elif line.startswith("ERR"):
            raise RuntimeError(line)
        elif line == "OK":
            break

    mib_s = size / max(elapsed, 1e-9) / (1024 * 1024)
    print(f"host: bytes={size} seconds={elapsed:.3f} MiB/s={mib_s:.2f} crc32={crc:08x}")
    print(f"device: {device_result}")
    if device_result is None:
        raise RuntimeError("device did not return benchmark results")
    match = re.fullmatch(
        r"BENCH bytes=(\d+) ms=(\d+) crc32=([0-9a-fA-F]{8}) dropped=(\d+)",
        device_result,
    )
    if match is None:
        raise RuntimeError(f"could not parse device result: {device_result}")
    device_bytes, device_ms, device_crc, dropped = match.groups()
    if int(device_bytes) != size or int(device_crc, 16) != crc:
        raise RuntimeError("device CRC did not match host CRC")
    if int(dropped) != 0:
        raise RuntimeError(f"device dropped {dropped} bytes")
    device_mib_s = size / max(int(device_ms), 1) * 1000 / (1024 * 1024)
    print(f"verified: device MiB/s={device_mib_s:.2f}, CRC matched, no drops")


def file_crc(path):
    crc = 0
    size = 0
    with open(path, "rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
            size += len(chunk)
    return size, crc


def validate_remote_path(remote_path, allow_empty=False):
    try:
        encoded = remote_path.encode("ascii")
    except UnicodeEncodeError as error:
        raise RuntimeError("remote path must contain only ASCII characters") from error
    if not encoded and allow_empty:
        return
    if (
        not encoded
        or remote_path.startswith("/")
        or ".." in remote_path
        or ":" in remote_path
        or "\\" in remote_path
        or any(byte < 0x20 or byte >= 0x7F for byte in encoded)
    ):
        raise RuntimeError("remote path must be a safe path relative to the SD card")


def run_put(port, local_path, remote_path):
    validate_remote_path(remote_path)

    size, crc = file_crc(local_path)
    if size == 0:
        raise RuntimeError("cannot upload an empty file")
    print(f"local: bytes={size} crc32={crc:08x}")
    port.write(f"put {size} {crc:08x} {remote_path}\n".encode("ascii"))
    while True:
        line = read_line(port)
        if line == f"READY {size}":
            break
        if line.startswith("ERR"):
            raise RuntimeError(line)

    started = time.monotonic()
    with open(local_path, "rb") as source:
        while True:
            chunk = source.read(64 * 1024)
            if not chunk:
                break
            port.write(chunk)
    port.flush()

    result = None
    while True:
        line = read_line(port, timeout=30)
        if line.startswith("PUT "):
            result = line
        elif line.startswith("ERR"):
            raise RuntimeError(line)
        elif line == "OK":
            break
    elapsed = time.monotonic() - started
    if result is None or f"bytes={size}" not in result or f"crc32={crc:08x}" not in result:
        raise RuntimeError(f"invalid upload result: {result}")

    lines = run_command(port, f"crc {remote_path}")
    expected = f"FILE bytes={size} crc32={crc:08x}"
    if expected not in lines:
        raise RuntimeError("SD readback CRC did not match the local file")
    mib_s = size / max(elapsed, 1e-9) / (1024 * 1024)
    print(f"uploaded: {remote_path} in {elapsed:.3f}s ({mib_s:.2f} MiB/s)")
    print("verified: SD readback size and CRC matched")


def read_exact(port, destination, size, timeout=10):
    remaining = size
    crc = 0
    last_data = time.monotonic()
    while remaining:
        chunk = port.read(min(64 * 1024, remaining))
        if not chunk:
            if time.monotonic() - last_data >= timeout:
                raise TimeoutError(
                    f"timed out after receiving {size - remaining} of {size} bytes"
                )
            continue
        destination.write(chunk)
        crc = zlib.crc32(chunk, crc)
        remaining -= len(chunk)
        last_data = time.monotonic()
    return crc


def run_get(port, remote_path, local_path):
    validate_remote_path(remote_path)
    destination = os.path.abspath(local_path)
    parent = os.path.dirname(destination)
    if not os.path.isdir(parent):
        raise RuntimeError(f"local destination directory does not exist: {parent}")

    port.write(f"get {remote_path}\n".encode("ascii"))
    size = None
    while size is None:
        line = read_line(port)
        if line.startswith("READY "):
            try:
                size = int(line.split()[1])
            except (IndexError, ValueError) as error:
                raise RuntimeError(f"invalid download header: {line}") from error
        elif line.startswith("ERR"):
            raise RuntimeError(line)

    temporary_path = None
    started = time.monotonic()
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=".tangget-", dir=parent, delete=False
        ) as temporary:
            temporary_path = temporary.name
            crc = read_exact(port, temporary, size)

        result = None
        while True:
            line = read_line(port, timeout=30)
            if line.startswith("GET "):
                result = line
            elif line.startswith("ERR"):
                raise RuntimeError(line)
            elif line == "OK":
                break

        match = re.fullmatch(
            r"GET bytes=(\d+) ms=(\d+) crc32=([0-9a-fA-F]{8})", result or ""
        )
        if match is None:
            raise RuntimeError(f"invalid download result: {result}")
        device_size, _device_ms, device_crc = match.groups()
        if int(device_size) != size or int(device_crc, 16) != crc:
            raise RuntimeError("downloaded size or CRC did not match the device")

        os.replace(temporary_path, destination)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                os.unlink(temporary_path)
            except FileNotFoundError:
                pass

    elapsed = time.monotonic() - started
    mib_s = size / max(elapsed, 1e-9) / (1024 * 1024)
    print(f"downloaded: {remote_path} -> {local_path}")
    print(f"verified: bytes={size} crc32={crc:08x} ({mib_s:.2f} MiB/s)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="serial device; auto-detected when omitted")
    parser.add_argument(
        "--vid", type=lambda value: int(value, 0), default=USB_VID,
        help="USB vendor ID used for auto-detection (default: 0xffff)",
    )
    parser.add_argument(
        "--pid", type=lambda value: int(value, 0), default=USB_PID,
        help="USB product ID used for auto-detection (default: 0x6160)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("ping")
    subparsers.add_parser("status")
    benchmark = subparsers.add_parser("bench")
    benchmark.add_argument("--size", type=int, default=8 * 1024 * 1024)
    upload = subparsers.add_parser("put")
    upload.add_argument("local")
    upload.add_argument("remote", help="path relative to the SD-card root")
    download = subparsers.add_parser("get")
    download.add_argument("remote", help="path relative to the SD-card root")
    download.add_argument("local")
    listing = subparsers.add_parser("ls")
    listing.add_argument("remote", nargs="?", default="")
    remove = subparsers.add_parser("rm")
    remove.add_argument("remote")
    make_directory = subparsers.add_parser("mkdir")
    make_directory.add_argument("remote")
    args = parser.parse_args()

    path = args.port or find_port(args.vid, args.pid)
    with open_port(path) as port:
        if args.command == "bench":
            run_benchmark(port, args.size)
        elif args.command == "put":
            run_put(port, args.local, args.remote)
        elif args.command == "get":
            run_get(port, args.remote, args.local)
        elif args.command == "ls":
            validate_remote_path(args.remote, allow_empty=True)
            run_command(port, "ls" + (f" {args.remote}" if args.remote else ""))
        elif args.command in ("rm", "mkdir"):
            validate_remote_path(args.remote)
            run_command(port, f"{args.command} {args.remote}")
        else:
            run_command(port, args.command)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
