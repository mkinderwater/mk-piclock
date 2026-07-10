# mk-piclock Kids

> A native C bedside alarm clock for Raspberry Pi, created for Rylie.

| Release | HTTP API | Private IPC |
|:--|:--|:--|
| `1.6.37` | `1.16` | `11` |

mk-piclock drives a **256x64 SSD1322 OLED**, **MAX98357A I2S amplifier**, **4-ohm speaker**, and **TTP223B touch sensor**.

It is designed to behave like a simple bedside appliance, not a general-purpose computer.

## Contents

- [Highlights](#highlights)
- [Features](#features)
- [Web interface](#web-interface)
- [Network access](#network-access)
- [Architecture](#architecture)
- [Supported platform](#supported-platform)
- [Time and NTP](#time-and-ntp)
- [Raspberry Pi boot configuration](#raspberry-pi-boot-configuration)
- [Build dependencies](#build-dependencies)
- [Installation](#installation)
- [Storage](#storage)
- [Service logs](#service-logs)
- [Known limitations](#known-limitations)
- [Documentation](#documentation)

---

## Highlights

| Area | Capability |
|:--|:--|
| Display | Large clock, full date, image rotation, status indicators, brightness schedules |
| Alarms | Seven alarm slots, weekday selection, random or selected music, volume ramp |
| Touch | Tap to stop audio, hold for three seconds to play random music |
| Bedtime | Separate schedule, artwork, and display brightness |
| Messages | Immediate, delayed, or scheduled OLED messages |
| Images | Separate Day and Bedtime libraries with PNG upload and ZIP export |
| Music | In-process MP3 validation, decoding, transcoding, metadata, and playback |
| Web GUI | Local browser control with an exact 256x64 framebuffer preview |
| Services | Separate native core and API processes using a private Unix socket |

---

## Features

### Clock and display

- Large 12-hour or 24-hour clock
- Full date below the time
- Blinking colon
- Separate Day and Bedtime image libraries
- Automatic image rotation
- Wi-Fi and alarm indicators
- Alarm volume indicator during playback
- Adjustable OLED brightness
- Separate bedtime schedule and brightness
- Yellow, green, or white browser preview panels
- Built-in and uploaded TrueType or OpenType fonts
- Exact 256x64 live framebuffer preview on the Home page

### Alarms

- Seven independent alarm slots
- Per-alarm time and weekday selection
- Selected or random uploaded music
- Separate starting and final volume
- Volume ramp across the song
- Short touch to stop active audio

### Kid-friendly touch control

| Action | Result |
|:--|:--|
| Short press | Stops the current alarm or song |
| Hold for three seconds | Plays a random song |

This provides simple bedside music control without exposing technical settings.

### Bedtime behaviour

Bedtime mode has its own:

- schedule
- image library
- display brightness

During bedtime, the clock automatically uses Bedtime Images and the configured bedtime brightness.

### Messages

Parents can send an OLED message:

- immediately
- after 10, 30, or 60 seconds
- at a browser-local date and time within the next 30 days

A message may contain:

- text only
- a selected Day Image
- a selected Bedtime Image
- a random Day Image
- a random Bedtime Image
- an image without text

> [!NOTE]
> One future message may be pending. Creating a new scheduled message replaces the existing one. Pending messages survive a core restart.

Message previews are rendered by `mk-piclock-core` using the same framebuffer, font measurements, wrapping, image placement, centring, grayscale, and brightness rules as the physical OLED.

### Images

Day Images and Bedtime Images each support:

- PNG upload
- OLED conversion
- paged library review
- individual deletion
- delete all
- download of all original PNG files as a ZIP

Converted `.raw` files are excluded from the ZIP download.

### Music

Uploaded MP3 files are decoded and re-encoded inside `mk-piclock-api`. No shell command or external encoder is launched.

#### Recommended encoding profile

| Setting | Value |
|:--|:--|
| Channels | Mono |
| Bitrate | 96 kbps CBR |
| Sample rate | 44.1 kHz |
| Low-pass filter | 16 kHz |

The Music page provides:

- upload and processing progress
- play, stop, and delete controls
- global volume
- optional encoding settings
- title, artist, album, year, track, and genre
- duration, bitrate, sample rate, channel, MPEG layer, and file size

Long `Title - Artist` text scrolls across the OLED. Short text remains centred.

#### Upload queue rule

Only one MP3 may be uploaded at a time. New uploads are refused while another song is queued or processing.

This prevents upload validation and transcoding from competing for CPU, memory, and storage.

> [!IMPORTANT]
> **Clear Queue** deletes waiting jobs and their temporary source files. It does not interrupt the file currently being transcoded.

---

## Web interface

The GUI is grouped by task.

| Group | Pages |
|:--|:--|
| Everyday | Home, Alarms, Messages, Music, Day Images, Bedtime Images |
| Settings | Display |
| Help | Recent Activity |

Technical details remain available in collapsed sections so common controls stay simple.

---

## Network access

The web interface opens without a password.

Any device that can reach TCP port `8080` can view and change the clock.

> [!WARNING]
> Keep mk-piclock on a trusted home network.

Do not:

- forward TCP port `8080` through the router
- expose the clock directly to the internet
- connect it to an untrusted guest or public network

The API runs under a restricted system account and communicates with the hardware process through a private Unix socket.

---

## Architecture

mk-piclock uses two native services.

### `mk-piclock-core`

Responsible for:

- OLED and SPI
- GPIO and touch input
- ALSA audio playback
- alarms
- clock and message rendering
- bedtime behaviour
- persistent clock configuration
- event logging

### `mk-piclock-api`

Responsible for:

- local web GUI
- HTTP API
- upload validation
- PNG conversion
- MP3 transcoding
- music and image libraries
- OpenAPI document

### Internal communication

```text
/run/mk-piclock/core.sock
```

The services use private binary protocol **IPC version 11**.

---

## Supported platform

### Operating system

```text
Raspberry Pi OS Lite
Debian 13 Trixie
```

### Recommended hardware

| Component | Recommended device |
|:--|:--|
| Raspberry Pi | Raspberry Pi Zero 2 W or Raspberry Pi 5 |
| Display | SSD1322 256x64 OLED |
| Amplifier | MAX98357A I2S |
| Speaker | 4-ohm, 3-watt |
| Touch sensor | TTP223B |

> [!CAUTION]
> Review `pinouts.md` before applying power.

---

## Time and NTP

mk-piclock reads the Linux system clock. It does not contact an NTP server itself.

Raspberry Pi OS Lite based on Debian 13 Trixie normally uses `systemd-timesyncd`.

Set the timezone and enable synchronization:

```bash
sudo timedatectl set-timezone America/Edmonton
sudo timedatectl set-ntp true
```

Confirm status:

```bash
timedatectl status
timedatectl show -p NTPSynchronized
```

Expected results include:

```text
System clock synchronized: yes
NTP service: active
NTPSynchronized=yes
```

> [!NOTE]
> Without a battery-backed real-time clock, correct time after a power loss depends on Linux restoring time and reaching a network time source.

---

## Raspberry Pi boot configuration

Raspberry Pi OS Lite based on Debian 13 Trixie stores boot configuration in:

```text
/boot/firmware/config.txt
```

### 1. Back up the existing file

```bash
sudo cp /boot/firmware/config.txt \
  /boot/firmware/config.txt.mk-piclock-backup
```

### 2. Edit the configuration

```bash
sudo nano /boot/firmware/config.txt
```

Add the following beneath the existing `[all]` section:

```ini
# mk-piclock hardware configuration

# Enable SPI0 for the SSD1322 OLED.
dtparam=spi=on

# Disable the Pi's built-in audio device.
dtparam=audio=off

# Enable the MAX98357A I2S amplifier.
dtoverlay=max98357a

# Minimize memory reserved for legacy GPU functions.
gpu_mem=16
```

> [!IMPORTANT]
> Do not add a second `[all]` heading if one already exists.

No changes to `/boot/firmware/cmdline.txt` are required.

### 3. Reboot

```bash
sudo reboot
```

### 4. Verify SPI

```bash
ls -l /dev/spidev0.0
```

### 5. Verify audio

```bash
aplay -l
```

The detected audio device should include `MAX98357A` or `bcm2835-i2s`.

### 6. Review the active settings

```bash
grep -E \
  '^(dtparam=spi|dtparam=audio|dtoverlay=max98357a|gpu_mem=)' \
  /boot/firmware/config.txt
```

Older Raspberry Pi OS releases used `/boot/config.txt`. The supported Debian 13 Trixie release uses `/boot/firmware/config.txt`.

---

## Build dependencies

Install the required packages:

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

---

## Installation

Apply the boot settings, reboot, then build and install mk-piclock.

### Raspberry Pi Zero 2 W

```bash
make clean
make -j2
make install
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

### Raspberry Pi 5

```bash
make clean
make -j4
make install
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

> [!IMPORTANT]
> Do not run `sudo make install`. The Makefile invokes `sudo` only for privileged installation steps.

### Open the web interface

```text
http://<clock-ip>:8080/
```

---

## Storage

| Purpose | Path |
|:--|:--|
| Day Images | `/opt/mk-piclock/assets/images` |
| Bedtime Images | `/opt/mk-piclock/assets/bedtime-images` |
| Music | `/opt/mk-piclock/assets/music` |
| Music processing | `/opt/mk-piclock/assets/music/.processing` |
| Fonts | `/opt/mk-piclock/assets/fonts` |
| Clock configuration | `/opt/mk-piclock/config/clock.conf` |
| Event log | `/opt/mk-piclock/config/event.log` |

---

## Service logs

### Recent core log

```bash
sudo journalctl -b \
  -u mk-piclock-core.service \
  -n 100 \
  --no-pager
```

### Recent API log

```bash
sudo journalctl -b \
  -u mk-piclock-api.service \
  -n 100 \
  --no-pager
```

### Follow both services

```bash
sudo journalctl -f \
  -u mk-piclock-core.service \
  -u mk-piclock-api.service
```

---

## Known limitations

- The MAX98357A and speaker may make a small pop when audio starts or stops.
- **Clear Queue** removes waiting MP3 jobs but does not cancel the active transcode.
- The clock has no battery-backed real-time clock.
- Accurate startup time depends on Linux time restoration or NTP.
- The GUI has no password. Network isolation is the access control.
- USB-C power behaviour depends on the power board used in the enclosure.
- A USB-A to USB-C cable is often safest for basic 5 V boards without proper USB-C power negotiation.
- The physical OLED colour is fixed by the panel.
- The GUI colour setting changes browser previews only.

---

## Version compatibility

```text
Product:     1.6.37
HTTP API:    1.16
Private IPC: 11
```

Install the core and API from the same release. Restart both services, then hard-refresh the browser.

---

## Documentation

| Document | Purpose |
|:--|:--|
| `INSTALL.md` | Build and installation |
| `pinouts.md` | Hardware wiring |
| `CHANGELOG.md` | Version history |
| `ADDON_API.md` | HTTP API |
| `api/openapi-v1.json` | OpenAPI schema |
| `RELEASE_NOTES.md` | Current release summary |
