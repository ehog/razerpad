# Changelog

Notable changes to razerpad-mac. Format follows
[Keep a Changelog](https://keepachangelog.com/); versions follow
[SemVer](https://semver.org/).

## [Unreleased]

### Added
- `?range=1|24|168` URL parameter to preselect the dashboard history view
- Protocol documentation (`docs/PROTOCOL.md`), changelog, contributing
  guide, bug report template, README screenshots + troubleshooting/uninstall

### Fixed
- Dashboard server 404'd on any path with a query string (routing now
  ignores the query component)

## [1.1.0] — 2026-08-28

First tagged release.

### Added
- Fan-curve auto-regulation daemon: Intel SMC temperature → HID fan commands,
  with hysteresis and hot-plug recovery
- Chroma lighting control: off / static / spectrum / wave / breathing /
  reactive (color tracks CPU temperature), plus brightness — lighting
  protocol verified on hardware (transaction `0x02`, LED id `0x00`)
- Sleep/wake handling: pad released on system sleep, control and lighting
  re-taken on wake
- `battery_quiet`: run the curve quieter on battery power
- `override` modes (auto / quiet / max) and **razerpadbar**, a menu bar
  readout and mode switcher
- Local web dashboard (127.0.0.1:8737): live temperature/RPM, fan-curve
  editor, lighting controls, °C/°F display toggle
- Persistent thermal history (30 s CSV averages) with 1h/24h/7d dashboard
  views and summary stats
- `temp_cmd`: pluggable temperature source for Apple Silicon / custom sensors
- Homebrew tap (`brew install ehog/tap/razerpad`) and GitHub Actions CI

### Fixed
- Hot-replug detection: recreate the IOHIDManager on reconnect attempts
  (without a run loop its device list never refreshes)
- Bounded disconnect-detection latency with an RPM heartbeat (~30 s worst
  case at steady temperature)

### Security
- Dashboard config API requires the `X-Razerpad` header (CSRF guard);
  `temp_cmd` is deliberately not settable through the web API

[Unreleased]: https://github.com/ehog/razerpad-mac/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/ehog/razerpad-mac/releases/tag/v1.1.0
