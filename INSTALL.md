# mk-piclock v1.7.6: Build and Install

> A practical installation guide for the Raspberry Pi Zero W and Zero 2 W builds of mk-piclock Kids.

| Product | HTTP API | Private IPC |
|:--|:--|:--|
| `1.7.6` | `1.25` | `16` |

mk-piclock uses two native C services:

- `mk-piclock-core` controls the OLED, touch sensor, RGB LED, alarms, messages, and audio.
- `mk-piclock-api` provides the browser interface, uploads, media processing, backups, and diagnostics.

Install both services from the same release.

## Contents

- [Before you start](#before-you-start)
- [Supported platform](#supported-platform)
- [Prepare Raspberry Pi OS](#prepare-raspberry-pi-os)
- [Install build dependencies](#install-build-dependencies)
- [Configure the Raspberry Pi hardware](#configure-the-raspberry-pi-hardware)
- [Verify the hardware interfaces](#verify-the-hardware-interfaces)
- [Extract and verify the release](#extract-and-verify-the-release)
- [Build mk-piclock](#build-mk-piclock)
- [Install the services](#install-the-services)
- [Open the web interface](#open-the-web-interface)
- [Reset a lost password](#reset-a-lost-password)
- [Initial setup](#initial-setup)
- [Upgrade an existing clock](#upgrade-an-existing-clock)
- [Storage locations](#storage-locations)
- [Logs and service control](#logs-and-service-control)
- [Troubleshooting](#troubleshooting)

---

## Before you start

You will need:

- Raspberry Pi Zero W or Raspberry Pi Zero 2 W
- Raspberry Pi OS Lite based on Debian 13 Trixie
- SSD1322 256x64 OLED
- MAX98357A I2S amplifier
- 4-ohm, 3-watt speaker
- TTP223B touch sensor
- Common-cathode RGB LED with one resistor per colour channel
- MicroSD card
- Reliable 5 V power supply

> [!IMPORTANT]
> Review `pinouts.md` before applying power. Confirm every 3.3 V, 5 V, ground, GPIO, SPI, I2S, and LED connection.

### Power

Use a reliable **5 V, 2 A or better** power supply.

A **USB-A to USB-C cable** is recommended for the current enclosure power connection. The simple USB-C power board does not perform full USB-C current negotiation, so some USB-C to USB-C supplies may not provide power correctly.

Do not power the clock from the Raspberry Pi and an external 5 V input at the same time.

---

## Supported platform

### Operating system

```text
Raspberry Pi OS Lite
Debian 13 Trixie
```

The source uses the **libgpiod 2.x API**.

Confirm the operating system:

```bash
cat /etc/os-release
```

Confirm the Pi model:

```bash
tr -d '\0' </proc/device-tree/model
printf '\n'
```

### Build jobs

| Raspberry Pi | Build command |
|:--|:--|
| Zero W | `make -j1` |
| Zero 2 W | `make -j2` |

The project is intended for the Pi Zero and Zero 2 family. Pi 5 instructions are intentionally excluded.

---

## Prepare Raspberry Pi OS

The easiest setup is through Raspberry Pi Imager.

Before writing the microSD card, configure:

- hostname
- Wi-Fi network and password
- username and password
- SSH, when remote access is required
- timezone

For Calgary and Edmonton time:

```bash
sudo timedatectl set-timezone America/Edmonton
sudo timedatectl set-ntp true
```

Confirm time synchronization:

```bash
timedatectl status
timedatectl show -p NTPSynchronized
```

Expected values include:

```text
System clock synchronized: yes
NTP service: active
NTPSynchronized=yes
```

mk-piclock reads the Linux system clock. It does not contact an NTP server itself.

> [!NOTE]
> The clock has no built-in battery-backed real-time clock. Correct time after a power loss depends on Linux restoring time and reaching a network time source.

---

## Install build dependencies

Update the package list:

```bash
sudo apt update
```

Install the required build packages:

```bash
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

Confirm libgpiod 2.x:

```bash
pkg-config --modversion libgpiod
```

---

## Configure the Raspberry Pi hardware

Raspberry Pi OS Lite based on Debian 13 stores boot configuration in:

```text
/boot/firmware/config.txt
```

### 1. Back up the file

```bash
sudo cp /boot/firmware/config.txt \
  /boot/firmware/config.txt.mk-piclock-backup
```

### 2. Edit the file

```bash
sudo nano /boot/firmware/config.txt
```

Add these lines under the existing `[all]` section:

```ini
# mk-piclock hardware

# SSD1322 OLED on SPI0
dtparam=spi=on

# Disable the Pi analogue audio device
dtparam=audio=off

# MAX98357A I2S amplifier
dtoverlay=max98357a,no-sdmode

# Reduce memory reserved for legacy GPU functions
gpu_mem=16
```

> [!IMPORTANT]
> Do not add a second `[all]` heading. Each setting should appear only once.

No changes to `/boot/firmware/cmdline.txt` are required.

### 3. Reboot

```bash
sudo reboot
```

---

## Verify the hardware interfaces

After rebooting, verify SPI and GPIO:

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
gpiodetect
```

Verify that Linux created an ALSA sound card and playback device:

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
```

The output should contain an I2S playback device. The exact card name can vary with the Raspberry Pi OS kernel.

### Expected GPIO assignments

| Device | BCM GPIO | Physical pin |
|:--|--:|--:|
| OLED SPI clock | GPIO11 | 23 |
| OLED SPI data | GPIO10 | 19 |
| OLED chip select | GPIO8 | 24 |
| OLED D/C | GPIO25 | 22 |
| OLED reset | GPIO27 | 13 |
| Touch sensor output | GPIO20 | 38 |
| RGB LED red | GPIO5 | 29 |
| RGB LED green | GPIO6 | 31 |
| RGB LED blue | GPIO13 | 33 |

See `pinouts.md` for the complete power, OLED, amplifier, speaker, touch, and RGB LED wiring table.

---

## Extract and verify the release

Place the release ZIP and `.sha256` file in the same directory.

For this updated v1.7.6 package:

```bash
cd ~
sha256sum -c mk-piclock-v1.7.6-install-update.zip.sha256
rm -rf mk-piclock-v1.7.6-install-update
unzip mk-piclock-v1.7.6-install-update.zip
cd mk-piclock-v1.7.6-install-update
```

A successful checksum test reports:

```text
mk-piclock-v1.7.6-install-update.zip: OK
```

When installing a differently named package, replace the filenames and directory name in the commands above.

---

## Build mk-piclock

### Raspberry Pi Zero W

```bash
make clean
make -j1
```

### Raspberry Pi Zero 2 W

```bash
make clean
make -j2
```

Confirm both programs were created:

```bash
ls -lh mk-piclock-core mk-piclock-api
```

Do not continue if either binary is missing.

---

## Install the services

Run:

```bash
make install
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

> [!IMPORTANT]
> Do not run `sudo make install`. The Makefile uses `sudo` only for the steps that require it.

The installer:

- creates restricted service accounts
- grants the core access to audio, SPI, and GPIO
- installs both binaries under `/opt/mk-piclock`
- installs the default alarm and message chime
- installs the complete web interface and OpenAPI document
- installs and enables both systemd services
- preserves existing configuration and uploaded media during upgrades

Confirm both services are running:

```bash
sudo systemctl --no-pager --full status \
  mk-piclock-core.service \
  mk-piclock-api.service
```

Both should show:

```text
Active: active (running)
```

---

## Open the web interface

### Find the clock address from the OLED

Simply hold down the touch sensor. The OLED network diagnostics screen shows:

- Wi-Fi network name
- Wi-Fi signal strength
- IP address
- hostname

Tap the sensor to close the screen. It also closes automatically.

### Find the address from Linux

```bash
hostname -I
```

Open the clock in a browser:

```text
http://<clock-ip>:8080/
```

Example:

```text
http://192.168.1.42:8080/
```

### Password behaviour

- When no password is configured, the controls open directly.
- When a password exists, the browser asks for it before opening the controls.
- Set, change, or remove the password under **System**.

The password is intentionally simple and stored as plain text on the clock. Keep port `8080` on a trusted home network. Do not expose it to the internet.

---

## Reset a lost password

A lost web password can be cleared from an SSH session or a local Linux terminal on the Raspberry Pi. You must still know the Raspberry Pi Linux account password to use `sudo`.

The OLED network diagnostics screen can show the clock IP address when needed. Simply hold down the touch sensor to open it.

### 1. Connect to the clock

From another computer:

```bash
ssh <linux-user>@<clock-ip>
```

Replace `<linux-user>` with the Raspberry Pi OS username and `<clock-ip>` with the address shown on the OLED.

### 2. Stop the web service

```bash
sudo systemctl stop mk-piclock-api.service
```

### 3. Remove the saved web password

```bash
sudo rm -f \
  /opt/mk-piclock/config/web-password.txt \
  /opt/mk-piclock/config/.web-password.tmp
```

### 4. Start the web service

```bash
sudo systemctl start mk-piclock-api.service
sudo systemctl --no-pager --full status mk-piclock-api.service
```

The service should report `Active: active (running)`.

### 5. Open the web interface

Reload:

```text
http://<clock-ip>:8080/
```

The password prompt will no longer appear. Open **System** to set a new password, or leave it blank to keep password protection disabled.

> [!IMPORTANT]
> This resets only the mk-piclock web password. It does not change the Raspberry Pi Linux login and does not remove alarms, settings, images, music, stories, or backups.

---

## Initial setup

Complete the following in the web interface:

1. Open **Display** and set the clock name, time format, font, brightness, and bedtime schedule.
2. Choose the browser preview colour that matches the physical OLED panel.
3. Upload normal artwork under **Day Images**.
4. Upload night artwork under **Bedtime Images**.
5. Upload songs under **Music**.
6. Upload story MP3 files under **Stories**.
7. Configure the alarm slots.
8. Configure Story Mode and its intro text.
9. Open **Lighting**, select the six activity profiles, and test the RGB LED channels.
10. Open **System** and confirm network, NTP, storage, OLED, touch, and service health.
11. Set an optional web password under **System**.
12. Download an initial backup.

### Messages

Each OLED message can optionally play the built-in short chime when it appears. The chime is skipped when other audio is already playing.

### Touch controls

- Tap while an alarm is active to dismiss it.
- Tap while music or a story is playing to stop it.
- Hold, then release, to play a random song.
- Keep holding to open network diagnostics.
- Tap ten times within the Story Mode window to play a random story when Story Mode is enabled.

The diagnostic hold does not start music and is not counted toward Story Mode.

### Browser confirmations

Save, test, upload, delete, and other confirmations appear in a floating notice. The result remains visible even when the page is scrolled.

---

## Upgrade an existing clock

Installing a new release replaces the programs, web interface, service files, default alarm, message chime, and API document.

It does not intentionally remove:

- clock configuration
- alarm settings
- uploaded images
- uploaded music
- uploaded stories
- uploaded fonts
- RGB lighting settings
- optional web password

### Recommended upgrade steps

```bash
cd ~/mk-piclock-v1.7.6-install-update
make clean
make -j2
make install
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

Use `make -j1` on a Pi Zero W.

Confirm the installed versions under **System**:

```text
Product:     1.7.6
HTTP API:    1.25
Private IPC: 16
```

The core, API, and browser interface must come from the same release.

When the browser reports an API version mismatch:

1. Rebuild the release.
2. Run `make install` again.
3. Restart both services.
4. Hard-refresh the browser.

### Backup before a major change

Use **System > Download Backup** before large upgrades or configuration changes.

Uploaded music and stories are excluded from the built-in backup and should be copied separately when required:

```bash
sudo cp -a /opt/mk-piclock/assets/music ~/mk-piclock-music-backup
sudo cp -a /opt/mk-piclock/assets/stories ~/mk-piclock-stories-backup
```

---

## Storage locations

| Purpose | Path |
|:--|:--|
| Core binary | `/opt/mk-piclock/mk-piclock-core` |
| API binary | `/opt/mk-piclock/mk-piclock-api` |
| Day Images | `/opt/mk-piclock/assets/images` |
| Bedtime Images | `/opt/mk-piclock/assets/bedtime-images` |
| Music | `/opt/mk-piclock/assets/music` |
| Music processing | `/opt/mk-piclock/assets/music/.processing` |
| Stories | `/opt/mk-piclock/assets/stories` |
| Fonts | `/opt/mk-piclock/assets/fonts` |
| Default alarm | `/opt/mk-piclock/assets/default-alarm.mp3` |
| Message chime | `/opt/mk-piclock/assets/message-chime.mp3` |
| Clock configuration | `/opt/mk-piclock/config/clock.conf` |
| Optional web password | `/opt/mk-piclock/config/web-password.txt` |
| Event log | `/opt/mk-piclock/config/event.log` |
| Web interface | `/opt/mk-piclock/web` |
| OpenAPI document | `/opt/mk-piclock/api/openapi-v1.json` |
| Private core socket | `/run/mk-piclock/core.sock` |

Old `auth`, `faces`, `bedtime-faces`, and `bedtime_faces` directories are not used.

---

## Logs and service control

### View service status

```bash
sudo systemctl status mk-piclock-core.service
sudo systemctl status mk-piclock-api.service
```

### Restart both services

```bash
sudo systemctl restart \
  mk-piclock-core.service \
  mk-piclock-api.service
```

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

### Follow both logs

```bash
sudo journalctl -f \
  -u mk-piclock-core.service \
  -u mk-piclock-api.service
```

---

## Troubleshooting

### Web interface does not open

Confirm the API is running:

```bash
sudo systemctl restart mk-piclock-core mk-piclock-api
sudo systemctl --no-pager --full status mk-piclock-api
sudo journalctl -b -u mk-piclock-api -n 100 --no-pager
```

Confirm the clock IP:

```bash
hostname -I
```

Port `8080` must be reachable from the local network.

### Forgotten web password

Follow [Reset a lost password](#reset-a-lost-password). The recovery removes only the optional web password file and leaves the clock configuration and media intact.

### API version mismatch

Reinstall the complete release, not only one binary:

```bash
make clean
make -j2
make install
sudo systemctl restart mk-piclock-core mk-piclock-api
```

Use `make -j1` on a Pi Zero W, then hard-refresh the browser.

### OLED is blank

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
id mk-piclock-core
sudo journalctl -b -u mk-piclock-core -n 100 --no-pager
```

Confirm:

- SPI is enabled
- OLED wiring matches `pinouts.md`
- OLED power is 3.3 V where specified
- D/C, reset, and chip-select pins are correct
- `mk-piclock-core` belongs to the `spi` and `gpio` groups

### No sound

Check the kernel ALSA devices and the core service log:

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
id mk-piclock-core
sudo journalctl -b -u mk-piclock-core.service -n 100 --no-pager
```

Confirm:

- `dtoverlay=max98357a,no-sdmode` is active
- the amplifier is connected to the Pi I2S pins
- the speaker is connected to the amplifier output
- the amplifier SD/EN connection is not holding the board disabled
- `mk-piclock-core` belongs to the `audio` group

### Touch does not respond

Confirm the TTP223B output is connected to BCM GPIO20, physical pin 38.

Follow the core log while touching the sensor:

```bash
sudo journalctl -f -u mk-piclock-core.service
```

### Network diagnostics shows unavailable values

The OLED and System diagnostics read values exposed by the Linux network driver. SSID or signal fields can show unavailable when the interface is disconnected or the driver does not expose the value.

### Wrong time after startup

```bash
sudo timedatectl set-timezone America/Edmonton
sudo timedatectl set-ntp true
timedatectl status
```

Allow the Pi to connect to Wi-Fi and synchronize.

### Build header is missing

Install the matching development package, then rebuild:

```bash
make clean
make -j2
```

Common packages are:

- `libmicrohttpd-dev`
- `libgpiod-dev`
- `libasound2-dev`
- `libmpg123-dev`
- `libmp3lame-dev`
- `libfreetype-dev`
- `libpng-dev`

### Re-add existing image assets

The recommended method is uploading the original PNG files through **Day Images** and **Bedtime Images**. The API creates the matching OLED `.raw` files.

Existing converted artwork works only when both files share the same base name:

```text
example.png
example.raw
```

After copying files manually, restore ownership and restart the services:

```bash
sudo chown -R mk-piclock-api:mk-piclock /opt/mk-piclock/assets
sudo chmod -R u=rwX,g=rX,o= /opt/mk-piclock/assets
sudo systemctl restart mk-piclock-core mk-piclock-api
```

A PNG without its converted `.raw` partner does not appear in the OLED image rotation.

---

## Related documentation

| Document | Purpose |
|:--|:--|
| `README.md` | Features and normal use |
| `pinouts.md` | Complete hardware wiring |
| `CHANGELOG.md` | Version history |
| `RELEASE_NOTES.md` | Current release changes |
| `ADDON_API.md` | HTTP API details |
| `api/openapi-v1.json` | OpenAPI schema |
