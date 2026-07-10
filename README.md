# mk-piclock Kids

> A native C bedside alarm clock for Raspberry Pi, created for Rylie.

| Product | Release | HTTP API | Private IPC |
|:--|:--|:--|:--|
| mk-piclock Kids | `1.7.1` | `1.16` | `11` |

mk-piclock Kids drives a **256x64 SSD1322 OLED**, **MAX98357A I2S amplifier**, **4-ohm speaker**, **TTP223B touch sensor**, and optional **RGB LED module**.

It is designed to behave like a simple bedside appliance, not a general-purpose computer.

## Contents

- [Highlights](#highlights)
- [Clock and display](#clock-and-display)
- [Alarms](#alarms)
- [Touch controls](#touch-controls)
- [Story Mode](#story-mode)
- [Music](#music)
- [Bedtime mode](#bedtime-mode)
- [Messages](#messages)
- [Images](#images)
- [Lighting](#lighting)
- [Web interface](#web-interface)
- [Diagnostics and maintenance](#diagnostics-and-maintenance)
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
- [Version compatibility](#version-compatibility)
- [Documentation](#documentation)

---

## Highlights

| Area | Capability |
|:--|:--|
| Display | Large clock, full date, images, fonts, status indicators, and scheduled brightness |
| Alarms | Seven alarm slots, weekday schedules, random or selected music, looping, and volume ramp |
| Touch | Tap to stop, hold to play music, rapid taps to enter Story Mode |
| Story Mode | Random bedtime story playback with its own library, volume, splash, and metadata display |
| Bedtime | Separate schedule, image library, brightness, and music restrictions |
| Messages | Immediate, delayed, or scheduled messages with optional artwork |
| Images | Separate Day and Bedtime libraries with upload, conversion, deletion, and ZIP export |
| Music | In-process MP3 validation, transcoding, metadata, playback, and queue management |
| Lighting | Optional RGB accent and status lighting |
| Diagnostics | Network, time, temperature, storage, service, alarm, and device identity information |
| Maintenance | Event history, backup, restore, and factory reset |
| Web GUI | Local browser control with an exact 256x64 framebuffer preview |
| Services | Separate native core and API services using a private Unix socket |

---

## Clock and display

The OLED clock supports:

- 12-hour or 24-hour time
- large centred time
- blinking colon
- stable spacing for single-digit hours
- full date below the time
- ordinal date formatting
- automatic Day and Bedtime image rotation
- Wi-Fi status indicator
- alarm-enabled indicator
- alarm volume indicator during playback
- next-alarm information
- adjustable day brightness
- separate bedtime brightness
- scheduled bedtime transitions
- built-in fonts
- uploaded TrueType and OpenType fonts
- adjustable clock placement
- yellow, green, or white browser preview panels
- exact 256x64 live framebuffer preview on the Home page

The browser preview is generated from the same framebuffer used by the OLED. Font size, positioning, wrapping, grayscale, and brightness match the physical display.

---

## Alarms

mk-piclock Kids provides seven independent alarm slots.

Each alarm supports:

- enabled or disabled state
- alarm time
- weekday selection
- selected MP3
- random MP3
- starting volume
- final volume
- gradual volume ramp
- repeat playback until stopped
- touch cancellation
- browser stop control
- fallback alarm audio if the selected file cannot play
- maximum alarm duration of 30 minutes

The Home page shows the next active alarm.

The system records the most recent successful alarm event for diagnostics.

> [!IMPORTANT]
> The alarm continues until stopped or until the maximum alarm duration is reached.

---

## Touch controls

The TTP223B sensor provides simple child-friendly control.

| Touch action | Result |
|:--|:--|
| Short press | Stops the current alarm, song, or story |
| Hold for three seconds | Plays a random song |
| Ten rapid taps | Starts Story Mode |

Touch input is ignored briefly during Story Mode startup so the activation taps do not immediately stop the story.

The touch controls work without opening the web interface.

---

## Story Mode

Story Mode provides a separate bedtime listening experience.

It supports:

- enable or disable control
- activation with ten rapid taps
- separate Stories library
- random story selection
- separate story volume
- configurable startup message
- full-screen startup splash
- bedtime image grid during startup
- story title and artist display
- scrolling metadata when text is too long
- automatic return to the normal clock display
- touch to stop playback
- hidden Wi-Fi and alarm pills during the Story Mode display

Story audio is stored separately from normal music so songs and bedtime stories remain independently managed.

Typical flow:

1. The child taps the sensor ten times.
2. The OLED clears and shows the Story Mode splash.
3. A random story begins.
4. The OLED shows its title and artist.
5. Long metadata scrolls across the display.
6. The clock display returns while the story continues.
7. A short touch stops playback.

---

## Music

Uploaded MP3 files are validated, decoded, and re-encoded inside `mk-piclock-api`.

No shell command or external encoder is launched.

### Recommended encoding profile

| Setting | Value |
|:--|:--|
| Channels | Mono |
| Bitrate | 96 kbps CBR |
| Sample rate | 44.1 kHz |
| Low-pass filter | 16 kHz |

### Music library features

The Music page provides:

- MP3 upload
- upload progress
- processing progress
- play control
- stop control
- delete control
- global music volume
- optional encoding settings
- title display
- artist display
- album display
- year display
- track display
- genre display
- duration
- bitrate
- sample rate
- channel count
- MPEG layer
- file size
- current playback state

Long `Title - Artist` text scrolls across the OLED. Short text remains centred.

### Upload queue

Only one MP3 may be uploaded at a time.

A new upload is refused while another file is queued or processing. This prevents validation and transcoding from competing for CPU, memory, and storage on the Raspberry Pi.

**Clear Queue** removes:

- waiting jobs
- queued source files
- temporary source files for waiting jobs

It does not interrupt the file currently being transcoded.

---

## Bedtime mode

Bedtime mode has its own:

- start time
- end time
- image library
- display brightness
- artwork rotation
- music permission

During bedtime, the clock automatically uses Bedtime Images and the configured bedtime brightness.

Parents may disable normal music playback during bedtime while leaving alarms and Story Mode available.

This lets the clock remain useful at night without keeping the OLED unnecessarily bright or allowing unrestricted music.

---

## Messages

Parents can send a message to the OLED:

- immediately
- after 10 seconds
- after 30 seconds
- after 60 seconds
- at a browser-local date and time within the next 30 days

A message can use:

- text only
- a selected Day Image
- a selected Bedtime Image
- a random Day Image
- a random Bedtime Image
- an image without text

Message rendering supports:

- automatic word wrapping
- centred text
- live font measurement
- image placement
- OLED grayscale conversion
- configured display brightness
- exact framebuffer preview

> [!NOTE]
> One future message may be pending. A new scheduled message replaces the existing one. Pending messages survive a core restart.

The preview is rendered by `mk-piclock-core`, using the same rules as the physical OLED.

---

## Images

Day Images and Bedtime Images are stored and managed separately.

Each library supports:

- PNG upload
- validation
- OLED conversion
- paged library review
- lazy loading
- individual deletion
- delete all
- download of all original PNG files as a ZIP

Converted `.raw` files are excluded from ZIP downloads.

The clock can select:

- a specific image
- a random image
- automatically rotated images
- different images for day and bedtime

---

## Lighting

The optional RGB LED module provides accent and status lighting.

Lighting features include:

- enable or disable control
- browser control
- configurable colour
- adjustable brightness
- persistent settings
- coordination with clock operation
- low-light use with the bedtime display

Lighting is handled independently from the OLED panel colour. The OLED colour is fixed by its physical panel.

See `pinouts.md` for the confirmed GPIO assignments.

---

## Web interface

The local web interface is organized by task.

| Group | Pages |
|:--|:--|
| Everyday | Home, Alarms, Messages, Music, Day Images, Bedtime Images |
| Listening | Music, Stories |
| Settings | Display, Lighting, Story Mode, System |
| Help | Recent Activity, Diagnostics |

The interface includes:

- live clock preview
- current playback state
- next alarm
- Wi-Fi status
- alarm controls
- music controls
- story controls
- image management
- font management
- lighting controls
- recent activity
- diagnostics
- system information
- maintenance actions

Technical details remain in collapsed sections so normal controls stay simple.

---

## Diagnostics and maintenance

### Diagnostics

The Diagnostics page reports:

- IP address
- hostname
- connected SSID
- Wi-Fi signal strength
- network state
- NTP synchronization
- CPU temperature
- free storage
- used storage
- core service health
- API service health
- last successful alarm
- current playback state

### Device identity

The System page reports:

- product version
- HTTP API version
- private IPC version
- compile time
- hostname
- Raspberry Pi serial-derived device identity
- `MK-<serial>` device identifier
- storage usage
- image library size
- bedtime image library size
- music library size
- story library size

### Recent Activity

The event log records important actions, including:

- service startup
- alarm events
- playback requests
- stop requests
- message activity
- upload activity
- configuration changes
- errors

### Backup and restore

Maintenance tools support:

- configuration backup
- image backup
- bedtime image backup
- font backup
- restore
- factory reset

Music and story audio are excluded from the simplified backup to keep archive size manageable.

> [!CAUTION]
> Factory reset removes user configuration and uploaded assets covered by the reset operation. Download required files first.

---

## Network access

The web interface opens without a password.

Any device that can reach TCP port `8080` can view and change the clock.

> [!WARNING]
> Keep mk-piclock Kids on a trusted home network.

Do not:

- forward TCP port `8080` through the router
- expose the clock directly to the internet
- connect it to an untrusted guest network
- connect it to a public network

The API runs under a restricted system account and communicates with the hardware process through a private Unix socket.

---

## Architecture

mk-piclock Kids uses two native services.

### `mk-piclock-core`

Responsible for:

- OLED framebuffer
- SSD1322 SPI communication
- GPIO access
- touch input
- optional RGB LED control
- ALSA audio playback
- alarms
- music playback
- story playback
- clock rendering
- message rendering
- bedtime behaviour
- persistent clock configuration
- event logging
- private IPC server

### `mk-piclock-api`

Responsible for:

- local web GUI
- HTTP API
- upload validation
- PNG conversion
- font upload
- MP3 metadata extraction
- MP3 transcoding
- upload queue management
- music library
- story library
- image libraries
- diagnostics presentation
- maintenance actions
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

### Supported Raspberry Pi models

- Raspberry Pi Zero
- Raspberry Pi Zero 2 W

### Recommended hardware

| Component | Recommended device |
|:--|:--|
| Raspberry Pi | Raspberry Pi Zero 2 W |
| Display | SSD1322 256x64 OLED |
| Amplifier | MAX98357A I2S |
| Speaker | 4-ohm, 3-watt |
| Touch sensor | TTP223B |
| Lighting | Optional RGB LED module |
| Storage | Reliable microSD card |
| Power | Stable 5 V, 2 A supply |

> [!CAUTION]
> Review `pinouts.md` before applying power.

---

## Time and NTP

mk-piclock Kids reads the Linux system clock. It does not contact an NTP server itself.

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

The web interface warns when Linux has not confirmed time synchronization.

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
  unzip
```

Package roles:

| Package | Purpose |
|:--|:--|
| `build-essential` | Compiler, linker, and standard build tools |
| `pkg-config` | Library discovery during compilation |
| `libgpiod-dev` | GPIO access |
| `gpiod` | GPIO diagnostic tools |
| `libpng-dev` | PNG decoding and conversion |
| `libfreetype-dev` | TrueType and OpenType rendering |
| `libasound2-dev` | ALSA audio output |
| `libmpg123-dev` | MP3 decoding |
| `libmp3lame-dev` | In-process MP3 encoding |
| `libmicrohttpd-dev` | Embedded HTTP server |
| `alsa-utils` | ALSA testing and diagnostics |
| `unzip` | Archive handling |

---

## Installation

Apply the boot settings and reboot before building.

### Raspberry Pi Zero

```bash
make clean
make -j1
make install
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

### Raspberry Pi Zero 2 W

```bash
make clean
make -j2
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

After installing a new release:

1. Restart both services.
2. Hard-refresh the browser.
3. Confirm that the Product, HTTP API, and Private IPC versions match.

---

## Storage

| Purpose | Path |
|:--|:--|
| Day Images | `/opt/mk-piclock/assets/images` |
| Bedtime Images | `/opt/mk-piclock/assets/bedtime-images` |
| Music | `/opt/mk-piclock/assets/music` |
| Stories | `/opt/mk-piclock/assets/stories` |
| Music processing | `/opt/mk-piclock/assets/music/.processing` |
| Story processing | `/opt/mk-piclock/assets/stories/.processing` |
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

### Service status

```bash
systemctl status \
  mk-piclock-core.service \
  mk-piclock-api.service
```

---

## Known limitations

- The MAX98357A and speaker may make a small pop when audio starts or stops.
- **Clear Queue** removes waiting jobs but does not cancel the active transcode.
- The clock has no battery-backed real-time clock.
- Accurate startup time depends on Linux time restoration or NTP.
- The GUI has no password. Network isolation is the access control.
- USB-C power behaviour depends on the power board used in the enclosure.
- A USB-A to USB-C cable is often safest for basic 5 V boards without correct USB-C power negotiation.
- The physical OLED colour is fixed by the panel.
- The GUI panel colour changes browser previews only.
- Backup archives exclude music and story audio.
- Raspberry Pi Zero transcoding is slower than Raspberry Pi Zero 2 W transcoding.
- Only one MP3 upload may be queued or processed at a time.

---

## Version compatibility

```text
Product:     1.7.1
HTTP API:    1.16
Private IPC: 11
```

Install `mk-piclock-core` and `mk-piclock-api` from the same release.

A mismatched GUI or API may refuse to open clock controls until the correct binaries are installed and the browser is hard-refreshed.

---

## Documentation

| Document | Purpose |
|:--|:--|
| `INSTALL.md` | Build and installation |
| `pinouts.md` | Hardware wiring and GPIO assignments |
| `CHANGELOG.md` | Version history |
| `ADDON_API.md` | HTTP API |
| `api/openapi-v1.json` | OpenAPI schema |
| `RELEASE_NOTES.md` | Current release summary |
