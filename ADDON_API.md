# mk-piclock v1.6.10 Add-on API

The public interface is HTTP under `/api/v1`, normally on port `8080`.

Use it for the built-in GUI, scripts, home automation, and third-party add-ons. Do not connect directly to `/run/mk-piclock/core.sock`. That socket uses the private binary IPC protocol shared by the installed API and core services.

## Base URL

```bash
BASE="http://rylie-clock:8080/api/v1"
```

There is no API authentication. The service is intended for a trusted LAN. CORS is disabled by default, so browser code hosted on another origin cannot call it unless the API service is configured with one allowed origin.

## Request formats

- `GET` calls use query parameters.
- Normal `POST` calls use `application/x-www-form-urlencoded`.
- Upload calls use `multipart/form-data`.
- JSON request bodies are not accepted.
- Non-file responses are JSON.

Use URL encoding for user text and filenames:

```bash
curl -s --data-urlencode 'message_text=Good morning, Rylie!' \
  "$BASE/display/message"
```

## Complete call list

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1` | API discovery |
| GET | `/api/v1/capabilities` | Feature names for add-on detection |
| GET | `/api/v1/openapi.json` | OpenAPI 3.1 contract |
| GET | `/api/v1/health` | Check API-to-core communication |
| GET | `/api/v1/status` | Full clock state |
| GET | `/api/v1/assets/faces` | List normal or bedtime faces |
| GET | `/api/v1/assets/faces/source` | Download a source face PNG |
| POST | `/api/v1/assets/faces/upload` | Upload normal face PNG files |
| POST | `/api/v1/assets/faces/bedtime/upload` | Upload bedtime face PNG files |
| POST | `/api/v1/assets/faces/delete` | Delete one face |
| POST | `/api/v1/assets/faces/delete-all` | Delete all faces |
| GET | `/api/v1/assets/fonts` | List built-in and uploaded fonts |
| GET | `/api/v1/assets/fonts/file` | Download an uploaded font |
| POST | `/api/v1/assets/fonts/upload` | Upload one TTF or OTF font |
| POST | `/api/v1/assets/fonts/delete` | Delete one uploaded font |
| GET | `/api/v1/assets/music` | List MP3 files and ID3 metadata |
| POST | `/api/v1/assets/music/upload` | Upload MP3 files |
| POST | `/api/v1/assets/music/delete-all` | Delete all uploaded music |
| POST | `/api/v1/display/action` | Show clock, clear, stop, or play music |
| POST | `/api/v1/display/face` | Preview a face |
| POST | `/api/v1/display/message` | Show or schedule a message |
| GET | `/api/v1/display/message/limits` | Read message layout limits |
| GET | `/api/v1/display/message/fit` | Test message wrapping and fit |
| POST | `/api/v1/config/alarms` | Configure one alarm slot |
| POST | `/api/v1/config/audio` | Set volume and song-metadata display |
| POST | `/api/v1/config/personalization` | Set the clock name |
| POST | `/api/v1/config/display` | Set font, clock mode, and bedtime options |
| GET | `/api/v1/logs` | Read recent event-log entries |
| POST | `/api/v1/logs/clear` | Clear the event log |

# Discovery and state

## GET `/api/v1`

Returns the public versions and discovery links.

```bash
curl -s "$BASE"
```

Example:

```json
{
  "name": "mk-piclock API",
  "api_version": "1.0",
  "product_version": "1.6.10",
  "http_engine": "libmicrohttpd",
  "core_protocol": "binary-ipc-v4",
  "status": "/api/v1/status",
  "capabilities": "/api/v1/capabilities",
  "openapi": "/api/v1/openapi.json"
}
```

## GET `/api/v1/capabilities`

Use this call instead of assuming an installed feature.

```bash
curl -s "$BASE/capabilities"
```

Current capability names:

```text
status.read
display.control
display.message
display.message.delay
alarm.configure
audio.configure
audio.metadata
touch.input
assets.read
assets.upload
assets.delete
logs.read
```

## GET `/api/v1/openapi.json`

```bash
curl -s "$BASE/openapi.json" -o openapi-v1.json
```

## GET `/api/v1/health`

Checks whether the API can contact the core. It currently returns the same state document as `/status`.

```bash
curl -s "$BASE/health"
```

HTTP `503` means the API is running but the core is unavailable.

## GET `/api/v1/status`

```bash
curl -s "$BASE/status"
```

Important fields:

| Field | Meaning |
| --- | --- |
| `time`, `date` | Formatted local time and date |
| `clock_name` | Configured clock name |
| `app_version` | Core version |
| `uptime_seconds` | Core process uptime |
| `audio_file` | Current or most recent MP3 filename |
| `audio_title` | Current ID3 title, or filename-derived title |
| `audio_artist` | Current ID3 artist, or empty |
| `audio_display` | Text used on the OLED, normally `Title - Artist` |
| `audio_playing` | `1` while audio is playing |
| `global_volume` | Global volume, 0 to 100 |
| `show_song_metadata` | `1` when OLED metadata is enabled |
| `touch_ok` | `1` when the GPIO20 touch input initialized |
| `touch_pressed` | `1` while the sensor output is active |
| `touch_gpio` | Touch input BCM GPIO number, currently `20` |
| `message_pending` | `1` when a delayed message is queued |
| `message_send_in_seconds` | Remaining delay for the queued message |
| `alarm_active` | `1` while an alarm is active |
| `alarm_volume_percent` | Current ramped alarm volume |
| `display_mode` | Current display mode |
| `oled_ok` | `1` when the OLED initialized |
| `oled_brightness_percent` | Effective brightness |
| `current_face` | Current numeric fallback face ID |
| `oled_font` | Built-in font ID, 0 to 3 |
| `oled_font_size` | TrueType pixel size |
| `oled_font_file` | Selected uploaded font filename |
| `oled_font_name` | Active font display name |
| `clock_24h_mode` | `1` for 24-hour mode |
| `bedtime_enabled` | `1` when bedtime scheduling is enabled |
| `bedtime_active` | `1` during the current bedtime period |
| `bedtime_start_hour`, `bedtime_start_min` | Bedtime start |
| `bedtime_end_hour`, `bedtime_end_min` | Bedtime end |
| `bedtime_dim_percent` | Bedtime brightness |
| `alarms` | Seven alarm-slot objects |

### Touch behavior

The TTP223B touch sensor is handled directly by the core on BCM GPIO20, physical pin 38:

- A normal press stops audio when a song is playing.
- Holding the sensor for three seconds starts a random uploaded MP3.
- The long-press action fires once while held. Releasing it does not stop the new song.
- Touch playback uses the same ID3 title and artist display as alarms and API music playback.

Each alarm object contains:

```json
{
  "id": 1,
  "enabled": 1,
  "hour": 7,
  "min": 0,
  "weekdays": 62,
  "start_volume": 20,
  "end_volume": 80,
  "music_file": "wake-up.mp3"
}
```

`weekdays` is a bit mask:

| Day field | Bit value | Day |
| --- | ---: | --- |
| `day0` | 1 | Sunday |
| `day1` | 2 | Monday |
| `day2` | 4 | Tuesday |
| `day3` | 8 | Wednesday |
| `day4` | 16 | Thursday |
| `day5` | 32 | Friday |
| `day6` | 64 | Saturday |

# Face assets

## GET `/api/v1/assets/faces`

Parameters:

| Name | Values | Default |
| --- | --- | --- |
| `kind` | `normal`, `bedtime` | `normal` |
| `page` | Integer 1 or higher | `1` |
| `all` | `0`, `1` | `0` |

```bash
curl -s "$BASE/assets/faces?kind=normal&page=1"
curl -s "$BASE/assets/faces?kind=bedtime&all=1"
```

Response fields include `kind`, `page`, `max_page`, `per_page`, `count`, and `faces`. Each face includes:

```json
{
  "id": 1,
  "file": "happy-frame1.raw",
  "title": "Happy Frame1",
  "source_png": "happy-frame1.png",
  "preview_url": "/api/v1/assets/faces/source?kind=normal&file=happy-frame1.raw",
  "source_exists": 1,
  "exists": 1
}
```

## GET `/api/v1/assets/faces/source`

Required: `file`

Optional: `kind=normal` or `kind=bedtime`

```bash
curl -f "$BASE/assets/faces/source?kind=normal&file=happy-frame1.raw" \
  -o happy-frame1.png
```

## POST `/api/v1/assets/faces/upload`

Upload one or more normal PNG files. The multipart field name is `files`.

```bash
curl -s -H 'Accept: application/json' \
  -F 'files=@happy-frame1.png' \
  -F 'files=@happy-frame2.png' \
  "$BASE/assets/faces/upload"
```

## POST `/api/v1/assets/faces/bedtime/upload`

```bash
curl -s -H 'Accept: application/json' \
  -F 'files=@sleeping-frame1.png' \
  "$BASE/assets/faces/bedtime/upload"
```

Face upload limits:

```text
8 MiB per PNG
4096 x 4096 maximum dimensions
16,777,216 maximum pixels
```

The API stores the source PNG and creates the 64x64, four-bit OLED RAW file.

## POST `/api/v1/assets/faces/delete`

Fields:

| Name | Required | Values |
| --- | --- | --- |
| `file` | Yes | Safe `.raw` filename |
| `kind` | No | `normal` or `bedtime` |

```bash
curl -s \
  --data-urlencode 'file=happy-frame1.raw' \
  --data 'kind=normal' \
  "$BASE/assets/faces/delete"
```

## POST `/api/v1/assets/faces/delete-all`

```bash
curl -s -X POST "$BASE/assets/faces/delete-all"
```

# Font assets

## GET `/api/v1/assets/fonts`

```bash
curl -s "$BASE/assets/fonts"
```

Returns the selected font, built-in font ID, font size, built-in font names, and uploaded font filenames.

## GET `/api/v1/assets/fonts/file`

```bash
curl -f "$BASE/assets/fonts/file?file=Clock.ttf" -o Clock.ttf
```

## POST `/api/v1/assets/fonts/upload`

Upload one TTF or OTF file using field `file`.

```bash
curl -s -H 'Accept: application/json' \
  -F 'file=@Clock.ttf' \
  "$BASE/assets/fonts/upload"
```

Maximum uploaded font size: 16 MiB. The API verifies that FreeType can open it.

## POST `/api/v1/assets/fonts/delete`

```bash
curl -s --data-urlencode 'font=Clock.ttf' \
  "$BASE/assets/fonts/delete"
```

# Music assets and ID3

## GET `/api/v1/assets/music`

```bash
curl -s "$BASE/assets/music"
```

The response keeps the legacy filename array and adds parsed track objects:

```json
{
  "global_volume": 80,
  "current": "wake-up.mp3",
  "files": ["wake-up.mp3"],
  "tracks": [
    {
      "file": "wake-up.mp3",
      "title": "Wake Up Song",
      "artist": "Rylie Artist",
      "display": "Wake Up Song - Rylie Artist",
      "id3": 1
    }
  ]
}
```

`id3` is `1` when a supported ID3 tag supplied title or artist data. When title is absent, the API derives it from the filename. Supported metadata includes common ID3v2.2, ID3v2.3, ID3v2.4, and ID3v1 title and artist fields.

The core reads metadata again when playback starts. If song metadata is enabled, `Title - Artist` replaces the OLED date footer while the track plays. Text wider than the available footer loops continuously, with a short pause at each end.

## POST `/api/v1/assets/music/upload`

Use one or more multipart fields named `files`:

```bash
curl -s -H 'Accept: application/json' \
  -F 'files=@wake-up.mp3' \
  -F 'files=@weekend.mp3' \
  "$BASE/assets/music/upload"
```

Maximum size: 64 MiB per MP3. The API verifies that libmpg123 can decode the file. Disk-reserve and total music-quota settings may reject an otherwise valid file.

## POST `/api/v1/assets/music/delete-all`

```bash
curl -s -X POST "$BASE/assets/music/delete-all"
```

# Display and audio actions

## POST `/api/v1/display/action`

Field `do` accepts:

| `do` | Additional field | Result |
| --- | --- | --- |
| `clock` | None | Return to the clock display |
| `clear` | None | Clear the OLED |
| `stop` | None | Stop active audio |
| `play-music` | `file` | Play one uploaded MP3 |

```bash
curl -s --data 'do=clock' "$BASE/display/action"
curl -s --data 'do=clear' "$BASE/display/action"
curl -s --data 'do=stop' "$BASE/display/action"
curl -s --data 'do=play-music' \
  --data-urlencode 'file=wake-up.mp3' \
  "$BASE/display/action"
```

Starting music populates `audio_title`, `audio_artist`, and `audio_display` in `/status`.

## POST `/api/v1/display/face`

Supply `file`, `id`, or both. A valid uploaded RAW filename is preferred. Numeric `id` is a fallback.

```bash
curl -s --data-urlencode 'file=happy-frame1.raw' \
  "$BASE/display/face"

curl -s --data 'id=3' "$BASE/display/face"
```

# Screen messages

## POST `/api/v1/display/message`

Fields:

| Name | Required | Meaning |
| --- | --- | --- |
| `message_text` | Yes | Message text, up to 180 input characters and subject to OLED fit validation |
| `face_file` | No | Uploaded RAW face filename |
| `face_id` | No | Numeric fallback face ID |
| `delay_seconds` | No | `0`, `10`, `30`, or `60`; default `0` |

Send now:

```bash
curl -s \
  --data-urlencode 'message_text=Time for school' \
  --data-urlencode 'face_file=happy-frame1.raw' \
  --data 'delay_seconds=0' \
  "$BASE/display/message"
```

Send after 30 seconds:

```bash
curl -s \
  --data-urlencode 'message_text=Dinner is ready' \
  --data 'delay_seconds=30' \
  "$BASE/display/message"
```

An immediate message is displayed for 30 seconds and cancels any pending delayed message. The core stores one pending delayed message in memory. Scheduling another delayed message replaces the existing pending message. A pending message is lost if the core restarts.

Immediate response:

```json
{"ok":true,"mode":"message","delay_seconds":0}
```

Scheduled response includes the delay and activation time:

```json
{
  "ok": true,
  "mode": "scheduled-message",
  "delay_seconds": 30,
  "scheduled_for": 1781900000
}
```

Use `/status` to read `message_pending` and `message_send_in_seconds`.

Compatibility aliases `message` for `message_text` and `file` for `face_file` are accepted but should not be used in new add-ons.

## GET `/api/v1/display/message/limits`

```bash
curl -s "$BASE/display/message/limits"
```

Returns current limits such as `max_chars`, `advisory_chars`, `max_lines`, text width, and display duration.

## GET `/api/v1/display/message/fit`

Required query parameter: `text`

```bash
curl -sG --data-urlencode 'text=Time for school' \
  "$BASE/display/message/fit"
```

Returns `ok`, line count, wrapped lines, layout coordinates, and a rejection reason when the message does not fit.

# Configuration

## POST `/api/v1/config/alarms`

Fields:

| Name | Required | Range or meaning |
| --- | --- | --- |
| `id` | Yes | Alarm slot 1 to 7 |
| `time` | Yes | `HH:MM`, 24-hour form |
| `enabled` | No | `0` or `1` |
| `day0` through `day6` | No | Include a field to select that day |
| `start_volume` | No | 0 to 100, default 20 |
| `end_volume` | No | 0 to 100, default 80 |
| `music_file` | No | Uploaded MP3 filename; empty allows random selection |

Weekday alarm example:

```bash
curl -s -H 'Accept: application/json' \
  --data 'id=1' \
  --data 'enabled=1' \
  --data 'time=07:00' \
  --data 'day1=1' --data 'day2=1' --data 'day3=1' \
  --data 'day4=1' --data 'day5=1' \
  --data 'start_volume=20' \
  --data 'end_volume=80' \
  --data-urlencode 'music_file=wake-up.mp3' \
  "$BASE/config/alarms"
```

## POST `/api/v1/config/audio`

Supply one or both fields:

| Name | Values | Meaning |
| --- | --- | --- |
| `global_volume` | 0 to 100 | Normal playback volume |
| `show_song_metadata` | `0`, `1` | Show ID3 title and artist while music plays |

```bash
curl -s \
  --data 'global_volume=70' \
  --data 'show_song_metadata=1' \
  "$BASE/config/audio"
```

Disable the OLED metadata line while retaining ID3 in API responses:

```bash
curl -s --data 'show_song_metadata=0' "$BASE/config/audio"
```

## POST `/api/v1/config/personalization`

```bash
curl -s --data-urlencode 'clock_name=Rylie Bear' \
  "$BASE/config/personalization"
```

Maximum clock-name length: 63 characters.

## POST `/api/v1/config/display`

All fields are optional. Supply only those being changed.

| Name | Values |
| --- | --- |
| `oled_font` | Built-in font ID 0 to 3 |
| `oled_font_size` | 18 to 54 |
| `oled_font_file` | Uploaded TTF/OTF filename, or empty to clear |
| `clock_24h_mode` | `0` or `1` |
| `bedtime_enabled` | `0` or `1` |
| `bedtime_dim_percent` | 0 to 100 |
| `bedtime_start` | `HH:MM` |
| `bedtime_end` | `HH:MM` |

```bash
curl -s \
  --data 'clock_24h_mode=0' \
  --data 'bedtime_enabled=1' \
  --data 'bedtime_start=20:30' \
  --data 'bedtime_end=07:00' \
  --data 'bedtime_dim_percent=10' \
  "$BASE/config/display"
```

Select an uploaded font:

```bash
curl -s \
  --data-urlencode 'oled_font_file=Clock.ttf' \
  --data 'oled_font_size=48' \
  "$BASE/config/display"
```

# Logs

## GET `/api/v1/logs`

```bash
curl -s "$BASE/logs"
```

Returns recent structured event entries from the core log.

## POST `/api/v1/logs/clear`

```bash
curl -s -X POST "$BASE/logs/clear"
```

# Common responses

Successful modifying calls normally return:

```json
{"ok":true}
```

Errors normally return:

```json
{"ok":false,"error":"description"}
```

Common status codes:

| Code | Meaning |
| ---: | --- |
| 200 | Success |
| 303 | Browser-form redirect when JSON was not requested |
| 400 | Invalid field, filename, media, or message fit |
| 404 | Route or requested file not found |
| 405 | Method not allowed |
| 413 | Request or upload too large |
| 414 | URI too long |
| 500 | API or storage failure |
| 503 | Core daemon unavailable or incompatible |
| 507 | Insufficient storage |

For upload and configuration scripts, send:

```text
Accept: application/json
```

This prevents browser-style `303` redirects and asks for a direct JSON success response.

# Add-on rules

- Use `/api/v1/capabilities` before relying on optional features.
- Treat unknown JSON fields as forward-compatible additions.
- Do not depend on the private Unix socket or binary IPC layout.
- Do not send JSON request bodies.
- URL encode filenames and user text.
- Use POST only for state changes.
- Poll `/status` moderately. One request every second is sufficient for a live dashboard.
- Use the `tracks` array for ID3-aware music interfaces, but retain support for `files` when targeting older releases.
