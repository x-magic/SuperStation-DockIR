# Apple Remote Research

These notes record the Apple Remote protocol research and captured reader output
used to adapt DockIR for the slim aluminum Apple Remote.

## Online Reference

The [Apple Remote technical notes on Wikipedia](https://en.wikipedia.org/wiki/Apple_Remote#Technical_details)
describe it as a modified NEC stream with the normal NEC timing envelope:

- leader: 9000 us burst, 4500 us space
- data bits: 560 us burst followed by 560 us (`0`) or 1690 us (`1`) space
- 32 data bits, sent least-significant bit first

The Apple payload layout is documented as:

```text
0xEE 0x87 <command_with_parity> <remote_id>
```

The final byte is documented as a one-byte remote ID in the range 0-255. That
is the value this branch prints as `remote_id`.

The least significant bit of the command byte is a parity bit. The reader
therefore prints both:

- `command_raw`: the exact third byte from the frame.
- `command`: the normalized command value, equal to `command_raw >> 1`.

## Serial Output

Flash `build/pico_ir_keyboard.uf2`, then open UART0 with a TTL adapter. The
build enables UART stdio on `GP0`/`GP1` at 115200 baud. USB CDC stdio is
intentionally disabled so the USB port only exposes the keyboard HID interface.

Example lines:

```text
[23 ms] DockIR Apple Remote firmware boot
[24 ms] stdio=uart tx=GP0 rx=GP1 baud=115200 usb_cdc=disabled ir_gpio=GP27
[25 ms] pairing=none accepting=any-apple-remote
[12345 ms] button raw=0xf70687ee bytes=ee 87 06 f7 command=0x03 name=Right remote_id=0xf7 hid=ArrowRight paired=0
[12680 ms] tail raw=0xf70587ee bytes=ee 87 05 f7 remote_id=0xf7 active_hid=Enter
[45012 ms] pairing_request raw=0xf70287e0 bytes=e0 87 02 f7 remote_id=0xf7 count=5/5
[45013 ms] pairing=stored remote_id=0xf7
[60002 ms] frame raw=0xfb8287ee bytes=ee 87 82 fb ignored=bad-parity
```

Fields to check:

- `bytes=ee 87 ...` confirms the Apple-specific address/custom-code pattern.
- `command=...` shows the normalized Apple command byte and the branch's
  current command label.
- `remote_id=...` is the Apple pair/remote ID byte from the frame.
- `hid=...` shows the USB keyboard key emitted for the decoded command.
- `tail ... active_hid=...` shows the aluminum remote's shared `0x02` tail
  frame being ignored as a separate key while preserving hold/repeat state.
- `ignored=...` gives the reject reason for malformed or unsupported frames.

## Captured Aluminum Apple Remote Pattern

The supplied reader output from this remote used `remote_id=0xf7` and confirmed
the Apple custom bytes `ee 87`.

| Button | Observed `command_raw` | Normalized `command` |
| --- | --- | --- |
| Menu | `0x03` | `0x01` |
| Right | `0x06` | `0x03` |
| Left | `0x09` | `0x04` |
| Up | `0x0a` | `0x05` |
| Down | `0x0c` | `0x06` |
| Center prefix | `0x5c` | `0x2e` |
| Play/Pause prefix | `0x5f` | `0x2f` |
| Center/Play tail | `0x05` | `0x02` |

## Center And Play/Pause

The aluminum remote sends two full frames for Center and Play/Pause:

```text
Center:     0x2e prefix, then 0x02 tail
Play/Pause: 0x2f prefix, then 0x02 tail
```

When a button is held, NEC repeat frames follow the tail frame. The tail exists
for compatibility with older Apple Remote behavior, where Center/Play was a
shared action. A receiver that understands the aluminum remote should use the
prefix to distinguish Center from Play/Pause.

In testing, the prefix frame was more reliable than the tail frame. Some Center
and Play/Pause presses produced a valid prefix followed by a corrupted second
frame such as:

```text
ee c7 82 fb
ee 47 ae fb
ee 87 82 fb
```

For aluminum-only production decoding, treat the prefix as the logical button
event and do not emit an action for the tail:

- `0x2e` prefix: emit Center and set the active held button to Center.
- `0x2f` prefix: emit Play/Pause and set the active held button to Play/Pause.
- `0x02` tail: ignore if it follows a Center or Play/Pause prefix.
- NEC repeat marker: repeat the active held button.

This drops support for legacy Apple Remotes that only send the shared `0x02`
action, but it avoids depending on the flaky tail frame from the aluminum
remote.

## Remote ID Locking

The observed normal button frames use `remote_id=0xf7`. Rare corrupted frames
may decode as another ID, for example `0xfb`, while still passing parity. Parity
is only one bit, so it cannot reject every multi-bit corruption.

Production decoding should lock to one remote ID:

- Accept only `ee 87` frames with `parity=ok`.
- Accept only the configured or learned `remote_id`.
- Reject unknown commands.
- Ignore corrupt frames without clearing the currently held logical button
  until the hold timeout expires.

## Pairing Frames

Holding the [Apple Remote pairing key combination](https://support.apple.com/en-au/101285)
produced a separate repeated stream after a short delay. The stable frame was:

```text
e0 87 02 f7
```

This looks like an Apple pairing/control namespace rather than normal button
input:

- Normal button namespace: `ee 87 <command_with_parity> <remote_id>`
- Pairing/control namespace: `e0 87 02 <remote_id>`

For production pairing support, do not treat `e0 87 02 f7` as a button. Instead
use it as a pairing request and only lock to the remote ID after several
matching pairing frames, for example five matching frames within one second.
Noisy pairing-mode captures also included corrupted variants such as `e0 47`,
`e0 07`, and `f0 43`; those should be ignored.

## Test Checklist

Press each remote button and capture the serial lines:

- Confirm all normal button presses start with `bytes=ee 87`.
- Confirm the command byte matches the expected Apple command set:
  `0x01` Menu, `0x03` Right, `0x04` Left, `0x05` Up, `0x06` Down.
- Confirm `remote_id` is stable across all buttons from the same remote.
- Press Center repeatedly and check for a `0x2e` prefix command followed by
  `0x02`.
- Press Play/Pause repeatedly and check for a `0x2f` prefix command followed by
  `0x02`.

If a line prints `apple=no nec=valid`, the frame is a standard NEC frame rather
than the Apple layout. If it prints `apple=no nec=invalid`, the PIO timing layer
captured a 32-bit NEC-shaped payload that does not satisfy standard NEC inverse
byte validation.
