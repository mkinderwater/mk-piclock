# mk-piclock HTTP API 1.25

The GUI and add-ons use the HTTP API on port 8080.

```text
http://<clock-ip>:8080/api/v1
```

Most routes are directly available when no password is configured. When a password exists, authenticate through `/api/v1/auth/login`; the API returns a session cookie used by the GUI. The password is stored as plain text and is intended only for a trusted home network. Do not expose port 8080 to the internet.

## Optional password

```http
GET  /api/v1/auth/status
POST /api/v1/auth/login
POST /api/v1/auth/password
```

`GET /api/v1/auth/status` reports whether a password exists and whether the current browser cookie is valid. `POST /api/v1/auth/login` accepts `password`. `POST /api/v1/auth/password` sets a new password or removes it when `password` is empty. The discovery endpoint and the two status/login routes remain public so the GUI can decide whether to show the prompt.

## Discovery and status

```http
GET /api/v1
GET /api/v1/capabilities
GET /api/v1/openapi.json
GET /api/v1/health
GET /api/v1/status
GET /api/v1/diagnostics
GET /api/v1/diagnostics/report
```

## Images

```http
GET  /api/v1/assets/images
GET  /api/v1/assets/bedtime-images
GET  /api/v1/assets/images/source?file=<raw-name>
GET  /api/v1/assets/bedtime-images/source?file=<raw-name>
GET  /api/v1/assets/images/download
GET  /api/v1/assets/bedtime-images/download
POST /api/v1/assets/images/upload
POST /api/v1/assets/bedtime-images/upload
POST /api/v1/assets/images/delete
POST /api/v1/assets/bedtime-images/delete
POST /api/v1/assets/images/delete-all
POST /api/v1/assets/bedtime-images/delete-all
```

The Day and Bedtime download routes return ZIP archives containing only original PNG files.

## Music

```http
GET  /api/v1/assets/music
POST /api/v1/assets/music/upload
GET  /api/v1/assets/music/jobs
POST /api/v1/assets/music/jobs/clear
POST /api/v1/assets/music/delete
POST /api/v1/assets/music/delete-all
```

MP3 upload settings include bitrate, sample rate, and low-pass frequency. One request may include 1 to 32 MP3 files. The full selection is validated and queued as one batch, and uploads are refused while any job is queued or processing. `POST /api/v1/assets/music/jobs/clear` removes waiting jobs but does not interrupt the active transcoder.

## Display and messages

```http
POST /api/v1/display/action
GET  /api/v1/display/preview
POST /api/v1/display/brightness-preview
POST /api/v1/display/message
POST /api/v1/display/message/preview
```

`POST /api/v1/display/message/preview` accepts `image_file`, `image_bedtime`, and `message_text`, then returns the exact packed 4-bit 256x64 framebuffer without changing the physical OLED.

Messages accept:

```text
image_file=<image raw filename, optional; empty selects a random image>
image_bedtime=0 for Day Images or 1 for Bedtime Images
message_text=<optional text; an image-only message is allowed>
delay_seconds=0, 10, 30, or 60
scheduled_at=<Unix timestamp, optional, within 30 days>
notification_sound=0 or 1; 1 plays the built-in chime when displayed
```

`GET /api/v1/diagnostics` returns release and compile information, network and NTP state, platform, device-identity, and operating-system details, used and available storage, asset directory sizes and file counts, root and boot filesystem details, SD-card identity, temperature, core/API, OLED, touch, and alarm health. `GET /api/v1/diagnostics/report` downloads the same information as text. Diagnostics are gathered in-process from Linux APIs and files under `/etc`, `/proc`, and `/sys`; the API does not launch system commands.

## Backup, restore, and reset

```http
GET  /api/v1/backup/download
POST /api/v1/backup/restore
POST /api/v1/factory-reset
```

Restore expects one multipart ZIP created by the download route. Backup ZIPs contain configuration, images, and fonts but exclude uploaded music and stories. Music and stories are outside the backup and restore system, so restore leaves both current libraries unchanged. Factory reset requires `confirm=RESET`. Restore and reset are refused while an alarm is active. Factory reset is also refused while music is processing.

## Configuration

```http
POST /api/v1/config/alarms
POST /api/v1/config/audio
POST /api/v1/config/personalization
POST /api/v1/config/display
POST /api/v1/config/led
```

Display configuration accepts `bedtime_music_enabled=0` to block touch-started and browser-started music during bedtime. Alarms remain enabled.

## Alarm behaviour

Alarms loop until the physical touch sensor is pressed or the 30-minute safety limit is reached. Browser stop requests return HTTP 409 while an alarm is active. If the selected MP3 is missing or unreadable, the protected built-in alarm is used.

`GET /api/v1/status` includes `next_alarm_at`, `next_alarm_text`, `last_successful_alarm`, and `bedtime_music_enabled`.


## RGB lighting

Save one activity profile:

```http
POST /api/v1/config/led
Content-Type: application/x-www-form-urlencoded

scene=alarm&effect=fade&brightness=100&color1=%23ff0000&color2=%23ffa000
```

Valid scenes are `alarm`, `bedtime`, `message`, `music`, `daytime`, and `stories`. Valid effects are `solid`, `fade`, and `rainbow`. Brightness is 0 through 100. Colours use `#RRGGBB`.

Preview unsaved settings for up to 30 seconds:

```http
POST /api/v1/led/preview
Content-Type: application/x-www-form-urlencoded

scene=music&effect=rainbow&brightness=70&color1=%23ff0000&color2=%230000ff&hold_seconds=10
```

`GET /api/v1/status` includes `led_ok`, `led_scene`, `led_colour`, `led_output`, `led_gpio`, PWM diagnostics, global calibration settings, preview state, bedtime-fade state, and all six `led_profiles`.

Activity priority is Alarm, Message, Stories, Music, Bedtime, then Daytime. Alarm lighting cannot be overridden by a preview.

`POST /api/v1/config/led-global` saves the master switch, brightness ceiling, RGB gains, idle-off option, and bedtime-fade duration. `POST /api/v1/config/led` also accepts `cycle_seconds` from 2 to 60. Preview requests may set `bypass_master=1`; direct wiring tests also use `raw_output=1` to bypass calibration and limits.

## Activity

```http
GET  /api/v1/logs
POST /api/v1/logs/clear
```

## Compatibility

```text
HTTP API:    1.25
Private IPC: 16
```

See `api/openapi-v1.json` for the complete schema.


## Story Mode

- `GET /api/v1/assets/stories` lists story MP3 files and Story Mode settings.
- `POST /api/v1/assets/stories/upload` uploads one validated MP3 into the separate Stories library.
- `POST /api/v1/assets/stories/delete` deletes one story.
- `POST /api/v1/assets/stories/delete-all` clears the story library.
- `POST /api/v1/display/action` with `do=play-story` plays a selected story or a random story when `file` is blank.
- `POST /api/v1/config/audio` accepts `story_enabled`, `story_volume`, and `story_message`.

Story Mode must be enabled before a story can be started. Ten short touch presses within eight seconds start a random story. An eight-second hold opens the OLED network diagnostic screen and is not counted as a Story Mode tap.
