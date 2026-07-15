# DockIR

DockIR is a Raspberry Pi Pico IR receiver that emulates a USB HID keyboard.
This branch is for adapting the firmware to the slim aluminum Apple Remote.

The current goal is to replace the original remote mapping with Apple Remote
decoding while keeping the Pico as the USB keyboard interface. Research notes,
captured frame examples, and decoding decisions are documented in
[APPLE_REMOTE.md](APPLE_REMOTE.md).

## Apple Remote Scope

This branch targets the aluminum Apple Remote IR protocol:

- NEC-like IR timing with Apple-specific payload bytes.
- Normal button frames in the `ee 87 <command_with_parity> <remote_id>` form.
- Aluminum Center and Play/Pause prefix handling.
- Remote ID locking or pairing support to reject corrupted frames.

Legacy Apple Remote behavior is documented in [APPLE_REMOTE.md](APPLE_REMOTE.md),
but the planned production decoder can focus on the aluminum remote only.

## Build

Set up the Raspberry Pi Pico SDK, then configure and build with CMake:

```sh
mkdir -p build
cd build
cmake ..
cmake --build . -j4
```

The build produces:

- `build/pico_ir_keyboard.elf` for debugger loading.
- `build/pico_ir_keyboard.uf2` for drag-and-drop flashing to the Pico.

The default build intentionally uses a 4 Mbit / 512 KiB logical flash layout
(`DOCKIR_FLASH_SIZE_BYTES=524288`). That keeps one firmware image usable on both
standard Pico development boards and the smaller production board. The paired
remote ID is stored in the final 4 KiB sector of that logical flash range.

## Serial Logging

Firmware diagnostics are written to RP2040 UART0 stdio only:

- TX: `GP0`
- RX: `GP1`
- Baud: `115200`

USB CDC stdio is intentionally disabled, so the USB device profile remains the
keyboard HID interface only.

## Hardware Notes

- IR receiver input defaults to `GP27`.
- The original hardware test receiver was a TSOP4438.
- The firmware uses the PIO NEC receiver in `nec_receive_library/`.

## Source Layout

- `src/main.c`: IR decode and HID output behavior.
- `src/usb_descriptors.c`: TinyUSB HID descriptors.
- `nec_receive_library/`: PIO-based NEC frame receiver.
- `APPLE_REMOTE.md`: Apple Remote protocol notes and captured test data.
