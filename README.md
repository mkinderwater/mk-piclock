# mk-piclock v1.6.10

Native C alarm clock software for a Raspberry Pi, SSD1322 256x64 OLED, and MAX98357A I2S audio.

Hardware wiring: see [`pinouts.md`](pinouts.md).

## Architecture

```text
Browser or add-on
       |
HTTP :8080
       |
mk-piclock-api
libmicrohttpd, GUI, uploads, asset listings, ID3 metadata
       |
SOCK_SEQPACKET binary IPC
/run/mk-piclock/core.sock
       |
mk-piclock-core
OLED, touch GPIO, alarms, audio, messages, clock state
```

HTTP ends at `mk-piclock-api`. The core contains no HTTP, URL, query-string, multipart, route, or CORS parsing.

The API has no authentication and is intended for a trusted LAN. CORS remains disabled unless one exact origin is configured.

## v1.6.10 changes

- Keeps the core network model SSID-free: it stores and renders only a cached connected/disconnected Wi-Fi state.
- Keeps audio-thread completion synchronized with `pthread_cond_t`; no spin-wait or `usleep()` polling remains.
- Makes latency-sensitive audio stops request-only so IPC and touch responses are immediate.
- Uses `pthread_cond_timedwait()` only where completion is required, including track replacement and daemon shutdown.
- Makes delete-all-music notification nonblocking while the decoder closes its already-open file handle.

## v1.6.8 changes

- Adds TTP223B touch input on GPIO20, physical pin 38.
- A short press stops the current song.
- Holding for three seconds starts a random uploaded song.
- Releasing after a long press does not stop the newly started song.
- Song metadata now uses the same display path for alarm, GUI/API, and touch playback.
- When metadata display is enabled, starting any song returns to the clock screen and shows `Title - Artist`.
- Status now reports `touch_ok`, `touch_pressed`, and `touch_gpio`.

## v1.6.7 changes

- Replaces generic GUI notices with action-specific feedback on every page.
- Music actions now report the track being started, stopped, uploaded, or deleted.
- Face, display, alarm, message, and log actions now report their exact outcome.
- Adds the 16 MB GPU memory recommendation to `INSTALL.md`.

## v1.6.6 changes

- Reads ID3 title and artist fields from uploaded MP3 files.
- Music listings return filenames plus title, artist, display text, and ID3 availability.
- The OLED can show `Title - Artist` while a track plays.
- Long song metadata scrolls continuously in the OLED footer, pausing at each end.
- Song metadata display can be enabled or disabled from the Music module.
- Screen messages can be sent now or after 10, 30, or 60 seconds.
- The core holds one pending delayed message in memory. A newer delayed message replaces it.

## Modular GUI

```text
web/assets/css/master.css
web/assets/js/app.js
web/modules/modules.json
web/modules/<module>/module.html
web/modules/<module>/module.css
web/modules/<module>/module.js
```

Only the shell and manifest load at startup. A module's HTML, CSS, and JavaScript load only after its menu item is selected.

Set a module to `"enabled": false` in `web/modules/modules.json` to remove it from the menu and prevent loading.

## Build and install

```bash
sudo apt update
sudo apt install --no-install-recommends -y \
  build-essential pkg-config libgpiod-dev gpiod libpng-dev \
  libfreetype-dev libasound2-dev libmpg123-dev libmicrohttpd-dev \
  alsa-utils unzip curl

make clean
make -j2
make install
sudo systemctl restart mk-piclock-core mk-piclock-api
```

Open:

```text
http://<clock-ip>:8080/
```

See `INSTALL.md` for complete Raspberry Pi setup and `ADDON_API.md` for every public call.

## Runtime paths

```text
/opt/mk-piclock/mk-piclock-core
/opt/mk-piclock/mk-piclock-api
/opt/mk-piclock/web
/opt/mk-piclock/api/openapi-v1.json
/opt/mk-piclock/config
/opt/mk-piclock/assets
/run/mk-piclock/core.sock
```

Neither runtime service performs `exec()`, `system()`, `popen()`, `fork()`, `vfork()`, or `posix_spawn()`.
