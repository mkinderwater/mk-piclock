# mk-piclock v1.7.6

mk-piclock is a native C bedside alarm clock for Raspberry Pi Zero and Zero 2 W. It drives a 256x64 SSD1322 OLED, MAX98357A I2S amplifier, speaker, TTP223B touch sensor, and optional RGB LED.

The project was created for my daughter Rylie. It is designed to behave like a simple bedside appliance, not a general-purpose computer.

## What's new in v1.7.6

- Removed unsupported parentheses from the OLED Wi-Fi signal line so it no longer ends with a fallback question mark.
- Simplified the touch instructions: keep holding the sensor to open network diagnostics.
- Retained clean OLED song metadata filtering. Unsupported characters are omitted while the original title and artist remain unchanged in the GUI and API.
- Kept HTTP API 1.25 and private IPC 16.


## Features

### Clock and display

- Large 12-hour or 24-hour clock
- Full date under the time
- Blinking colon
- Day and bedtime image libraries
- Automatic image rotation
- Wi-Fi and alarm status indicators
- Alarm volume indicator while an alarm is playing
- OLED brightness control
- Separate bedtime schedule and brightness
- Yellow, green, or white panel selection for browser previews
- Built-in and uploaded TrueType or OpenType fonts
- Exact 256x64 live framebuffer preview on the Home page

### Alarms

- Seven independent alarm slots
- Per-alarm time and weekday selection
- Specific or random uploaded music
- Separate starting and final volume
- Volume ramp across the song
- Next alarm shown on Home and System
- Last successful alarm recorded on System

#### Alarm safety

When an alarm starts:

- the selected song repeats until the touch sensor is pressed
- browser stop controls cannot dismiss the alarm
- an unreadable or missing song falls back to the protected built-in alarm
- the alarm stops after 30 minutes if nobody presses the sensor

The fallback alarm is installed at:

```text
/opt/mk-piclock/assets/default-alarm.mp3
```

It is retained during factory reset and is not part of the user music library.

### Kid-friendly touch control

- Press while an alarm is active: dismiss the alarm
- Short press while music or a story is playing: stop the audio
- Hold, then release: play a random song
- Keep holding the sensor: open the OLED network diagnostic screen
- Tap while diagnostics are open: close diagnostics
- Ten short taps within eight seconds: play a random story when Story Mode is enabled
- Any touch press: blink the RGB LED using the selected touch-feedback colour

The music action runs only when the sensor is released. Keep holding to open diagnostics instead, so the same press cannot start music. A diagnostic hold is not counted toward Story Mode.

Alarms cannot be dismissed from the browser. The physical touch sensor must be pressed.

#### OLED network diagnostics

Simply hold down the touch sensor to open the diagnostic screen. It shows:

- Wi-Fi network name
- Wi-Fi signal strength
- IP address
- hostname

It closes after 30 seconds or immediately when the touch sensor is tapped. Network values refresh while the screen is open.

### Bedtime behaviour

Bedtime mode uses its own schedule, image library, and brightness. It lets the clock remain useful at night without keeping the OLED unnecessarily bright.

Day Images and Bedtime Images are stored separately. During bedtime, the clock uses bedtime artwork and the configured bedtime brightness. Parents can disable touch-started music during bedtime while leaving alarms enabled.

### Messages

Parents can send a message to the OLED:

- immediately
- after 10, 30, or 60 seconds
- at a browser-local date and time within the next 30 days

A message can use:

- text only
- a specific Day Image
- a specific Bedtime Image
- a random Day Image
- a random Bedtime Image
- an image without text

One future message can be pending. A new scheduled message replaces the existing one. Pending messages survive a core restart. Each message can optionally play the built-in short chime when it appears. The chime is skipped when other audio is already playing.

The preview is rendered by `mk-piclock-core` using the same framebuffer, font measurements, word wrapping, image placement, centring, grayscale, and brightness rules as the physical OLED.

### Images

Day Images and Bedtime Images each support:

- PNG upload
- OLED conversion
- paged library review
- individual deletion
- delete all
- download all original PNG files as a ZIP

The download ZIP excludes converted `.raw` files.

### Music

Uploaded MP3 files are decoded and re-encoded inside `mk-piclock-api`. No shell command or external encoder is launched.

The recommended profile is:

- mono
- 96 kbps CBR
- 44.1 kHz
- 16 kHz low-pass

The Music page includes:

- multiple-file upload
- selected-file count and total upload size before upload
- upload and processing progress for queued and active jobs
- play, stop, and delete controls
- global volume
- optional encoding settings
- title, artist, album, year, track, genre, duration, bitrate, sample rate, channel, MPEG layer, and file size when available

Long `Title - Artist` text scrolls across the OLED. Short text remains centred.

Before OLED rendering, the title and artist are filtered against the built-in 5x7 glyph set. Unsupported Unicode, emoji, and symbols are omitted instead of becoming question marks. Filtering affects only the OLED copy. Original ID3 metadata remains available in the browser and API.

#### Upload queue rule

A batch of MP3 files may be uploaded at one time. A new upload is refused while any song from the current batch is queued or processing.

The full selection is validated and staged before any job is queued. If staging or queueing fails, the staged files are rolled back instead of leaving a partial batch. This keeps upload validation and transcoding from competing for CPU, memory, and storage on the Raspberry Pi.

**Clear Queue** deletes waiting jobs and their temporary source files. It does not interrupt the file currently transcoding.

### Stories

Story Mode uses a separate MP3 library stored under `assets/stories`. It can be enabled or disabled by a parent from the Stories page and has its own volume control.

When the touch sensor is tapped ten times within eight seconds:

1. a random story is selected
2. the OLED clears and shows up to five random Bedtime Images
3. the configurable intro message appears, defaulting to `STORY MODE!`
4. after three seconds, the selected story title appears in the normal clock footer
5. after four more seconds, the standard date returns while the story keeps playing

A single touch stops the story. Stories are not used by alarms and are not included in backups.

### RGB activity lighting

A common-cathode RGB LED can use a separate profile for each activity:

- Alarm
- Bedtime
- Message
- Music
- Daytime
- Stories

Each profile supports a solid colour, a fade between two colours, or rainbow mode. Brightness and the full effect-cycle time are configurable. The Lighting page can preview unsaved settings for ten seconds and includes red, green, blue, and white wiring tests.

Touch feedback is handled as a high-priority temporary LED scene while the touch sensor is pressed. Its colour and brightness live under Lighting, Global Controls, so it can stay consistent no matter which activity profile is currently active. The default touch feedback is a white blink.

Global controls provide a master switch, maximum brightness, per-channel calibration, optional idle-off behaviour, a gradual fade before bedtime, and touch-blink settings. Profile previews and wiring tests may temporarily bypass the master switch. A touch press briefly takes priority while pressed, then the normal alarm, message, story, music, bedtime, or daytime scene resumes.

The driver uses 200 Hz batched trailing-edge software PWM with 32 stable duty levels. A modest real-time priority is applied only to the LED timing thread, and missed low-brightness pulses are skipped rather than emitted late. Scene changes crossfade over 350 ms except alarms, which apply immediately. Repeated GPIO write failures disable lighting and appear in status and diagnostics.

The runtime priority after touch feedback is Alarm, Message, Stories, Music, Bedtime, then Daytime.

## Web interface

The GUI is organized by task.

### Everyday

- Home
- Alarms
- Messages
- Music
- Lighting
- Stories
- Day Images
- Bedtime Images

### Settings

- Display
- System

### Optional password

The web interface opens directly when no password exists. When a password is configured, the GUI asks for it before loading clock controls. The password can be set, changed, or removed from System. It is intentionally simple and stored as plain text for use only on a trusted home network.

### Always-visible confirmations

Save, test, delete, upload, and other action confirmations appear in a fixed floating notice. Feedback remains readable at any scroll position instead of appearing inside a section that may be off-screen.

### System diagnostics

The System page provides live diagnostics without launching external commands.

#### Network

- IP address
- hostname
- SSID
- active interface
- Wi-Fi signal percentage and dBm

#### Time

- system-time validity
- NTP synchronization state
- a prominent Home-page warning when time is not synchronized

#### Device identity

- short Inventory ID derived from the Raspberry Pi board serial
- full Raspberry Pi board serial
- Raspberry Pi board revision
- OS machine ID
- CPU signature for processor identification

Use the Inventory ID or Raspberry Pi serial to track a physical clock. The OS machine ID can change after reimaging, and the CPU signature is not unique.

#### Release and Raspberry Pi platform

- installed release version
- HTTP API version
- API compile date and time
- Raspberry Pi model
- operating system name
- OS version and codename
- kernel version
- CPU architecture
- system uptime
- CPU temperature

#### Storage and SD card

- used, available, and total system storage
- Day Images directory size and file count
- Bedtime Images directory size and file count
- music directory size and file count
- stories directory size and file count
- fonts directory size and file count
- system drive
- root partition and filesystem
- root read/write state
- boot partition, filesystem, and mount point
- SD-card device and type
- product name
- capacity
- manufacturer ID
- OEM ID
- serial number
- manufacture date
- CID

#### Service and clock health

- API health
- core connection
- OLED state
- touch-sensor state
- next alarm
- last successful alarm

**Download Diagnostic Report** saves the same information as a text file for support or troubleshooting.

### Backup and reset

The System page can:

- download a backup ZIP
- restore a backup created by mk-piclock
- factory reset settings and user-added assets

Backups include:

- alarms
- display and bedtime settings
- clock name and personalization
- scheduled-message data
- Day Images
- Bedtime Images
- uploaded fonts

Restore accepts only a validated mk-piclock backup ZIP. Paths, metadata, stored entries, sizes, and CRC values are checked before files are activated. Existing settings, images, and fonts are staged first and restored if the operation fails. Uploaded music and stories are not included in backups. Restore has no music or story handling, and both current libraries remain unchanged.

Restore is blocked while an alarm is active. Factory reset is blocked while an alarm is active or music is processing. Factory reset still removes uploaded music and stories. The protected fallback alarm remains installed after a factory reset.

### Help

- Recent Activity

Technical details remain available in collapsed sections so normal controls stay simple.

## Network access

The web interface opens directly when no password is configured. A simple optional password can be set or removed from System. It is stored as plain text on the clock and is intended only to keep casual users out of the controls.

Any device that can reach TCP port 8080 can view and change the clock. Keep the clock on a trusted home network.

Do not:

- forward port 8080 through the router
- expose the clock directly to the internet
- place it on an untrusted guest or public network

The API still runs as a restricted system account and communicates with the hardware process through a private Unix socket.

## Architecture

mk-piclock uses two native services.

### `mk-piclock-core`

Owns:

- OLED and SPI
- GPIO and touch input
- ALSA audio playback
- alarms
- clock and message rendering
- bedtime behaviour
- persistent clock configuration
- event logging

### `mk-piclock-api`

Owns:

- local web GUI
- HTTP API
- upload validation
- PNG conversion
- MP3 transcoding
- music, story, and image libraries
- OpenAPI document

The services communicate through:

```text
/run/mk-piclock/core.sock
```

The private binary protocol is IPC version 16.

## Command-free runtime

The running core and API do not use:

- `exec*()`
- `system()`
- `popen()`
- `posix_spawn()`
- `fork()` or `vfork()`

MP3 decoding, MP3 encoding, ZIP backup and restore, network inspection, platform reporting, and storage inspection run in-process through libraries and direct Linux interfaces.

The Makefile and installation steps use normal shell tools. This restriction applies to the installed runtime services.

## Time and NTP on Raspberry Pi OS Trixie

mk-piclock reads the Linux system clock. It does not contact an NTP server itself.

Raspberry Pi OS Lite based on Debian 13 Trixie normally uses `systemd-timesyncd` for network time synchronization. Set the timezone and confirm synchronization during installation:

```bash
sudo timedatectl set-timezone America/Edmonton
sudo timedatectl set-ntp true

timedatectl status
timedatectl show -p NTPSynchronized
```

Expected results include:

```text
System clock synchronized: yes
NTP service: active
NTPSynchronized=yes
```

Without a battery-backed real-time clock, correct time after a power loss depends on the Pi restoring time and reaching a time source.

## Supported platform

Recommended operating system:

```text
Raspberry Pi OS Lite
Debian 13 Trixie
```

Recommended hardware:

- Raspberry Pi Zero W or Zero 2 W
- SSD1322 256x64 OLED
- MAX98357A I2S amplifier
- 4-ohm, 3-watt speaker
- TTP223B touch sensor
- common-cathode RGB LED with one resistor per colour channel

See `pinouts.md` before applying power.

## Build dependencies

```bash
sudo apt update
sudo apt install --no-install-recommends -y \
  build-essential \
  pkg-config \
  libgpiod-dev \
  gpiod \
  libpng-dev \
  libfreetype-dev \
  libasound2-dev \
  libmpg123-dev \
  libmp3lame-dev \
  libmicrohttpd-dev \
  unzip
```

`libmp3lame-dev` provides the in-process MP3 encoder.

## Quick installation

After configuring SPI and the MAX98357A overlay as described in `INSTALL.md`:

```bash
make clean
make -j2
make install
sudo systemctl restart mk-piclock-core.service mk-piclock-api.service
```

Open:

```text
http://<clock-ip>:8080/
```

Do not run `sudo make install`. The Makefile invokes `sudo` only for privileged installation steps.

## Storage

The installer and services only create the storage and runtime directories used by the clock. No old `auth`, `faces`, `bedtime-faces`, or `bedtime_faces` directories are created.

```text
/opt/mk-piclock/assets/images
/opt/mk-piclock/assets/bedtime-images
/opt/mk-piclock/assets/music
/opt/mk-piclock/assets/music/.processing
/opt/mk-piclock/assets/stories
/opt/mk-piclock/assets/fonts
/opt/mk-piclock/assets/default-alarm.mp3
/opt/mk-piclock/config/clock.conf
/opt/mk-piclock/config/event.log
/run/mk-piclock/core.sock
```

## Service logs

```bash
sudo journalctl -b -u mk-piclock-core.service -n 100 --no-pager
sudo journalctl -b -u mk-piclock-api.service -n 100 --no-pager
```

Follow both services:

```bash
sudo journalctl -f \
  -u mk-piclock-core.service \
  -u mk-piclock-api.service
```

## Known limitations

- The MAX98357A and speaker may make a small pop when audio starts or stops.
- **Clear Queue** removes waiting MP3 jobs but does not cancel the active transcode.
- The clock has no built-in battery-backed real-time clock. Accurate time after startup depends on Linux time restoration or NTP. The GUI warns when the clock is not synchronized.
- Restore accepts backup ZIP files generated by mk-piclock. Other ZIP compression formats are rejected.
- The optional GUI password is stored as plain text and is not strong security. Keep the clock on a trusted home network and do not expose port 8080 to the internet.
- USB-C power behaviour depends on the power board used in the enclosure. A USB-A to USB-C cable is often the safest choice for simple 5 V boards that do not implement USB-C power negotiation correctly.
- The physical OLED colour is fixed by the panel. The GUI colour choice changes browser previews only.
- Some network or SD-card fields can show **Unavailable** when the Linux driver does not expose the value.
- The API service sandbox remains restricted. Network detection uses direct IPv4 ioctls instead of netlink discovery.

## Versions

```text
Product:     1.7.6
HTTP API:    1.25
Private IPC: 16
```

Install the core and API from the same release, then restart both services. Installation replaces the complete web tree, and every GUI asset uses `Cache-Control: no-store`, preventing retired or cached files from surviving an upgrade.

## Documentation

- Build and install: `INSTALL.md`
- Hardware wiring: `pinouts.md`
- Version history: `CHANGELOG.md`
- HTTP API: `ADDON_API.md`
- OpenAPI schema: `api/openapi-v1.json`
- Current release summary: `RELEASE_NOTES.md`
