# mk-piclock-c Build and Install Guide

This guide installs `mk-piclock-c` on a Raspberry Pi and places the daemon, service file, web GUI, and runtime asset folders in the expected `/opt/mk-piclock` location.

This release renames the message editor to Screen Message, keeps multiple MP3 upload support, and keeps the phone-friendly message GUI where the screen preview is the text entry point. The typed message is now vertically centered in the preview to match the actual clock display. It also keeps the previous cleanup and stability fixes: table-driven HTTP routing, flat 5x7 font lookup, X-macro config serialization, consolidated delete handlers, lighter JSON construction, request-thread isolation, socket timeouts, static OLED SPI flush buffers, and Wi-Fi status without `popen()`.

## What `make install` copies

The supplied `Makefile` installs the project into `/opt/mk-piclock`.

Installed binary:

```text
/opt/mk-piclock/mk-piclock
```

Installed web GUI assets:

```text
/opt/mk-piclock/web/index.html
/opt/mk-piclock/web/app.js
/opt/mk-piclock/web/style.css
```

Installed systemd service:

```text
/etc/systemd/system/mk-piclock.service
```

Runtime folders created by install:

```text
/opt/mk-piclock/assets/faces
/opt/mk-piclock/assets/bedtime-faces
/opt/mk-piclock/assets/music
/opt/mk-piclock/assets/fonts
/opt/mk-piclock/config
/opt/mk-piclock/web
```

The service runs from:

```text
WorkingDirectory=/opt/mk-piclock
ExecStart=/opt/mk-piclock/mk-piclock
```

So yes: the HTML/CSS/JS assets are copied into the required `/opt/mk-piclock/web/` folder.

## 1. Copy the release ZIP to the Pi

From your workstation:

```bash
scp mk-piclock-v1.5.23-final-optimization.zip matthew@rylie-clock:~/
```

Or copy it by any other method.

## 2. Install dependencies

On the Pi:

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
  alsa-utils \
  unzip
```

## 3. Enable SPI and MAX98357A audio

Edit the Raspberry Pi boot config.

Bookworm path:

```bash
sudo nano /boot/firmware/config.txt
```

Older Raspberry Pi OS path:

```bash
sudo nano /boot/config.txt
```

Ensure these lines are present:

```ini
dtparam=spi=on
dtparam=audio=off
dtoverlay=max98357a,no-sdmode
```

Reboot if you changed the file:

```bash
sudo reboot
```

## 4. Unzip and build

```bash
cd ~
unzip mk-piclock-v1.5.23-final-optimization.zip
cd mk-piclock-v1.5.23-final-optimization

make clean
make
```

Expected build style:

```text
gcc ... mk-piclock.c ... -o mk-piclock
```

## 5. Install

```bash
sudo make install
```

The install target performs these actions:

```make
sudo mkdir -p /opt/mk-piclock/assets/faces /opt/mk-piclock/assets/bedtime-faces /opt/mk-piclock/assets/music /opt/mk-piclock/assets/fonts /opt/mk-piclock/config /opt/mk-piclock/web
sudo cp mk-piclock /opt/mk-piclock/mk-piclock
sudo cp -r web/* /opt/mk-piclock/web/
sudo cp mk-piclock.service /etc/systemd/system/mk-piclock.service
sudo systemctl daemon-reload
sudo systemctl enable mk-piclock
```

## 6. Start or restart the service

```bash
sudo systemctl restart mk-piclock
```

Check status:

```bash
sudo systemctl status mk-piclock
```

Follow systemd logs:

```bash
journalctl -u mk-piclock -f
```

The web GUI also has a Log page. It shows recent clock events from:

```text
/opt/mk-piclock/config/event.log
```

## 7. Open the GUI

Hostname:

```text
http://rylie-clock/
```

Or IP:

```text
http://192.168.24.95/
```

## 8. Verify OLED and audio

SPI device:

```bash
ls -l /dev/spidev0.0
```

ALSA devices:

```bash
aplay -l
```

Speaker test:

```bash
speaker-test -D default -c2 -t sine
```

API status check:

```bash
curl http://127.0.0.1:8080/api/status
```

## Upgrade procedure

For a future release:

```bash
unzip mk-piclock-vX.X.X.zip
cd mk-piclock-vX.X.X

make clean
make
sudo make install
sudo systemctl restart mk-piclock
```

Existing configuration and uploaded assets are retained under `/opt/mk-piclock` unless you delete them from the GUI.

## Clean reinstall, preserving nothing

Only run this if you want to remove uploaded faces, music, fonts, and config:

```bash
sudo systemctl stop mk-piclock
sudo rm -rf /opt/mk-piclock
sudo rm -f /etc/systemd/system/mk-piclock.service
sudo systemctl daemon-reload
```

Then run the normal build and install steps again.
