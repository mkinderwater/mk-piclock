# Changelog

All notable changes to mk-piclock are recorded here.

This project uses a practical changelog focused on user-visible features, compatibility changes, security work, and important internal fixes. Small development builds may be grouped when they formed part of one larger change.

## [1.6.26] - 2026-06-23

### Changed

* Simplified the Message page around the exact OLED preview.
* Applied a direct 180-character message limit.
* Replaced the old character counter and fit-status row with one short helper line.
* Made Home and Messages use the same browser framebuffer renderer.
* Made shared success and warning styles work consistently on Password & Access.

### Removed

* Removed duplicate browser-side OLED framebuffer conversion.
* Removed browser-estimated message wrapping and fit checks.
* Removed the separate message limits request.
* Removed the old message line-count response and display.
* Removed duplicate OLED preview CSS and obsolete preview classes.
* Removed `/api/v1/display/message/limits`.
* Removed `/api/v1/display/message/fit`.
* Removed the corresponding private IPC structures and operations.

### Internal

* Reduced the active C, JavaScript, CSS, and HTML code by a net 157 lines without changing message delivery or scheduling behaviour.

### Compatibility

```text
Product:     1.6.26
HTTP API:    1.8
Private IPC: 11
```

The core and API must be installed and restarted together.

## [1.6.25] - 2026-06-23

### Added

* Added an exact draft framebuffer endpoint for Message previews.
* Added core-rendered preview validation without changing the physical OLED.

### Changed

* Message drafts are now rendered by `mk-piclock-core` using the same FreeType measurements, wrapping, image placement, centring, grayscale, and brightness rules as the OLED.
* Removed browser-estimated text wrapping from the active preview path.
* Centred message text within the 182-pixel text region using measured rendered width.
* Vertically centred the complete line block using actual line heights and gaps.

### Compatibility

```text
Product:     1.6.25
HTTP API:    1.7
Private IPC: 10
```

## [1.6.24] - 2026-06-23

### Added

* Added **Remember this device** for 90-day trusted-browser access.
* Added **Forget All Remembered Devices**.
* Added signed remembered-device tokens backed by a random secret in the protected password record.
* Added Day Image, Bedtime Image, random Day Image, and random Bedtime Image choices to Messages.
* Added image-only messages without automatic placeholder text.

### Changed

* Reorganized the GUI around simple user tasks:

  * Home
  * Alarms
  * Messages
  * Music
  * Day Images
  * Bedtime Images
  * Display
  * Password & Access
  * Recent Activity
* Renamed technical or ambiguous menu labels to clearer user-facing names.
* Moved advanced status, audio, font, filename, and log information into collapsed detail sections.
* Made Messages the single place for intentionally showing a selected image on the OLED.
* Kept Day Images and Bedtime Images focused on upload, review, and deletion.
* Improved horizontal and vertical message centring.
* Added the project background explaining that mk-piclock was created for Rylie.

### Security

* Signing out now forgets the current browser.
* Changing the password invalidates other sessions and remembered devices.
* Cookies remain HttpOnly and SameSite Strict.
* Passwords remain protected with salted PBKDF2-HMAC-SHA256 hashes.

### Compatibility

```text
Product:     1.6.24
HTTP API:    1.6
Private IPC: 9
```

## [1.6.21 to 1.6.23] - 2026-06-23

### Changed

* Refined the authenticated GUI introduced in v1.6.20.
* Simplified page wording and common controls for non-technical users.
* Reduced visible technical detail on everyday pages.
* Improved image-library presentation and message image selection.
* Refined trusted-session handling that became the 90-day remembered-device system in v1.6.24.
* Continued HTTP API and private IPC updates required by the new authentication, messaging, and preview paths.

These releases were short development iterations and are grouped here by feature area.

## [1.6.20] - 2026-06-23

### Added

* Added password protection for the GUI and all control API routes.
* Added the initial `password` login and a dedicated Security page.
* Added salted PBKDF2-HMAC-SHA256 password storage.
* Added HttpOnly and SameSite Strict session cookies.
* Added session expiry and temporary lockout after repeated failed logins.
* Added in-process MP3 decoding and LAME re-encoding inside `mk-piclock-api`.
* Added a recommended mono audio profile:

  * 96 kbps CBR
  * 44.1 kHz
  * 16 kHz low-pass
* Added optional bitrate, sample-rate, and low-pass choices.
* Added music preparation progress and validation.
* Added expanded MP3 information, including album, year, track, genre, duration, bitrate, sample rate, channels, layer, and file size.
* Added browser-local scheduled messages up to 30 days ahead.
* Added persistence for the one pending scheduled message.

### Changed

* Music uploads are optimized before entering the active music library.
* Temporary upload and encoder files are isolated under `.processing`.
* Deleting an active song stops it and resets alarms using it to Random.
* The API service stores and owns the protected password record.

### Security

* The API does not execute shell commands for audio processing.
* The built-in server remains HTTP only and is intended for a trusted LAN, VPN, or HTTPS reverse proxy.

### Compatibility

```text
Private IPC: 7
```

## [1.6.19] - 2026-06-22

### Added

* Added separate **Images** and **Bedtime Images** pages.
* Added matching normal and bedtime image storage directories.
* Added Yellow, Green, and White physical OLED choices for accurate browser previews.
* Added a live 0 to 100 bedtime-brightness control.
* Added the exact 256x64 Home framebuffer preview.

### Changed

* Renamed Faces to Images throughout the GUI and storage model.
* Renamed normal artwork storage to `/opt/mk-piclock/assets/images`.
* Renamed bedtime artwork storage to `/opt/mk-piclock/assets/bedtime-images`.
* Required matching `.png` and converted `.raw` files for manually copied artwork.
* Simplified the GUI and moved advanced font controls out of the normal workflow.
* Improved song-title layout so short metadata remains centred and long metadata scrolls continuously.
* Improved accented Latin text handling so UTF-8 metadata is not replaced byte by byte with question marks.

### Removed

* Removed the old `faces`, `bedtime-faces`, and `bedtime_faces` storage directories during a clean install.
* Removed the dedicated Show Face workflow. Selected images are now sent through Messages.

### Installation

* Standardized Raspberry Pi OS Lite on Debian 13 Trixie with libgpiod 2.x.
* Added `gpu_mem=16` for the headless appliance configuration.
* Documented `make -j2` for Raspberry Pi Zero 2 W and `make -j4` for Raspberry Pi 5.

## [1.6.11 to 1.6.18] - 2026-06-22

### Changed

* Incrementally refined the live OLED preview, GUI layout, image terminology, bedtime controls, and UTF-8 rendering.
* Improved the Message workflow so an image and text could be sent from one page.
* Removed older duplicated GUI elements as the exact framebuffer preview became the source of truth.

These builds were intermediate development releases leading to v1.6.19.

## [1.6.10] - 2026-06-21

### Added

* Added TTP223B touch input on BCM GPIO20.
* Added touch status to the HTTP status response.
* Added a short press to stop active music or an alarm.
* Added a three-second hold to play a random uploaded song.
* Added ID3 title and artist information to the OLED music display.
* Added a scrolling metadata line for long song names.
* Added exact action notices in the GUI.

### Changed

* Network status now reports whether `wlan0` is connected without collecting or displaying the SSID in the core.
* Replaced audio stop polling with condition-variable signalling.
* Audio stop requests now wait passively for the playback thread to finish.
* Track replacement and daemon shutdown use timed condition waits.
* Delete All Music no longer blocks while waiting for audio completion.

### Fixed

* Reduced the risk of API stalls caused by audio shutdown waits.
* Improved audio thread completion and replacement behaviour.
* Corrected MAX98357A documentation so SD / EN remains unconnected with `dtoverlay=max98357a,no-sdmode`.

## [1.6.8] - 2026-06-21

### Added

* Added the first touch-sensor implementation.
* Added touch-driven music stop and hold-to-play behaviour.

### Changed

* Integrated touch state with the core status and GUI diagnostics.

## [1.6.7] - 2026-06-19

### Added

* Added consistent toast-style action notifications across the GUI.

### Changed

* Replaced vague success messages with action-specific results such as alarm saved, screen cleared, song playing, or message scheduled.

## [1.6.6] - 2026-06-19

### Added

* Added ID3v1, ID3v2.2, ID3v2.3, and ID3v2.4 title and artist parsing.
* Added filename fallback when metadata is unavailable.
* Added a metadata-rich `tracks` array while retaining the legacy `files` array.
* Added optional `Title - Artist` display on the OLED.
* Added continuous scrolling for long music metadata.
* Added immediate and delayed messages using 0, 10, 30, or 60 seconds.
* Added pending-message status.

### Compatibility

* Introduced private binary IPC protocol version 4.
* Core and API upgrades became mandatory as a matched pair.

## [1.6.4] - 2026-06-19

### Added

* Added separate restricted `mk-piclock-core` and `mk-piclock-api` service accounts.
* Added hardened systemd units.
* Added bearer-token protection for write operations.
* Added an OpenAPI document and API installation path.
* Added build checks, sanitizer targets, and static analysis support.

### Changed

* Installation now creates and preserves application users, groups, assets, configuration, and API credentials.
* The API and hardware core run as separate services.

## [1.6.3] - 2026-06-19

### Added

* Added versioned binary IPC over `/run/mk-piclock/core.sock`.
* Added fixed request and response headers with operation codes.
* Added shared `util.c`, `util.h`, `asset_store.c`, and `asset_store.h` components.

### Changed

* Removed the internal HTTP server from the hardware core.
* Moved multipart handling, PNG conversion, font validation, directory listing, sorting, and JSON generation into `mk-piclock-api`.

## [1.6.2] - 2026-06-19

### Changed

* Replaced the custom API HTTP server with libmicrohttpd.
* Preserved the `/api/v1` route structure.
* Added a fixed two-thread worker pool.
* Added a 12-connection limit and 15-second connection timeout.
* Kept hardware ownership behind the private core socket.

## [1.5.20] - 2026-06-17

### Added

* Added multiple MP3 upload support.

### Changed

* Removed the separate face preview and message textarea.
* Made the OLED preview the main Message input and preview area.
* Improved the phone-sized Message workflow.

## [1.5.7] - 2026-06-16

### Added

* Established the stable single-daemon native C baseline.
* Added SSD1322 OLED output over SPI.
* Added MAX98357A audio through ALSA and mpg123.
* Added uploaded PNG conversion to packed 4-bit grayscale RAW images.
* Added normal and bedtime image rotation.
* Added seven alarm slots, per-alarm songs, weekday selection, and volume ramping.
* Added 12-hour and 24-hour clock modes.
* Added FreeType TTF and OTF clock fonts with OLED gamma correction.
* Added screen messages, bedtime dimming, clock naming, and startup greeting.
* Added the original built-in mobile web GUI.

### Known limitations

* The project still used one root-capable service and one large source file.
* Browser message previews could differ slightly from the physical OLED.
* Audio start could produce a small speaker click.

## Earlier development

Earlier native C builds established the hardware fundamentals:

* SSD1322 output on `/dev/spidev0.0`
* 256x64 packed 4-bit grayscale framebuffer
* GPIO25 for OLED DC
* GPIO27 for OLED reset
* SPI writes split into safe chunks
* migration from sysfs GPIO to libgpiod
* direct ALSA playback through MAX98357A

The Python predecessor and early C prototypes are not itemized in this changelog.
