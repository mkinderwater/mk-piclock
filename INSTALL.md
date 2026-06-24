# mk-piclock v1.6.37

mk-piclock is a native C alarm clock for Raspberry Pi. It drives a 256x64 SSD1322 OLED, MAX98357A I2S amplifier, speaker, and TTP223B touch sensor.

The project was created for my daughter Rylie. It is designed to behave like a simple bedside appliance, not a general-purpose computer.

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
- A short touch stops active audio

### Kid-friendly touch control

- Short press: stop the current alarm or song
- Hold for three seconds: play a random song

This gives the child a simple bedside music control without exposing technical settings.

### Bedtime behaviour

Bedtime mode uses its own schedule, image library, and brightness. It lets the clock remain useful at night without keeping the OLED unnecessarily bright.

Day Images and Bedtime Images are stored separately. During bedtime, the clock uses bedtime artwork and the configured bedtime brightness.

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

One future message can be pending. A new scheduled message replaces the existing one. Pending messages survive a core restart.

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

- upload and processing progress
- play, stop, and delete controls
- global volume
- optional encoding settings
- title, artist, album, year, track, genre, duration, bitrate, sample rate, channel, MPEG layer, and file size when available

Long `Title - Artist` text scrolls across the OLED. Short text remains centred.

#### Upload queue rule

Only one MP3 may be uploaded at a time. A new upload is refused while another song is queued or processing.

This prevents upload validation and transcoding from competing for CPU, memory, and storage on the Raspberry Pi.

**Clear Queue** deletes waiting jobs and their temporary source files. It does not interrupt the file currently transcoding.

## Web interface

The GUI is organized by task.

### Everyday

- Home
- Alarms
- Messages
- Music
- Day Images
- Bedtime Images

### Settings

- Display

### Help

- Recent Activity

Technical details remain available in collapsed sections so normal controls stay simple.

## Network access

The web interface opens directly without a password.

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
- music and image libraries
- OpenAPI document

The services communicate through:

```text
/run/mk-piclock/core.sock
```

The private binary protocol is IPC version 11.

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

- Raspberry Pi Zero 2 W
- Raspberry Pi 5
- SSD1322 256x64 OLED
- MAX98357A I2S amplifier
- 4-ohm, 3-watt speaker
- TTP223B touch sensor

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
  alsa-utils \
  unzip \
  curl
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

```text
/opt/mk-piclock/assets/images
/opt/mk-piclock/assets/bedtime-images
/opt/mk-piclock/assets/music
/opt/mk-piclock/assets/music/.processing
/opt/mk-piclock/assets/fonts
/opt/mk-piclock/config/clock.conf
/opt/mk-piclock/config/event.log
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
- The clock has no built-in battery-backed real-time clock. Accurate time after startup depends on Linux time restoration or NTP.
- The GUI has no password. Network isolation is the access control.
- USB-C power behaviour depends on the power board used in the enclosure. A USB-A to USB-C cable is often the safest choice for simple 5 V boards that do not implement USB-C power negotiation correctly.
- The physical OLED colour is fixed by the panel. The GUI colour choice changes browser previews only.

## Versions

```text
Product:     1.6.37
HTTP API:    1.16
Private IPC: 11
```

Install the core and API from the same release, then restart both services and hard-refresh the browser.

## Documentation

- Build and install: `INSTALL.md`
- Hardware wiring: `pinouts.md`
- Version history: `CHANGELOG.md`
- HTTP API: `ADDON_API.md`
- OpenAPI schema: `api/openapi-v1.json`
- Current release summary: `RELEASE_NOTES.md`
