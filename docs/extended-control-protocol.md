# Extended FPGA control protocol

Tang-Control reserves legacy UART frame type `0x10` for a versioned request and
response channel. It is intended for development control, diagnostics, and
future negotiated transports without assigning core-specific meanings in the
BL616 firmware.

All multibyte fields are big-endian. CRC fields use CRC-16/CCITT-FALSE
(polynomial `0x1021`, initial value `0xffff`, no reflection, no final XOR) and
cover the `0x10` command byte followed by every payload byte before the CRC.

## Version 1 request

The legacy frame length is 15 bytes: one command byte and this 14-byte payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Protocol version (`1`) |
| 1 | 1 | Opcode |
| 2 | 2 | Transaction sequence |
| 4 | 4 | 32-bit address |
| 8 | 4 | 32-bit data; zero for reads |
| 12 | 2 | CRC-16 |

Opcodes are `0x00` capability query, `0x01` 32-bit read, `0x02` 32-bit write,
and `0x03` negotiated baud change. A baud change accepts `2000000` or `5000000`
in the data field. Its response is sent completely at the old rate; both ends
switch only after that response finishes. Every FPGA configuration starts at
2 Mbps.

## Version 1 response

The legacy frame length is 16 bytes: one command byte and this 15-byte payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Protocol version (`1`) |
| 1 | 1 | Request opcode with bit 7 set |
| 2 | 1 | Status |
| 3 | 2 | Transaction sequence |
| 5 | 4 | Address copied from the request |
| 9 | 4 | Read data, written data, or capability bits |
| 13 | 2 | CRC-16 |

Status values are `0` success, `1` unsupported version, `2` unsupported
opcode, and `3` bad CRC. Capability bit 0 is 32-bit read and bit 1 is 32-bit
write, bit 2 is streaming, bit 3 is negotiated baud switching, and bit 4 is
validated block writes. A requester
must match the version, opcode, sequence, and address before
accepting a response.

## Block writes

Legacy frame type `0x12` writes 1 to 64 consecutive 32-bit registers in one
transaction. Its frame length is `12 + 4 * count` bytes: one command byte and
this payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Protocol version (`1`) |
| 1 | 1 | Opcode `0x04` |
| 2 | 2 | Transaction sequence |
| 4 | 4 | Word-aligned first register address |
| 8 | 1 | Word count, 1-64 |
| 9 | 4 * count | Register values in address order |
| 9 + 4 * count | 2 | CRC-16 over `0x12` and every preceding payload byte |

The FPGA buffers the words and applies none of them unless the CRC, version,
opcode, count, frame length, and alignment are all valid; it then writes them
in address order. The reply is an ordinary `0x10` version 1 response whose
opcode field is `0x84` and whose data field is the word count. Status `2`
reports an invalid opcode, count, length, or alignment. A requester must hold
the shared link through the response exactly as for a single register write.

## Stream frames

Legacy frame type `0x11` carries a stop-and-credit byte stream. Request payloads
contain version, flags, stream ID, byte offset, 16-bit data length, data, and a
CRC-16. Flags are start (`0x01`), data (`0x02`), end (`0x04`), and cancel
(`0x08`). Data frames carry at most 1024 bytes.

The FPGA replies after consuming each frame with status, echoed flags and
stream ID, the next expected byte offset, receive credit, and CRC-16. The
sender does not transmit the next frame until this acknowledgement arrives.
This gives the FPGA explicit backpressure without additional physical pins.
Start resets the expected offset to zero; end and cancel carry no data.
