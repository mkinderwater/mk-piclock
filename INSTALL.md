# mk-piclock v1.6.10: Build and Install

## Supported target

Use Raspberry Pi OS Lite based on Debian 13 Trixie. The source uses the libgpiod 2.x C API.

Recommended targets:

- Raspberry Pi Zero 2 W: Raspberry Pi OS Lite 32-bit or 64-bit, build with two jobs.
- Raspberry Pi 5: Raspberry Pi OS Lite 64-bit, build with four jobs.

Confirm the OS:

```bash
cat /etc/os-release
```

The output should identify Debian 13 and Trixie.

## 1. Copy and extract the release

Copy the ZIP to the Pi, then run:

```bash
cd ~
rm -rf mk-piclock-v1.6.10-network-status
unzip mk-piclock-v1.6.10-network-status.zip
cd mk-piclock-v1.6.10-network-status
```

When a checksum file is supplied:

```bash
sha256sum -c mk-piclock-v1.6.10-network-status.zip.sha256
```

## 2. Install build dependencies

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
  libmicrohttpd-dev \
  alsa-utils \
  unzip \
  curl
```

Confirm libgpiod is 2.x:

```bash
pkg-config --modversion libgpiod
```

Confirm the HTTP development header exists:

```bash
test -f /usr/include/microhttpd.h && echo "libmicrohttpd header installed"
```

Optional dependency check:

```bash
pkg-config --modversion \
  libgpiod \
  libmicrohttpd \
  freetype2 \
  libpng \
  libmpg123 \
  alsa
```

## 3. Connect GPIO-header power

Power the Raspberry Pi from a regulated 5 V supply through the 40-pin header:

```text
External +5 V -> Pi 5 V, physical pin 4
External GND  -> Pi GND, physical pin 9
```

The amplifier uses separate physical connectors:

```text
MAX98357A VIN -> Pi 5 V, physical pin 2
MAX98357A GND -> Pi GND, physical pin 14
```

Pins 2 and 4 are electrically connected to the same Raspberry Pi 5 V rail. All ground pins are common.

Use a regulated supply sized for the Pi, OLED, amplifier, and speaker load. Do not connect USB power while the GPIO-header supply is connected.

## 4. Configure boot firmware

Edit:

```bash
sudo nano /boot/firmware/config.txt
```

Under the existing `[all]` section, ensure these entries exist once:

```ini
[all]

dtparam=spi=on
dtparam=audio=off
dtoverlay=max98357a,no-sdmode

# Headless appliance. Reserve the minimum GPU memory.
gpu_mem=16
```

Do not add a second `[all]` section when one already exists.

Save and reboot:

```bash
sudo reboot
```

## 5. Connect and verify hardware

### Confirmed SSD1322 OLED wiring

This wiring is confirmed working with the smaller 3.12-inch 256×64 SSD1322 OLED used by mk-piclock:

| OLED pin | Signal | Raspberry Pi |
| ---: | --- | --- |
| 1 | VSS | GND, physical pin 6 |
| 2 | VCC_IN | 3.3 V, physical pin 1 |
| 4 | D0 / CLK | GPIO11 / SPI0 SCLK, physical pin 23 |
| 5 | D1 / DIN | GPIO10 / SPI0 MOSI, physical pin 19 |
| 14 | D/C# | GPIO25, physical pin 22 |
| 15 | RES# | GPIO27, physical pin 13 |
| 16 | CS# | GPIO8 / SPI0 CE0, physical pin 24 |

Treat this table as the authoritative OLED wiring.

Do not substitute the discarded GPIO27/GPIO24 test mapping.

The core expects:

```text
SPI device: /dev/spidev0.0
OLED CS:    GPIO8 / SPI0 CE0, physical pin 24
OLED DC:    GPIO25, physical pin 22
OLED RST:   GPIO27, physical pin 13
```

The OLED does not use SPI MISO. Connect only the OLED pins listed above.

### TTP223B wiring

Connect the touch module at 3.3 V:

```text
TTP223B VCC -> Pi 3.3 V, physical pin 17
TTP223B GND -> Pi GND, physical pin 39
TTP223B OUT -> Pi GPIO20, physical pin 38
```

The core treats the TTP223B output as active-high and applies software debounce.

Do not power the module from 5 V while its output is connected directly to a Raspberry Pi GPIO.

Touch actions:

```text
Short press:   stop the current song
Hold 3 seconds: play a random uploaded song
```

### MAX98357A wiring

```text
MAX98357A VIN            -> Pi 5 V, physical pin 2
MAX98357A GND            -> Pi GND, physical pin 14
MAX98357A BCLK           -> Pi GPIO18, physical pin 12
MAX98357A LRC/LRCLK/WS   -> Pi GPIO19, physical pin 35
MAX98357A DIN            -> Pi GPIO21, physical pin 40
MAX98357A SD/EN          -> Not connected
```

Connect the speaker only to the amplifier:

```text
MAX98357A SPK+ -> Speaker +
MAX98357A SPK- -> Speaker -
```

Do not connect either speaker terminal to Raspberry Pi ground.

### Verify interfaces

After reconnecting:

```bash
ls -l /dev/spidev0.0
ls -l /dev/gpiochip0
gpiodetect
aplay -l
```

Expected hardware interfaces:

```text
SPI:       /dev/spidev0.0
GPIO chip: /dev/gpiochip0
OLED DC:   GPIO25
OLED RST:  GPIO27
Touch OUT: GPIO20, physical pin 38
Audio:     ALSA device "default"
```

Do not continue until `/dev/spidev0.0` and `/dev/gpiochip0` exist and ALSA lists the I2S audio device.

## 6. Build

Raspberry Pi Zero 2 W:

```bash
make clean
make -j2
```

Raspberry Pi 5:

```bash
make clean
make -j4
```

Confirm both programs exist:

```bash
ls -lh mk-piclock-core mk-piclock-api
```

Expected binaries:

```text
mk-piclock-core
mk-piclock-api
```

## 7. Install

Run:

```bash
make install
```

Do not prefix the full command with `sudo`. The Makefile invokes `sudo` only for privileged installation steps.

The install target:

- Creates the `mk-piclock` group.
- Creates separate `mk-piclock-core` and `mk-piclock-api` users.
- Installs both binaries under `/opt/mk-piclock`.
- Installs the modular GUI and OpenAPI document.
- Preserves existing assets and clock configuration.
- Installs and enables both systemd services.
- Removes the old single-service installation when present.

An ignored error from this command is harmless when the old service does not exist:

```text
systemctl disable --now mk-piclock.service
```

## 8. Start and verify services

```bash
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

Check status:

```bash
sudo systemctl --no-pager --full status \
  mk-piclock-core.service \
  mk-piclock-api.service
```

Confirm both start at boot:

```bash
systemctl is-enabled \
  mk-piclock-core.service \
  mk-piclock-api.service
```

Expected:

```text
enabled
enabled
```

Test the API locally:

```bash
curl -s http://127.0.0.1:8080/api/v1/status
```

Open the GUI:

```text
http://<clock-ip>:8080/
```

Port `8080` is required unless `MK_PICLOCK_API_PORT` is changed.

## 9. Verify v1.6.10 features

### Verify touch input

Read touch status:

```bash
curl -s http://127.0.0.1:8080/api/v1/status \
  | grep -o '"touch_[^"]*"[^,}]*'
```

Expected while idle:

```text
"touch_ok":1
"touch_pressed":0
"touch_gpio":20
```

Touch and hold the sensor while repeating the status request. `touch_pressed` should become `1`.

Functional test:

1. Start a song from the Music page.
2. Tap the sensor and confirm the song stops.
3. Hold the sensor for three seconds and confirm a random uploaded song starts.
4. Confirm `Title - Artist` appears on the clock when song metadata display is enabled.

Open each GUI page and perform a safe action. The notice bar should describe the exact result, such as:

```text
Playing <title>
Screen cleared
Alarm 1 saved
Message scheduled in 10 seconds
```

List music and ID3 metadata:

```bash
curl -s http://127.0.0.1:8080/api/v1/assets/music
```

The response includes the legacy `files` array and a `tracks` array containing:

```text
file
title
artist
display
id3
```

Schedule a message for 10 seconds later:

```bash
curl -s \
  --data-urlencode 'message_text=Time for school' \
  --data 'delay_seconds=10' \
  http://127.0.0.1:8080/api/v1/display/message
```

Enable OLED song metadata:

```bash
curl -s \
  --data 'show_song_metadata=1' \
  http://127.0.0.1:8080/api/v1/config/audio
```

Disable it:

```bash
curl -s \
  --data 'show_song_metadata=0' \
  http://127.0.0.1:8080/api/v1/config/audio
```

## Upgrade from an earlier release

Build and install normally:

```bash
cd ~/mk-piclock-v1.6.10-network-status
make clean
make -j2
make install
sudo systemctl restart mk-piclock-core mk-piclock-api
```

Existing faces, music, fonts, alarms, clock name, display settings, and volume are retained.

The `show_song_metadata` setting defaults to enabled until saved otherwise.

Upgrade the API and core together so their product versions remain aligned. Releases v1.6.5 and earlier must upgrade both programs because v1.6.6 introduced binary IPC protocol version 4.

## Module enable and disable

Edit:

```bash
sudo nano /opt/mk-piclock/web/modules/modules.json
```

Set a module to:

```json
{"enabled": false}
```

Refresh the browser. Disabled modules are removed from the menu and their HTML, CSS, and JavaScript are not loaded.

## Configuration locations

```text
/opt/mk-piclock/config/clock.conf
/opt/mk-piclock/config/event.log
/opt/mk-piclock/assets/faces
/opt/mk-piclock/assets/bedtime-faces
/opt/mk-piclock/assets/music
/opt/mk-piclock/assets/fonts
```

## Logs

Core logs:

```bash
sudo journalctl \
  -b \
  -u mk-piclock-core.service \
  -n 100 \
  --no-pager
```

API logs:

```bash
sudo journalctl \
  -b \
  -u mk-piclock-api.service \
  -n 100 \
  --no-pager
```

Follow both services:

```bash
sudo journalctl \
  -f \
  -u mk-piclock-core.service \
  -u mk-piclock-api.service
```

## Common build and hardware problems

### `microhttpd.h: No such file or directory`

Install the development package:

```bash
sudo apt update
sudo apt install -y libmicrohttpd-dev
make clean
make -j2
```

### libmicrohttpd status-name warnings

v1.6.10 uses:

```text
MHD_HTTP_URI_TOO_LONG
MHD_HTTP_CONTENT_TOO_LARGE
```

Run a clean build so older objects are not reused:

```bash
make clean
make -j2
```

### `gpiod_*` declarations are missing

Check the installed major version:

```bash
pkg-config --modversion libgpiod
```

It must be 2.x.

### `/dev/spidev0.0` is missing

Confirm SPI is enabled:

```bash
grep -n '^[[:space:]]*dtparam=spi=on' \
  /boot/firmware/config.txt
```

Then reboot:

```bash
sudo reboot
```

### `/dev/gpiochip0` is missing

```bash
gpiodetect
ls -l /dev/gpiochip*
```

The current source expects `/dev/gpiochip0`.

### OLED remains blank

Confirm the authoritative wiring:

```text
OLED pin 1  VSS     -> Pi GND, physical pin 6
OLED pin 2  VCC_IN  -> Pi 3.3 V, physical pin 1
OLED pin 4  D0/CLK  -> Pi GPIO11, physical pin 23
OLED pin 5  D1/DIN  -> Pi GPIO10, physical pin 19
OLED pin 14 D/C#    -> Pi GPIO25, physical pin 22
OLED pin 15 RES#    -> Pi GPIO27, physical pin 13
OLED pin 16 CS#     -> Pi GPIO8 / CE0, physical pin 24
```

Confirm the service has access to SPI and GPIO:

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
id mk-piclock-core
```

Check the core log:

```bash
sudo journalctl \
  -b \
  -u mk-piclock-core.service \
  -n 100 \
  --no-pager
```

### API is offline

```bash
sudo systemctl restart mk-piclock-core mk-piclock-api

sudo journalctl \
  -b \
  -u mk-piclock-api.service \
  -n 100 \
  --no-pager
```

### No audio

```bash
aplay -l
speaker-test -D default -c 2 -t sine
```

Stop the test with `Ctrl+C`.

Confirm the boot settings:

```bash
grep -nE \
  '^[[:space:]]*(dtparam=audio=off|dtoverlay=max98357a,no-sdmode)' \
  /boot/firmware/config.txt
```

### Incorrect GPU memory reservation

Confirm the headless setting exists once:

```bash
grep -n '^[[:space:]]*gpu_mem=16' \
  /boot/firmware/config.txt
```

A reboot is required after changing firmware configuration.
