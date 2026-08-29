# Razer Laptop Cooling Pad — USB protocol

Everything razerpad knows about the wire protocol of the Razer Laptop Cooling
Pad (model RC21-0230, USB VID `0x1532` PID `0x0F43`), as verified on real
hardware. Fan control was originally reverse-engineered by
[FanControl.RazerCoolingPadPlugin](https://github.com/Benson5650/FanControl.RazerCoolingPadPlugin)
and [razer-coolingpad-linux](https://github.com/Nox1con/razer-coolingpad-linux);
the lighting commands were first verified by this project
(see [openrazer#2382](https://github.com/openrazer/openrazer/issues/2382)).

## Transport

The pad is a full-speed USB HID device with two interfaces. The control
interface is the one whose **feature report** is 90 bytes (91 including the
report ID). All commands are HID **SET_REPORT (feature), report ID 0**;
responses are read back with **GET_REPORT (feature), report ID 0**.

## Report framing (standard Razer wire protocol)

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 1 | status (`0x00` when sending) |
| 1 | 1 | transaction id — **`0x02`** works for every command on this device |
| 2 | 2 | remaining packets (`0x0000`) |
| 4 | 1 | protocol type (`0x00`) |
| 5 | 1 | data size (length of meaningful args) |
| 6 | 1 | command class |
| 7 | 1 | command id |
| 8 | 80 | arguments |
| 88 | 1 | CRC — XOR of bytes 2..87 |
| 89 | 1 | reserved (`0x00`) |

Response status byte (offset 0 of a GET_REPORT after a command):
`0x01` busy · `0x02` success · `0x03` failure · `0x04` timeout ·
`0x05` not supported.

## Fan commands — class `0x0D`

### Set RPM — command `0x01`

Data size `0x03`. Arguments:

| Arg | Value |
|----:|-------|
| 0 | `0x01` |
| 1 | `0x05` (fan / curve id) |
| 2–3 | RPM ÷ 50, little-endian |

Range **500–3200 RPM in 50 RPM steps**. Sending this takes the pad out of its
firmware auto mode; it holds the commanded speed until released.

Example — set 2000 RPM (2000/50 = 0x28):

```
00 02 00 00 00 03 0D 01  01 05 28 00 00 ... 00 23 00
                                            └─ CRC = 03^0D^01^01^05^28
```

### Release control — command `0x10`

Data size `0x03`, arguments `[0x00, 0x06]`, CRC `0x18`. Returns the pad to its
own firmware behavior (its built-in auto mode / physical controls). razerpad
sends this on exit and on system sleep.

### Reading RPM

The pad has **no tachometer**. A GET_REPORT simply echoes the last response
buffer; bytes 10–11 × 50 give the last *commanded* RPM — and only if the last
command was a fan command. After a lighting command the buffer holds that
command's response instead, so the value is garbage until the next fan send.

## Lighting — class `0x0F` (Chroma "extended matrix")

The pad answers the standard openrazer extended-matrix commands with
**transaction id `0x02`** and **LED id `0x00`** (ZERO_LED). First argument is
the variable store (`0x01` = VARSTORE, persists).

### Set effect — command `0x02`

Arguments `[0x01, 0x00, effect, ...]`:

| Effect | Id | Data size | Extra arguments |
|--------|----|----------:|-----------------|
| none (off) | `0x00` | 6 | — |
| static | `0x01` | 9 | args[5] = `0x01`, args[6..8] = R, G, B |
| breathing | `0x02` | 9 | args[5] = `0x01`, args[6..8] = R, G, B |
| spectrum cycle | `0x03` | 6 | — |
| wave | `0x04` | 6 | args[3] = direction (`0x01`/`0x02`), args[4] = `0x28` (speed) |

### Brightness — command `0x04`

Data size 3, arguments `[0x01, 0x00, level]` with level 0–255.

## Quirks

- The pad's firmware validates commands and answers with the status byte —
  a rejected command is simply ignored, which makes probing safe.
- Power draw (measured by Benson5650): ~1 W with fan and LEDs off,
  ~1.15 W at 500 RPM, 15–18 W at full load.
- After the host sleeps or the pad is power-cycled, the pad reverts to its
  firmware behavior; a controller must re-send its state on reconnect/wake.

## Reference implementation

`razerpad.c` in this repository — `razer_build()` for framing,
`pad_set_rpm()` / `pad_release()` for fan, `led_*()` for lighting, and
`led_send()` for the transaction/LED-id probe order used to discover the
values above.
