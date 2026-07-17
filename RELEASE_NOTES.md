# mk-piclock v1.8.0 Release Notes

## Production cleanup and warning prevention

- Removed the redundant `trim_ascii_line()` and `read_first_line_from_file()` helpers.
- Simplified the Wi-Fi status check to read `wlan0` state directly, then fall back to `/proc/net/wireless` as before.
- Added build enforcement for unused static functions and implicit function declarations.
- Removed the unneeded `gpiod` command-line package from the build dependency list. The required libgpiod library remains installed through `libgpiod-dev`.
- Audited all C translation units for unreferenced static and exported functions. No other dead functions were found.
- Kept runtime behaviour, HTTP API 1.25, and private IPC 16 unchanged.

## Versions

```text
Product:     1.8.0
HTTP API:    1.25
Private IPC: 16
```
