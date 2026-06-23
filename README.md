# mk-piclock v1.6.19

Created for my daughter Rylie, mk-piclock is native C alarm clock software for a Raspberry Pi, SSD1322 256x64 OLED, and MAX98357A I2S audio.

Hardware wiring: see [`pinouts.md`](pinouts.md).

## Features

mk-piclock was designed as a practical bedroom clock for a child, not simply as a clock with alarms.

### Child-friendly clock

* Presents the time, date, alarm state, Images, and other information on a small, distraction-free OLED.
* Uses large clock text that can be read from across the room.
* Shows simple status indicators without filling the screen with technical information.
* Supports 12-hour and 24-hour clock formats.
* Displays the clock name so the device feels personal to the child using it.

### Bedtime behaviour

* Includes a scheduled bedtime mode designed to reduce the temptation to stay awake watching the clock.
* Dims the OLED during the configured bedtime period.
* Uses a separate Bedtime Images library so nighttime images can be calm and sleep-appropriate.
* Changes bedtime images less frequently than normal daytime images.
* Keeps bedtime behaviour separate from normal daytime display behaviour.
* Allows parents to control when bedtime mode starts and ends.
* Provides a simple way to quiet the clock when the child is awake or when a parent wants the room quiet.

### Touch control

* Uses a TTP223B touch sensor so the child can operate basic functions without opening the web interface.
* A single press stops the current song or alarm.
* A single press can be used when the child is awake or when a parent wants the clock quiet.
* Holding the touch sensor for three seconds starts a random uploaded song.
* Long-press music playback lets the clock act as the child's own small boom box.
* Releasing the sensor after a long press does not immediately stop the newly started song.
* Touch controls remain intentionally limited so the child cannot accidentally change alarms or configuration.

### Music and alarms

* Plays uploaded MP3 files through a MAX98357A I2S amplifier and speaker.
* Allows each alarm to use a selected or random song.
* Supports a configurable alarm-volume ramp.
* Displays the current song using ID3 title and artist metadata.
* Scrolls long song information while keeping shorter titles stationary.
* Allows music to be started and stopped from the GUI or touch sensor.
* Returns to the clock display when music starts.
* Keeps alarm playback, touch playback, and GUI playback on the same metadata and display path.

### Messages

* Supports immediate and delayed screen messages so Mom and Dad can schedule a message and create a little more magic.
* Allows a parent to send a message now or delay it by 10, 30, or 60 seconds.
* Allows a message to be paired with an Image.
* Returns to the normal clock display after the message period ends.
* Keeps one pending delayed message in memory.
* Replaces the pending delayed message when a newer one is scheduled.
* Lets parents surprise the child without needing to be in the room.

### Images

* Uses **Images** instead of the previous **Faces** terminology throughout the GUI.
* Displays normal Images during daytime operation.
* Uses a separate Bedtime Images library during bedtime mode.
* Supports PNG image uploads through the GUI.
* Keeps Images and Bedtime Images in separate folders and management pages.
* Supports image pagination, item counts, individual deletion, and delete-all controls.
* Allows parents to build image collections around routines, activities, encouragement, bedtime, or the child's interests.

### Parent controls

* Provides a browser-based GUI for configuring the clock from another device on the trusted LAN.
* Allows parents to manage alarms, music, Images, Bedtime Images, messages, display settings, and logs.
* Keeps detailed configuration out of the child's touch controls.
* Requires no cloud account or external management service.
* Keeps the API unauthenticated for trusted-LAN use only.

### Live Dashboard preview

* Shows a live browser preview that closely represents the physical OLED.
* Updates the preview once per second.
* Aligns preview updates with the system clock.
* Mirrors the clock layout, date, Image, status indicators, alarm state, and music information.
* Shows the selected Yellow, Green, or White display colour.
* Helps parents see what the child currently sees without entering the room.
* Pauses Dashboard polling when the Dashboard is not visible.

### Reliable appliance-style operation

* Separates the hardware-facing core from the HTTP API and GUI.
* Keeps HTTP, multipart uploads, routing, and browser handling outside the core process.
* Uses binary `SOCK_SEQPACKET` IPC between the API and core.
* Runs as two system services.
* Avoids shell execution and process spawning in both runtime services.
* Uses direct GPIO, SPI, ALSA, FreeType, PNG, and MP3 libraries.
* Keeps the core network model limited to a connected or disconnected Wi-Fi state.
* Does not require the core to know or display the Wi-Fi SSID.

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

## v1.6.19 changes

### Live Dashboard preview

* Adds a live Dashboard preview of the physical OLED screen.
* Updates the preview once per second.
* Aligns preview updates with the system clock.
* Makes the preview brighter and closer to the appearance of the physical OLED.
* Mirrors the current clock layout, date, Image, status indicators, alarm state, and music information.
* Displays playing-song metadata in the live preview.
* Scrolls `Title - Artist` only when the text exceeds the available width.
* Keeps song metadata stationary when it fits.
* Pauses Dashboard polling when the Dashboard is not visible.

### GUI changes

* Adds the clock name to the GUI header and browser title.
* Keeps the displayed clock name current after configuration changes.
* Adds Yellow, Green, and White display-colour options.
* Applies the selected display colour to the Dashboard, message, and font previews.
* Removes continuous status polling from configuration pages.
* Limits continuous status polling to the Dashboard.
* Prevents Alarm, Display, Music, Message, Images, Bedtime Images, and Log forms from being rebuilt while they are being edited.
* Fixes alarm-setting changes being interrupted or discarded by background GUI refreshes.
* Improves incomplete-update warnings by comparing the GUI, API, core, and IPC versions.

### Images

* Replaces the previous **Faces** terminology with **Images** throughout the GUI.
* Renames the **Faces** menu and page to **Images**.
* Renames bedtime faces to **Bedtime Images**.
* Gives Images and Bedtime Images separate menu entries and pages.
* Keeps Images and Bedtime Images as separate asset libraries.
* Adds independent upload controls for both image libraries.
* Adds pagination and item counts to both image pages.
* Adds individual delete and delete-all controls to both image pages.
* Stores normal Images in `/opt/mk-piclock/assets/images`.
* Stores Bedtime Images in `/opt/mk-piclock/assets/bedtime-images`.
* Removes obsolete face asset directories during installation.

### UTF-8 and music metadata

* Fixes UTF-8 song metadata handling.
* Processes multibyte UTF-8 characters as complete characters instead of individual bytes.
* Preserves supported accented ID3 title and artist characters instead of replacing them with `?`.
* Correctly converts supported UTF-8 metadata to uppercase for the OLED.
* Uses the same title and artist display path for alarm, GUI/API, and touch playback.

## v1.6.10 changes

* Keeps the core network model SSID-free: it stores and renders only a cached connected/disconnected Wi-Fi state.
* Keeps audio-thread completion synchronized with `pthread_cond_t`; no spin-wait or `usleep()` polling remains.
* Makes latency-sensitive audio stops request-only so IPC and touch responses are immediate.
* Uses `pthread_cond_timedwait()` only where completion is required, including track replacement and daemon shutdown.
* Makes delete-all-music notification nonblocking while the decoder closes its already-open file handle.

## v1.6.8 changes

* Adds TTP223B touch input on GPIO20, physical pin 38.
* A short press stops the current song.
* Holding for three seconds starts a random uploaded song.
* Releasing after a long press does not stop the newly started song.
* Song metadata now uses the same display path for alarm, GUI/API, and touch playback.
* When metadata display is enabled, starting any song returns to the clock screen and shows `Title - Artist`.
* Status now reports `touch_ok`, `touch_pressed`, and `touch_gpio`.

## v1.6.7 changes

* Replaces generic GUI notices with action-specific feedback on every page.
* Music actions now report the track being started, stopped, uploaded, or deleted.
* Image, display, alarm, message, and log actions now report their exact outcome.
* Adds the 16 MB GPU memory recommendation to `INSTALL.md`.

## v1.6.6 changes

* Reads ID3 title and artist fields from uploaded MP3 files.
* Music listings return filenames plus title, artist, display text, and ID3 availability.
* The OLED can show `Title - Artist` while a track plays.
* Long song metadata scrolls continuously in the OLED footer, pausing at each end.
* Song metadata display can be enabled or disabled from the Music module.
* Screen messages can be sent now or after 10, 30, or 60 seconds.
* The core holds one pending delayed message in memory. A newer delayed message replaces it.

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
/opt/mk-piclock/assets/images
/opt/mk-piclock/assets/bedtime-images
/run/mk-piclock/core.sock
```

Neither runtime service performs `exec()`, `system()`, `popen()`, `fork()`, `vfork()`, or `posix_spawn()`.

## Known issues and caveats

### Speaker pop

* The MAX98357A amplifier may produce a small pop or click when audio starts or stops.
* This is caused by the amplifier and audio path changing state.
* The project intentionally avoids complex software muting or enable-pin sequencing that could make alarm playback less reliable.
* The pop does not normally indicate a damaged speaker or amplifier.

### USB-C power

* The enclosure uses a USB-C connector for power, but this does not make the clock a USB Power Delivery device.
* The USB-C connection is used only to provide standard 5 V power.
* A USB-A to USB-C cable is recommended.
* A USB-A power source normally supplies 5 V directly through the cable without USB-C Power Delivery negotiation.
* Some USB-C to USB-C chargers will not enable their output unless they detect the correct USB-C configuration-channel resistors.
* A basic USB-C power insert or breakout may not provide those configuration resistors.
* As a result, a USB-C to USB-C cable may provide no power even though the charger and cable work with other devices.
* USB-A to USB-C avoids this issue and is the most predictable option for this build.
* Use a stable 5 V supply with enough current for the Raspberry Pi, OLED, amplifier, and speaker.
* Poor power supplies or thin cables may cause reboots, audio distortion, Wi-Fi instability, or undervoltage warnings.
* The USB-C connector is for power only and does not provide USB data access to the Raspberry Pi.

### Trusted-LAN access

* The web GUI and API do not include user authentication.
* The clock should be connected only to a trusted home or private network.
* Port `8080` should not be forwarded to the public Internet.
* CORS remains disabled unless one exact trusted origin is configured.

### Timekeeping

* Alarm accuracy depends on the Raspberry Pi system clock.
* The clock should have network access so the operating system can synchronize its time.
* A Raspberry Pi without a real-time clock may start with an incorrect time after losing power until network time synchronization completes.
* Alarms should be checked after extended power or network outages.

### Wi-Fi

* The core tracks only whether Wi-Fi is connected.
* It does not store or display the SSID.
* The clock and alarms continue to run without Wi-Fi once the system time is correct.
* The web GUI will be unavailable while the clock is disconnected from the network.

### OLED behaviour

* Yellow, Green, and White are GUI preview options.
* They do not change the physical colour of a fixed-colour OLED panel.
* Physical brightness and low-level dimming vary between OLED modules.
* The lowest configured brightness may still be visible in a very dark room.
* OLED panels can develop uneven wear if the same bright content remains on-screen for long periods.
* Bedtime dimming, changing Images, and the blinking clock colon help reduce static display time.

### Touch sensitivity

* TTP223B sensitivity depends on module tolerances, wiring, grounding, supply voltage, and the thickness of material above the sensor.
* The enclosure is designed to keep approximately 1 mm of plastic between the touch area and sensor.
* Thick paint, tape, adhesive, or an air gap may reduce sensitivity.
* Long sensor wires can increase false triggers or reduce reliability.
* The touch input should be tested before the enclosure is fully assembled.

### Audio and MP3 files

* MP3 volume varies between files because uploaded songs may have different mastering levels.
* The configured volume ramp cannot fully compensate for unusually quiet or loud source files.
* ID3 metadata quality depends on the tags stored in each MP3.
* Files without usable title or artist tags fall back to filename-based display information.
* Very unusual Unicode characters may not be available in the selected OLED font.

### Messages and delayed actions

* Only one delayed screen message is stored at a time.
* Scheduling another delayed message replaces the previous pending message.
* Pending delayed messages are held in memory and do not survive a service restart or power loss.
* The 10, 30, and 60 second options are short delays intended for nearby parent interaction, not long-term scheduling.

### Local storage

* Music, Images, Bedtime Images, fonts, and configuration are stored on the Raspberry Pi.
* The clock does not automatically back up its assets to a cloud service.
* A damaged or reformatted microSD card may result in lost configuration and uploaded content.
* Keep a copy of important assets and configuration outside the clock.

### Hardware-specific build

* GPIO assignments, SPI settings, I2S audio, and enclosure dimensions are specific to the documented hardware.
* Changing the OLED, amplifier, touch module, Raspberry Pi model, or wiring may require source, configuration, and enclosure changes.
* Confirm all wiring against `pinouts.md` before applying power.
