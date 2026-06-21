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

Pins 2 and 4 are electrically the same Raspberry Pi 5 V rail, while all ground pins are common. Use a regulated supply sized for the Pi, OLED, amplifier, and speaker load. Do not connect USB power while the header supply is connected.

## 4. Enable SPI and MAX98357A audio

Edit:

```bash
sudo nano /boot/firmware/config.txt
```

Ensure these entries exist once:

```ini
dtparam=spi=on
dtparam=audio=off
dtoverlay=max98357a,no-sdmode
```

Reduce the GPU memory reservation because this headless clock does not use the desktop or 3D graphics:

```bash
sudo raspi-config
```

Navigate to **Performance Options** (or **Advanced Options** on older OS versions) > **GPU Memory**. Set the value to **16 MB**. There is no need to reserve 64 MB for a GPU workload the clock does not use.

Save and reboot:

```bash
sudo reboot
```

## 5. Verify hardware interfaces

After reconnecting:

```bash
ls -l /dev/spidev0.0
ls -l /dev/gpiochip0
gpiodetect
aplay -l
```

The core expects:

```text
SPI:       /dev/spidev0.0
GPIO chip: /dev/gpiochip0
OLED DC:   GPIO 25
OLED RST:  GPIO 27
Touch OUT: GPIO 20, physical pin 38
Audio:     ALSA device "default"
```

### TTP223B wiring

Connect the touch module at 3.3 V:

```text
TTP223B VCC -> Pi 3.3 V, physical pin 17
TTP223B GND -> Pi GND, physical pin 39
TTP223B OUT -> Pi GPIO20, physical pin 38
```

The core treats the TTP223B output as active-high and applies software debounce. Do not power the module from 5 V when its output is connected directly to the Raspberry Pi GPIO.

Touch actions:

```text
Short press: stop the current song
Hold 3 seconds: play a random uploaded song
```

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
sudo systemctl restart mk-piclock-core.service mk-piclock-api.service
```

Check status:

```bash
sudo systemctl --no-pager --full status \
  mk-piclock-core.service \
  mk-piclock-api.service
```

Confirm both start at boot:

```bash
systemctl is-enabled mk-piclock-core.service mk-piclock-api.service
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
curl -s http://127.0.0.1:8080/api/v1/status | grep -o '"touch_[^"]*"[^,}]*'
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


Open each GUI page and perform a safe action. The notice bar should describe the exact result, such as `Playing <title>`, `Screen cleared`, `Alarm 1 saved`, or `Message scheduled in 10 seconds`.


List music and ID3 metadata:

```bash
curl -s http://127.0.0.1:8080/api/v1/assets/music
```

The response includes both the legacy `files` array and a `tracks` array containing:

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

Existing faces, music, fonts, alarms, clock name, display settings, and volume are retained. The new `show_song_metadata` setting defaults to enabled until saved otherwise.

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

```bash
sudo journalctl -b -u mk-piclock-core.service -n 100 --no-pager
sudo journalctl -b -u mk-piclock-api.service -n 100 --no-pager
```

Follow logs live:

```bash
sudo journalctl -f -u mk-piclock-core.service -u mk-piclock-api.service
```

## Common build problems

### `microhttpd.h: No such file or directory`

Install the development package:

```bash
sudo apt update
sudo apt install -y libmicrohttpd-dev
make clean
make -j2
```

### libmicrohttpd status-name warnings

v1.6.10 uses the non-deprecated status names:

```text
MHD_HTTP_URI_TOO_LONG
MHD_HTTP_CONTENT_TOO_LARGE
```

Run `make clean` before rebuilding so an older object or binary is not reused.

### `gpiod_*` declarations are missing

Check the installed major version:

```bash
pkg-config --modversion libgpiod
```

It must be 2.x.

### API is offline

```bash
sudo systemctl restart mk-piclock-core mk-piclock-api
sudo journalctl -b -u mk-piclock-api -n 100 --no-pager
```

### Core cannot open the OLED

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
id mk-piclock-core
```

Confirm SPI is enabled and the service user belongs to the required hardware groups.

### No audio

```bash
aplay -l
speaker-test -D default -c 2 -t sine
```

Stop the test with `Ctrl+C`.
