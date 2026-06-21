# mk-piclock v1.6.7: Build and Install

## Supported target

Use Raspberry Pi OS Lite based on Debian 13 Trixie. The source uses the libgpiod 2.x C API.

Recommended targets:

- Raspberry Pi Zero 2 W: Raspberry Pi OS Lite 32-bit or 64-bit, build with two jobs.
- Raspberry Pi 5: Raspberry Pi OS Lite 64-bit, build with four jobs.

Confirm the OS:

```bash
cat /etc/os-release
```

The output must identify Debian 13 and Trixie.

## 1. Copy and extract the release

Copy the ZIP to the Pi, then run:

```bash
cd ~
rm -rf mk-piclock-v1.6.7-gui-feedback
unzip mk-piclock-v1.6.7-gui-feedback.zip
cd mk-piclock-v1.6.7-gui-feedback
```

When a checksum file is supplied:

```bash
sha256sum -c mk-piclock-v1.6.7-gui-feedback.zip.sha256
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
test -f /usr/include/microhttpd.h
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

## 3. Configure boot firmware

Edit:

```bash
sudoedit /boot/firmware/config.txt
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

Apply the changes:

```bash
sudo reboot
```

## 4. Verify hardware interfaces

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
Audio:     ALSA device "default"
```

Do not continue until the SPI and GPIO devices exist and ALSA lists the I2S audio device.

## 5. Build

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

## 6. Install

Run:

```bash
make install
```

Do not prefix the command with `sudo`. The Makefile invokes `sudo` only for privileged installation steps.

The install target:

- Creates the `mk-piclock` group.
- Creates separate `mk-piclock-core` and `mk-piclock-api` users.
- Installs both binaries under `/opt/mk-piclock`.
- Installs the modular GUI and OpenAPI document.
- Preserves existing assets and clock configuration.
- Installs and enables both systemd services.
- Removes the old single-service installation when present.

This ignored error is harmless when the old service does not exist:

```text
systemctl disable --now mk-piclock.service
```

## 7. Start and verify services

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

## 8. Verify v1.6.7 features

Open each GUI page and perform a safe action. The notice bar should report the exact result, such as:

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
cd ~/mk-piclock-v1.6.7-gui-feedback
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
sudoedit /opt/mk-piclock/web/modules/modules.json
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

Recent core logs:

```bash
sudo journalctl \
  -b \
  -u mk-piclock-core.service \
  -n 100 \
  --no-pager
```

Recent API logs:

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

## Common build problems

### `microhttpd.h: No such file or directory`

```bash
sudo apt update
sudo apt install -y libmicrohttpd-dev
make clean
make -j2
```

### libmicrohttpd status-name warnings

v1.6.7 uses:

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

Confirm the boot setting:

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

The current source expects `/dev/gpiochip0`. Change `GPIO_CHIP` in the source only when the target OS exposes the header GPIO controller under another device name.

### API is offline

```bash
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service

sudo journalctl \
  -b \
  -u mk-piclock-api.service \
  -n 100 \
  --no-pager
```

### Core cannot open the OLED

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
id mk-piclock-core
```

Confirm SPI is enabled and `mk-piclock-core` belongs to the required hardware groups.

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
