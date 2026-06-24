# mk-piclock Changelog

This file records user-visible changes to the native C release.

## v1.6.37 - 2026-06-23

### Music queue reliability

- Limited MP3 upload to one file per request.
- Refused new uploads while any song is queued or processing.
- Added a browser-side queue check before upload.
- Added an atomic API-side busy check to prevent overlapping requests.
- Disabled upload controls while the worker is busy.
- Added **Clear Queue** for waiting uploads and their temporary source files.
- Kept the active transcode running when the waiting queue is cleared.
- Changed the processing view so the active job is listed before queued and completed jobs.
- Updated HTTP API to 1.16.

### Cause corrected

The previous uploader could start transcoding the first file while the same request was still validating and staging later files. It also accepted another upload while the worker was active. On a Raspberry Pi, this allowed concurrent MP3 decoding and heavy storage activity. Later items could remain displayed as **Queued**, while the older active item was hidden by the limited newest-first list.

## v1.6.36 - 2026-06-23

### Access-control cleanup

- Removed the final obsolete access-control path from the API systemd service.
- Removed old install and uninstall cleanup commands for the deleted auth directory.
- Removed historical access-control compatibility text from active documentation.
- Confirmed that no auth routes, auth source modules, password files, remembered-browser keys, or OpenAPI security definitions remained.
- Kept HTTP API 1.15 and private IPC 11 unchanged.

## v1.6.35 - 2026-06-23

### Direct local controls

- Removed the GUI login screen.
- Removed password requirements.
- Removed remembered-device handling.
- Removed browser session and local storage authentication state.
- Removed credential, lockout, recovery, and access-key logic.
- Removed all `/api/v1/auth/*` routes.
- Removed the Security page and sign-out controls.
- Removed `auth_store.c` and `auth_store.h`.
- Removed the `/opt/mk-piclock/auth` data directory.
- Made all GUI and API routes directly available on the trusted local network.
- Updated HTTP API to 1.15.

### Retained

- Restricted core and API system users
- Private Unix socket between services
- Day and Bedtime **Download All PNGs** actions
- Centered mK browser, Apple, Android, and GUI icons

## v1.6.34 - 2026-06-23

### Historical access-key fix

This release still contained the password system that was removed in v1.6.35.

- Corrected valid access keys being rejected immediately after login.
- Moved authentication evaluation until after libmicrohttpd request parsing.
- Retained stale-response protection.
- Simplified the old remembered-browser implementation.
- Added SSH password recovery instructions.
- Retained login throttling and PBKDF2-HMAC-SHA256 password storage.

### Image library downloads

- Added or retained **Download All Day PNGs**.
- Added or retained **Download All Bedtime PNGs**.
- Excluded converted OLED `.raw` files from download ZIPs.

### Icons

- Added centered mK favicon sizes.
- Added Apple touch and Android launcher icons.
- Used the centered mK artwork in the GUI header.

## Earlier v1.6 development - consolidated

The following work was completed throughout earlier v1.6 releases. Some intermediate builds were experimental and were replaced by later implementations.

### Native architecture

- Moved the clock to a native C core and native C HTTP API.
- Split hardware ownership from web and asset processing.
- Added private binary IPC over `/run/mk-piclock/core.sock`.
- Replaced legacy GPIO access with libgpiod 2.x.
- Added systemd services and restricted service accounts.
- Added service hardening and limited writable paths.

### OLED and clock rendering

- Added SSD1322 256x64 SPI support.
- Added chunked framebuffer writes for SPI transfer limits.
- Added a large centred clock, full date, and blinking colon.
- Added 12-hour and 24-hour modes.
- Added built-in and uploaded font selection.
- Added FreeType caching and grayscale gamma handling.
- Added Wi-Fi and alarm status indicators.
- Added rotating images beside the clock.
- Added separate day and bedtime image rotation.
- Added OLED panel colour selection for browser previews.
- Added live raw framebuffer display on the Home page.

### Alarms and audio

- Added seven configurable alarms.
- Added weekday schedules.
- Added specific or random wake-up music.
- Added per-alarm start and final volumes.
- Added song-length-based alarm volume ramping.
- Added MP3 playback with mpg123 and ALSA.
- Added global volume control.
- Added title and artist display.
- Added scrolling long metadata.
- Improved common accented Latin metadata handling.
- Added touch input: short press stops audio and a three-second hold plays random music.

### Messages

- Added immediate OLED messages.
- Added delayed messages.
- Added scheduled messages up to 30 days ahead.
- Added specific and random Day or Bedtime image selection.
- Added image-only messages.
- Added persistent pending-message state.
- Replaced approximate browser wrapping with core-rendered OLED previews.
- Centred message lines using actual rendered dimensions.

### Image libraries

- Separated Day Images and Bedtime Images.
- Added PNG validation and conversion to OLED raw assets.
- Added upload, review, pagination, individual deletion, and delete-all controls.
- Preserved original PNG files for browser display and download.
- Removed the older face-specific terminology from current storage paths and GUI labels.

### Music library

- Added MP3 validation and in-process LAME transcoding.
- Added a recommended mono profile for the MAX98357A and small speaker.
- Added processing progress and job history.
- Added music metadata inspection.
- Added play, stop, and delete controls.
- Reset alarms to Random when their selected file is deleted.

### Web GUI

- Reorganized controls around everyday tasks.
- Added Home, Alarms, Messages, Music, Day Images, Bedtime Images, Display, and Recent Activity pages.
- Added responsive phone and tablet layouts.
- Moved technical options into collapsed sections.
- Added API and GUI version compatibility checks.
- Added recent event-log viewing.
- Added network and uptime status.

### Installation and hardware documentation

- Standardized Raspberry Pi OS Lite Debian 13 Trixie instructions.
- Added SPI, MAX98357A, GPIO, OLED, touch, and power wiring documentation.
- Added `gpu_mem=16` configuration.
- Documented service verification and journal inspection.
- Documented image migration from older face directories.

## Compatibility table

| Product | HTTP API | Private IPC | Notes |
| --- | ---: | ---: | --- |
| 1.6.37 | 1.16 | 11 | Single MP3 upload and queue busy enforcement |
| 1.6.36 | 1.15 | 11 | Final access-control cleanup |
| 1.6.35 | 1.15 | 11 | Password system removed |
| 1.6.34 | 1.14 | 11 | Last password-protected release |

Core, API, web assets, and OpenAPI schema should be installed from the same release.
