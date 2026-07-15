# DockIR IR Reader

This branch turns DockIR into a Raspberry Pi Pico IR frame reader. Instead of
emulating a USB HID keyboard, the firmware decodes NEC-timed IR frames and
prints the received payloads to the Pico serial console.

The immediate purpose is to inspect Apple Remote IR traffic before changing the
production keyboard-emulation firmware. Captured Apple Remote findings and
decoding notes are in [IR_READER.md](IR_READER.md).

## What This Branch Does

- Receives IR on `GP27` using the PIO NEC receiver.
- Prints every decoded 32-bit NEC-shaped frame over USB CDC and UART stdio.
- Shows payload bytes and grouped binary bits for visual comparison.
- Identifies standard NEC frames, Apple-looking frames, repeat markers, and
  invalid NEC-shaped payloads.
- Normalizes Apple Remote command bytes by removing the command parity bit.

This branch is for testing and protocol discovery. It intentionally does not
send USB HID keyboard reports.

## Build

Set up the Raspberry Pi Pico SDK, then configure and build:

```sh
mkdir -p build
cd build
cmake ..
cmake --build . -j4
```

The build produces:

- `build/pico_ir_keyboard.elf` for debugger loading.
- `build/pico_ir_keyboard.uf2` for drag-and-drop flashing to the Pico.

## Serial Output

Flash the UF2, open the Pico serial port, then press buttons on an IR remote.
For Apple Remote frames, output looks like:

```text
[12345 ms] #7 raw=0xf70687ee bytes=ee 87 06 f7 bits=11101110 10000111 00000110 11110111 apple=yes custom=ok command_raw=0x06 command=0x03 (Right / Next) remote_id=0xf7 parity=ok sequence=none
```

Important fields:

- `raw`: full 32-bit frame as received from the PIO FIFO.
- `bytes`: payload bytes in transmit order.
- `bits`: the same payload bytes in binary, grouped by byte.
- `apple=yes`: the frame matches the Apple `ee 87` namespace.
- `command_raw`: Apple command byte including bit-0 parity.
- `command`: normalized Apple command, equal to `command_raw >> 1`.
- `remote_id`: Apple remote or pair ID byte.
- `parity`: whether the Apple command parity bit validates.
- `sequence`: Center/Play prefix-to-tail detection for aluminum remotes.

Standard NEC frames are printed as `apple=no nec=valid`. NEC-shaped payloads
that fail both Apple and standard NEC validation are printed as
`apple=no nec=invalid`.

## Apple Remote Notes

The reader confirmed that the slim aluminum Apple Remote uses Apple-specific
NEC-like frames and can produce occasional corrupted bit patterns. Details are
recorded in [IR_READER.md](IR_READER.md), including:

- Apple payload layout and command parity.
- Observed command table for the tested remote.
- Center and Play/Pause two-frame behavior.
- Remote ID locking recommendations.
- Pairing/control frame observations.

## Source Layout

- `src/main.c`: serial frame reader and Apple Remote interpretation.
- `nec_receive_library/`: PIO-based NEC frame receiver.
- `IR_READER.md`: captured Apple Remote research and decoding notes.
