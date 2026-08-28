# razerpad-mac

Automatic fan-speed regulation **and Chroma lighting control** for the
**Razer Laptop Cooling Pad** on macOS (Intel Macs). Replicates what Razer
Synapse does on Windows: reads the CPU die temperature and continuously adjusts
the pad's fan speed along a fan curve. Includes a local web dashboard.

Single dependency-free C binary using IOKit:

- **CPU temperature** — read from the SMC (`TC0P`/`TC0D`/per-core keys; the
  hottest sane reading wins).
- **Fan control** — 90-byte HID feature reports to the pad
  (VID `0x1532`, PID `0x0F43`). Protocol reverse-engineered by
  [FanControl.RazerCoolingPadPlugin](https://github.com/Benson5650/FanControl.RazerCoolingPadPlugin)
  and [razer-coolingpad-linux](https://github.com/Nox1con/razer-coolingpad-linux).

Range is 500–3200 RPM in 50 RPM steps. The pad has no tachometer — the RPM it
reports back is the last commanded value. On exit (or `-o`) control is released
back to the pad's own firmware.

## Requirements

- An Intel Mac (the temperature path reads the Intel SMC; see Notes)
- Xcode Command Line Tools (`xcode-select --install`)
- The Razer Laptop Cooling Pad (RC21-0230, USB VID `0x1532` PID `0x0F43`)
- Python 3 (ships with macOS) for the optional web dashboard

## Build & install

```sh
make            # build ./razerpad
make install    # copy to /usr/local/bin
make agent      # install + start the launchd login agent (auto-run)
make unagent    # stop and remove the login agent
```

The agent (`com.razerpad.fancurve`) runs at login, survives pad hot-plugging,
and logs to `/tmp/razerpad.log`.

## Usage

```sh
razerpad            # run the fan-curve loop in the foreground
razerpad -t         # print CPU temperature
razerpad -r         # print the pad's current commanded RPM
razerpad -s 2000    # set a fixed RPM and exit
razerpad -o         # release fan control back to the pad
razerpad -v -i 2    # verbose loop, 2 s poll interval
razerpad -C "40:0,55:30,70:60,85:100"   # custom curve (°C:percent pairs)

# Lighting (standard Chroma extended-matrix protocol, class 0x0F,
# transaction 0x02 / LED id 0x00 — probed and confirmed on this pad):
razerpad -L static:ff0044   # static color (RRGGBB)
razerpad -L spectrum        # spectrum cycle
razerpad -L wave            # wave (or wave2 for reverse)
razerpad -L breath:00ff00   # breathing
razerpad -L off             # LEDs off
razerpad -B 60              # brightness 0-100
```

Caveat: `-r` reads the pad's last HID response buffer, so it is only meaningful
after a fan command — after an LED command it returns garbage until the daemon's
next fan update.

## Web dashboard

`make dashboard` installs a second launchd agent (`com.razerpad.dashboard`)
serving **http://127.0.0.1:8737** (localhost only; stdlib Python, no deps).
Two pages with a Razer-inspired dark design system (`style.css`):

- **Dashboard** (`/`) — live temperature and fan RPM tiles with meter bars,
  history charts, and a read-only fan-curve view with the current operating
  point. Click the °C/°F unit on the temperature tile to flip units.
- **Settings** (`/settings`) — temperature unit (display-only, stored in the
  browser via localStorage; the config and protocol stay in °C), fan-curve
  editor with live preview (inputs shown in the selected unit), poll interval,
  and lighting controls.

`make undashboard` removes it.

Plumbing: the daemon writes live status to `/tmp/razerpad-status.json` each
cycle and hot-reloads `~/.config/razerpad/config` (key=value: `interval`,
`curve`, `led`, `led_color`, `brightness`) when it changes; the dashboard reads
the former and writes the latter. LED settings from the config are re-applied
on config change and whenever the pad reconnects. Set `led=unmanaged` to leave
lighting alone entirely.

Default curve: 40 °C → 0 %, 60 → 15, 75 → 25, 85 → 40, 93 → 65, 98 → 100
(percent maps linearly onto 500–3200 RPM). The top end is intentionally flat —
this i9 idles in the high 80s °C, so full speed is reserved for near-thermal-limit
temperatures. A 50 RPM hysteresis suppresses wobble from sensor noise.

To change the curve the agent uses, edit `ProgramArguments` in
`~/Library/LaunchAgents/com.razerpad.fancurve.plist` (add `-C "..."`), then
`launchctl unload` + `load` it.

## Notes

- Intel-only temperature path (AppleSMC). On Apple Silicon the SMC keys differ;
  the HID side would work unchanged — contributions welcome.
- No root or special TCC permissions needed — the pad is a vendor-specific HID
  device.

## License

MIT — see [LICENSE](LICENSE).

This project is not affiliated with, endorsed by, or supported by Razer Inc.
"Razer" and "Chroma" are trademarks of Razer Inc., used here only to identify
the hardware this tool talks to. Use at your own risk.
