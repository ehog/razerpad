---
name: Bug report
about: Something misbehaves — daemon, dashboard, menu bar, or the pad itself
labels: bug
---

**What happened / what you expected**


**Environment**
- Mac model + macOS version:
- Intel or Apple Silicon (and `temp_cmd` if set):
- How installed (make / Homebrew) and version (`git describe --tags` or release):

**Diagnostics** (paste what applies)
```
razerpad -t          # temperature read
razerpad -r          # pad readback (only meaningful after a fan command)
tail -30 /tmp/razerpad.log
system_profiler SPUSBDataType | grep -A4 "Razer"
```
