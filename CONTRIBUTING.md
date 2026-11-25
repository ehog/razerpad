# Contributing

Thanks for your interest! This is a small project with a simple layout —
most contributions need nothing more than `make` and (for anything
device-facing) the pad itself.

## Layout

| Path | What it is |
|------|------------|
| `razerpad.c` | The daemon/CLI — SMC temperature, HID protocol, fan curve, lighting, config/status plumbing. Single file, no dependencies. |
| `razerpadbar.swift` | Menu bar readout / mode switcher. |
| `dashboard.py` | Localhost web server (Python stdlib only). |
| `dashboard.html`, `settings.html`, `app.js`, `style.css` | The web UI. No frameworks, no external assets. |
| `docs/PROTOCOL.md` | The reverse-engineered USB protocol. |
| `*.plist.in` | launchd agent templates, filled in by the Makefile. |

## Building & testing

```sh
make              # daemon
make razerpadbar  # menu bar app
python3 -m py_compile dashboard.py && node --check app.js
```

Run the daemon in the foreground with `./razerpad -v` while developing.
Device-facing changes should be tested against real hardware, including the
unfriendly paths — unplug the pad mid-run, replug it, sleep/wake the Mac.
(Both bugs fixed in v1.1.0 were found exactly this way.)

## Ground rules

- **No kernel code, no root, and no SMC writes.** The project's safety story
  (see README) depends on staying an unprivileged userspace tool that only
  reads sensors. PRs that write SMC keys or drive internal fans won't be
  merged.
- Keep the zero-dependency property: plain C + IOKit, Python stdlib,
  vanilla JS.
- Match the style around you; keep the web UI offline-capable.
- New protocol findings belong in `docs/PROTOCOL.md` (and are worth
  cross-posting to [openrazer#2382](https://github.com/openrazer/openrazer/issues/2382)).

## Especially wanted

- Native Apple Silicon temperature support
  ([#1](https://github.com/ehog/razerpad/issues/1))
- Fan-curve presets/profiles
  ([#2](https://github.com/ehog/razerpad/issues/2))
- Reports of whether other Razer cooling hardware answers the same protocol
