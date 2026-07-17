# mk-piclock v1.8.1 Release Notes

## GUI consistency polish

- Added a consistent divider beneath every GUI card heading.
- Changed inactive sound labels from `Quiet` to `None`.
- Standardized spacing between RGB Lighting and Global Controls.
- Removed the duplicate divider beneath the Web password description.
- Added matching headings to Story playback and Activity history.
- Refreshed browser asset identifiers to prevent stale cached CSS or modules.

## Production display and code cleanup

- Removed the obsolete OLED pill renderer, rounded-pill geometry, Wi-Fi icon, music-note icon, and alarm-volume pill plumbing.
- Restored the approved footer format: `W.xxx | ALARM ON`, with `W.OFF` blinking when `wlan0` is not connected.
- Right-aligned the time and date to a fixed two-pixel display margin.
- Restored the permanent one-pixel footer separator with a moving black seconds position.
- Changed footer network detection to require Wi-Fi carrier and an assigned IPv4 address, refreshed once per second.
- Removed the redundant `trim_ascii_line()` and `read_first_line_from_file()` helpers.
- Added build enforcement for unused static functions and implicit function declarations.
- Removed the unneeded `gpiod` command-line package. `libgpiod-dev` remains required.
- Audited all C translation units and browser modules. No other unreferenced functions were found.
- Kept HTTP API 1.25 and private IPC 16.

## Versions

```text
Product:     1.8.1
HTTP API:    1.25
Private IPC: 16
```
