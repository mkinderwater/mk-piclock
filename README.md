# mk-piclock Kids

> A bedside clock built by a dad, for kids.

| Product | Release | HTTP API | Private IPC |
|:--|:--|:--|:--|
| mk-piclock Kids | `1.7.1` | `1.16` | `11` |

mk-piclock Kids is a Raspberry Pi alarm clock designed to be simple for a child and useful for a parent.

For the child, it is a clock, music player, story player, night light, and alarm.

For the parent, it provides a private web page for setting alarms, uploading music and stories, controlling bedtime behaviour, sending messages, and checking that everything is working.

The goal is not to put another screen, tablet, or voice assistant in a child's bedroom. The goal is to provide the useful parts of those devices without advertising, accounts, microphones, notifications, or unrestricted internet access.

---

## Why this clock exists

Most children's clocks are either too basic or far too connected.

mk-piclock Kids was created to provide:

- a clear clock that can be read at night
- alarms that use familiar music instead of harsh beeping
- a simple touch control a child can understand
- bedtime stories without handing over a phone or tablet
- parent control from another device on the home network
- no cloud account
- no microphone
- no advertisements
- no subscription
- no internet-facing control panel

The clock is intended to feel like an appliance. Once it is set up, a child should not need to understand Raspberry Pi, Linux, Wi-Fi, or the web interface.

---

## What the child can do

The child uses one touch sensor on the clock.

| Touch action | What happens |
|:--|:--|
| Short press | Stops the current alarm, song, or story |
| Hold for three seconds | Plays a random song |
| Ten rapid taps | Starts Story Mode |

The RGB LED briefly flashes when a touch is recognized. This tells the child that the clock felt the press, even before the requested action begins.

No menus, passwords, or screen navigation are required at the clock itself.

---

## What the parent can do

From a phone, tablet, or computer on the same home network, a parent can:

- set up to seven alarms
- choose which days each alarm runs
- choose a specific alarm song or let the clock select one randomly
- control alarm volume and volume ramping
- upload several songs or stories in one batch
- set separate music and story volumes
- set bedtime hours
- reduce display brightness at bedtime
- disable normal music during bedtime
- upload Day Images and Bedtime Images
- send an immediate or scheduled message to the OLED
- choose the clock font and display position
- control the RGB night light
- review recent activity
- check Wi-Fi, storage, time synchronization, and service health
- back up settings and artwork
- restore a backup
- perform a factory reset

The web interface is intentionally organized around normal parent tasks. Technical details remain available, but they are not placed in the way of everyday controls.

---

## Everyday behaviour

### Clock display

The 256x64 OLED can show:

- 12-hour or 24-hour time
- a large centred clock
- a blinking colon
- a full written date
- Day Images
- Bedtime Images
- the next alarm
- Wi-Fi status
- alarm-enabled status
- alarm volume while an alarm is playing
- music or story title and artist
- parent messages

The display uses separate daytime and bedtime brightness settings.

The browser preview is generated from the same framebuffer used by the physical OLED. This lets a parent see what will actually appear on the clock before saving a change or sending a message.

### Alarms

The clock provides seven independent alarm slots.

Each alarm can have its own:

- time
- weekdays
- enabled or disabled state
- selected MP3
- random MP3 option
- starting volume
- final volume
- gradual volume ramp

An alarm repeats until it is stopped or reaches the 30-minute safety limit.

A short touch stops the alarm immediately.

If the selected alarm file cannot be played, the clock uses fallback alarm audio rather than failing silently.

The Home page shows the next active alarm, and the clock records its most recent successful alarm for diagnostics.

### Why alarms repeat

Children do not always wake up to the first few seconds of a song. Repeating the alarm prevents a short file or momentary distraction from causing the alarm to end before the child is awake.

The 30-minute limit prevents an unattended alarm from playing forever.

---

## Story Mode

Story Mode gives the child access to bedtime stories without a phone, tablet, smart speaker, or streaming service.

To start Story Mode, the child taps the sensor ten times.

The clock then:

1. clears the normal clock display
2. shows the Story Mode splash
3. displays a grid of Bedtime Images
4. selects a random story
5. begins playback at the configured story volume
6. shows the story title and artist
7. scrolls long text when necessary
8. returns to the normal clock while the story continues

A short touch stops the story.

Story Mode has:

- its own enable or disable setting
- its own Stories library
- its own volume
- a configurable startup message
- random story selection
- a startup splash
- title and artist display
- scrolling metadata
- touch-to-stop behaviour

The Wi-Fi and alarm status pills are hidden during the Story Mode display so they do not interfere with the splash or story information.

Touch input is briefly ignored during startup so the ten activation taps do not immediately stop the story.

### Why stories are separate from music

Songs and stories are stored in separate libraries so they can have different volumes, controls, and bedtime rules.

A parent can maintain a music collection for daytime use and a calmer story collection for bedtime.

---

## Music

The child can hold the touch sensor for three seconds to play a random song.

A short touch stops playback.

The parent can also play, stop, upload, and delete music from the web interface.

### Music library information

The Music page can show:

- title
- artist
- album
- year
- track
- genre
- duration
- bitrate
- sample rate
- channel count
- MPEG layer
- file size
- current playback state

Long `Title - Artist` text scrolls across the OLED. Short text remains centred.

### Uploading MP3 files

> [!IMPORTANT]
> Multiple MP3 files can be selected and uploaded together. The upload queue may contain several files. The Raspberry Pi processes and transcodes them sequentially, one file at a time.

Several MP3 files may be selected and uploaded in one batch.

Each file is checked separately.

| Limit | Value |
|:--|:--|
| Maximum source size | 64 MiB per MP3 |
| Exact limit | 67,108,864 bytes |
| Limit applies to | Each original MP3 |
| Files per selection | Multiple |
| Files transcoded at once | One |

The 64 MiB limit applies to each original file before processing. It is not a limit on the combined batch.

For example, a batch containing four 20 MiB files is acceptable because each file is below the limit.

A single 70 MiB file is rejected because that individual file exceeds the limit.

### Why processing happens one file at a time

Multiple files can be uploaded together, but only one file is decoded and transcoded at a time.

The Raspberry Pi Zero and Zero 2 W have limited CPU, memory, and storage speed. Sequential processing keeps the clock responsive and reduces the chance of failed uploads, audio interruptions, or a full microSD card.

The processing page shows:

- the active file
- waiting files
- completed files
- failed files

### Upload validation

An MP3 may be rejected when:

- it exceeds 64 MiB
- it cannot be decoded as an MP3
- its filename is unsafe or invalid
- the music storage quota would be exceeded
- the required free-space reserve would be crossed
- temporary processing storage is unavailable

The default storage policy preserves at least 64 MiB of free space. This prevents an upload or transcode from consuming the last available space on the microSD card.

### Processing steps

Each accepted MP3 is:

1. validated
2. tested with `libmpg123`
3. copied into the processing area
4. added as a separate job
5. decoded
6. re-encoded with `libmp3lame`
7. moved into the Music library

No shell command or external MP3 encoder is launched.

### Recommended MP3 profile

| Setting | Value |
|:--|:--|
| Channels | Mono |
| Bitrate | 96 kbps CBR |
| Sample rate | 44.1 kHz |
| Low-pass filter | 16 kHz |

This profile provides clear speech and music while keeping storage and decoding requirements reasonable.

### Clear Queue

**Clear Queue** removes:

- jobs that are still waiting
- queued source files
- temporary files belonging to waiting jobs

It does not interrupt the file currently being transcoded.

---

## Bedtime mode

Bedtime mode automatically changes how the clock behaves at night.

It has its own:

- start time
- end time
- display brightness
- image library
- image rotation
- normal-music permission

During bedtime, the clock uses Bedtime Images and the configured bedtime brightness.

A parent may disable normal music during bedtime while leaving alarms and Story Mode available.

### Why bedtime mode exists

A bright or entertaining clock can encourage a child to keep watching it.

Bedtime mode lets the display remain useful without being unnecessarily bright. It can also prevent random daytime music while still allowing a calm bedtime story.

---

## Parent messages

A parent can send a message to the OLED:

- immediately
- after 10 seconds
- after 30 seconds
- after 60 seconds
- at a selected date and time within the next 30 days

A message may use:

- text only
- a selected Day Image
- a selected Bedtime Image
- a random Day Image
- a random Bedtime Image
- an image without text

Examples include:

- `Time for school`
- `Please brush your teeth`
- `Dinner is ready`
- `Good luck today`
- `Love you`

Message rendering supports:

- automatic word wrapping
- centred text
- live font measurements
- image placement
- OLED grayscale conversion
- configured brightness
- exact framebuffer preview

Only one future message may be pending. Creating a new scheduled message replaces the existing one.

Scheduled messages survive a core service restart.

### Why messages are local

The message feature gives a parent a simple way to communicate without giving the child a phone or adding a cloud messaging service to the bedroom.

---

## Day Images and Bedtime Images

The clock keeps two separate artwork libraries.

### Day Images

These can be brighter, more active, or more playful.

### Bedtime Images

These can be calmer and less distracting.

Each library supports:

- PNG upload
- image validation
- OLED conversion
- paged review
- lazy loading
- individual deletion
- delete all
- download of all original PNG files as a ZIP

Converted `.raw` files are not included in ZIP downloads.

The clock can use:

- a selected image
- a random image
- automatic image rotation
- different image sets during day and bedtime

---

## RGB lighting

The optional RGB LED acts as both a small night light and a touch-feedback indicator.

### Normal light behaviour

The Lighting page provides:

- master on or off control
- selectable RGB colour
- global brightness
- red-channel calibration
- green-channel calibration
- blue-channel calibration
- separate touch-feedback colour
- saved settings that survive restarts

When the light is enabled, it displays the selected colour at the selected brightness.

When the light is disabled, normal ambient output is turned off.

### Touch feedback

Every recognized touch briefly flashes the RGB LED using the configured touch colour.

This includes:

- a short press
- the beginning of a three-second hold
- each tap being counted toward Story Mode

The flash confirms that the touch sensor detected the child. It does not depend on whether music or an alarm is currently playing.

### Why channel calibration exists

Red, green, and blue elements often have different apparent brightness.

Channel calibration lets the parent reduce a colour that appears too strong or increase one that appears too weak. This produces more balanced mixed colours and a softer night light.

### PWM behaviour

| Property | Behaviour |
|:--|:--|
| PWM method | Batched trailing-edge software PWM |
| PWM frequency | 200 Hz |
| Stable duty levels | 32 |
| Expected LED type | Common-cathode RGB |
| Common-anode support | Not supported |

The global brightness setting scales the selected colour without changing its intended RGB balance.

### Wiring tests

The Lighting page contains direct tests for:

- red
- green
- blue
- white

These tests temporarily bypass:

- the master light switch
- the normal brightness limit
- channel calibration

They are intended for setup and troubleshooting. They help identify:

- swapped colour leads
- a failed LED channel
- an incorrect common connection
- a missing resistor
- an incorrectly identified LED

### RGB LED wiring

| LED connection | BCM GPIO | Physical pin |
|:--|--:|--:|
| Red anode through resistor | 5 | 29 |
| Green anode through resistor | 6 | 31 |
| Blue anode through resistor | 13 | 33 |
| Common cathode | Ground | 30 |

Use one **220 to 330 ohm current-limiting resistor** on each colour channel.

> [!WARNING]
> This release expects a common-cathode RGB LED. Common-anode RGB LEDs are not supported.

Four-lead RGB LEDs do not use a universal lead order. Check the LED datasheet and use the built-in channel tests before final assembly.

The RGB light is independent from the OLED colour. The OLED colour is fixed by the physical display panel. The GUI panel-colour setting changes browser previews only.

---

## Powering the clock

Use a stable **5 V, 2 A USB power adapter**.

### Required cable type

Use:

```text
USB-A power adapter -> USB-A to USB-C cable -> mk-piclock
```

Do not rely on:

```text
USB-C power adapter -> USB-C to USB-C cable -> mk-piclock
```

> [!IMPORTANT]
> Power the clock with a USB-A to USB-C cable.

### Why USB-A to USB-C is required

The USB-C connector used by the clock is a convenient physical power connector, but the clock does not implement USB-C current detection or power negotiation on the configuration-channel pins.

A USB-C power supply connected with a USB-C to USB-C cable may wait for a valid USB-C connection before providing power. Because the clock does not present that signalling, some USB-C supplies may provide no power at all.

A USB-A power source does not require USB-C negotiation. It supplies normal 5 V power through the USB-A to USB-C cable, which is what the clock expects.

This is not a fast-charging device. It does not request USB Power Delivery voltages and must only receive normal 5 V power.

### Power summary

| Item | Requirement |
|:--|:--|
| Voltage | 5 V DC |
| Recommended capacity | 2 A |
| Power adapter connector | USB-A |
| Clock connector | USB-C |
| Cable | USB-A to USB-C |
| USB-C to USB-C | Not recommended and may not power the clock |
| USB Power Delivery | Not used |
| Fast charging | Not required |

---

## Web interface

Open the clock from another device on the same network:

```text
http://<clock-ip>:8080/
```

The web interface is grouped around normal parent tasks.

| Group | Pages |
|:--|:--|
| Everyday | Home, Alarms, Messages, Music, Day Images, Bedtime Images |
| Listening | Music, Stories |
| Settings | Display, Lighting, Story Mode, System |
| Help | Recent Activity, Diagnostics |

The interface includes:

- live OLED preview
- next alarm
- Wi-Fi state
- playback state
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

---

## Privacy and network safety

The web interface does not use a password.

Any device that can reach TCP port `8080` can control the clock.

Keep the clock on a trusted home network.

Do not:

- forward port `8080` through the router
- expose the clock directly to the internet
- connect it to an untrusted guest network
- connect it to a public network

The clock does not require a cloud account or cloud control service.

The API runs as a restricted system account and communicates with the hardware service through a private Unix socket.

---

## Diagnostics and maintenance

### Diagnostics

The Diagnostics page reports:

- IP address
- hostname
- connected Wi-Fi network
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

These details are intended to answer common parent questions such as:

- Is the clock connected to Wi-Fi?
- Does the clock have the correct time?
- Is the microSD card becoming full?
- Are both services running?
- Did the last alarm actually trigger?
- Is audio currently playing?

### Device identity

The System page reports:

- product version
- HTTP API version
- private IPC version
- compile time
- hostname
- Raspberry Pi serial-derived identity
- `MK-<serial>` device identifier
- storage usage
- Day Image library size
- Bedtime Image library size
- Music library size
- Stories library size

The device identifier makes it easier to tell multiple clocks apart.

### Recent Activity

The event log records important actions, including:

- service startup
- alarm events
- playback requests
- stop requests
- message activity
- uploads
- configuration changes
- errors

### Backup and restore

Maintenance tools support:

- configuration backup
- Day Image backup
- Bedtime Image backup
- font backup
- restore
- factory reset

Music and story audio are excluded from the simplified backup to keep the archive manageable.

> [!CAUTION]
> Factory reset removes user settings and uploaded assets covered by the reset. Download anything important first.

---

## Hardware

### Supported Raspberry Pi models

- Raspberry Pi Zero
- Raspberry Pi Zero 2 W

The Raspberry Pi Zero 2 W is recommended because it processes uploads and serves the web interface more quickly.

### Recommended components

| Component | Recommended hardware |
|:--|:--|
| Raspberry Pi | Raspberry Pi Zero 2 W |
| Display | SSD1322 256x64 OLED |
| Amplifier | MAX98357A I2S |
| Speaker | 4-ohm, 3-watt |
| Touch sensor | TTP223B |
| Lighting | Common-cathode RGB LED module |
| Storage | Reliable microSD card |
| Power adapter | Stable 5 V, 2 A USB-A adapter |
| Power cable | USB-A to USB-C |

Review `pinouts.md` before applying power.

See `BOM.md` for the current parts list, required quantities, purchase costs, and estimated per-clock build price.

---

## Software architecture

mk-piclock Kids uses two native services.

### `mk-piclock-core`

This service directly controls the clock hardware and real-time behaviour:

- OLED framebuffer
- SSD1322 SPI communication
- GPIO
- touch input
- RGB LED
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

This service provides the web interface and file-management features:

- local web GUI
- HTTP API
- upload validation
- PNG conversion
- font upload
- MP3 metadata extraction
- MP3 transcoding
- upload queue
- Music library
- Stories library
- image libraries
- diagnostics
- maintenance actions
- OpenAPI document

### Internal communication

The services communicate through:

```text
/run/mk-piclock/core.sock
```

The private binary protocol is **IPC version 11**.

---

## Time and NTP

mk-piclock Kids reads the Linux system clock. It does not contact an NTP server itself.

Raspberry Pi OS Lite based on Debian 13 Trixie normally uses `systemd-timesyncd`.

Set the timezone and enable network time:

```bash
sudo timedatectl set-timezone America/Edmonton
sudo timedatectl set-ntp true
```

Check synchronization:

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

### Why time synchronization matters

The clock does not have a battery-backed real-time clock.

After a power loss, it depends on Linux restoring the approximate time and then reaching a network time source.

Alarms should not be trusted until the clock reports that time is synchronized.

---

## Raspberry Pi boot configuration

Raspberry Pi OS Lite based on Debian 13 Trixie stores its boot configuration in:

```text
/boot/firmware/config.txt
```

### 1. Back up the existing file

```bash
sudo cp /boot/firmware/config.txt \
  /boot/firmware/config.txt.mk-piclock-backup
```

### 2. Edit the file

```bash
sudo nano /boot/firmware/config.txt
```

Add these lines below the existing `[all]` section:

```ini
# mk-piclock hardware configuration

# Enable SPI0 for the SSD1322 OLED.
dtparam=spi=on

# Disable the Raspberry Pi built-in audio device.
dtparam=audio=off

# Enable the MAX98357A I2S amplifier.
dtoverlay=max98357a

# Minimize memory reserved for legacy GPU functions.
gpu_mem=16
```

Do not add another `[all]` heading if the file already contains one.

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

### 6. Review the configured values

```bash
grep -E \
  '^(dtparam=spi|dtparam=audio|dtoverlay=max98357a|gpu_mem=)' \
  /boot/firmware/config.txt
```

Older Raspberry Pi OS releases used `/boot/config.txt`. Debian 13 Trixie uses `/boot/firmware/config.txt`.

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

| Package | Why it is needed |
|:--|:--|
| `build-essential` | Compiler, linker, and normal build tools |
| `pkg-config` | Finds installed development libraries |
| `libgpiod-dev` | Lets the clock control GPIO pins |
| `gpiod` | Provides GPIO diagnostic commands |
| `libpng-dev` | Reads and converts PNG artwork |
| `libfreetype-dev` | Renders TrueType and OpenType fonts |
| `libasound2-dev` | Provides ALSA audio output |
| `libmpg123-dev` | Decodes and validates MP3 files |
| `libmp3lame-dev` | Re-encodes MP3 files inside the API |
| `libmicrohttpd-dev` | Provides the local web server |
| `alsa-utils` | Provides audio testing tools |
| `unzip` | Supports archive handling |

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

Do not run:

```bash
sudo make install
```

The Makefile calls `sudo` only for the installation steps that actually require it.

After installation:

1. restart both services
2. open the web interface
3. hard-refresh the browser
4. confirm that the Product, HTTP API, and Private IPC versions match

---

## Storage locations

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

### Check both services

```bash
systemctl status \
  mk-piclock-core.service \
  mk-piclock-api.service
```

### View recent core activity

```bash
sudo journalctl -b \
  -u mk-piclock-core.service \
  -n 100 \
  --no-pager
```

### View recent API activity

```bash
sudo journalctl -b \
  -u mk-piclock-api.service \
  -n 100 \
  --no-pager
```

### Follow both logs live

```bash
sudo journalctl -f \
  -u mk-piclock-core.service \
  -u mk-piclock-api.service
```

---

## Known limitations

- The MAX98357A amplifier and speaker may make a small pop when audio starts or stops.
- **Clear Queue** removes waiting files but does not cancel the active transcode.
- The clock has no battery-backed real-time clock.
- Correct startup time depends on Linux time restoration and NTP.
- The web interface has no password. The home network provides the access control.
- Use a USB-A to USB-C cable. USB-C to USB-C power may not work because the clock does not implement USB-C current detection or negotiation.
- The physical OLED colour is fixed by the display panel.
- The GUI panel-colour setting changes browser previews only.
- Backup archives exclude Music and Stories.
- MP3 transcoding is slower on a Raspberry Pi Zero than on a Zero 2 W.
- Multiple MP3 files can be uploaded together, but only one file is transcoded at a time.
- Common-anode RGB LEDs are not supported.

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
| `BOM.md` | Parts, quantities, pricing, and estimated build cost |
| `RELEASE_NOTES.md` | Current release summary |
