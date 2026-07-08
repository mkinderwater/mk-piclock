# mk-piclock Release Notes Since Touch Lighting

Current release: v1.7.1  
HTTP API: 1.24  
Private IPC: 15

## v1.7.1 - MP3 Queue Cleanup

This release keeps the touch-sensor LED feedback and batch MP3 upload feature, but cleans up the MP3 queue internals.

### Changes

- Replaced the per-file MP3 queue loop with a cleaner atomic batch queue.
- Validates the full MP3 selection before queueing any jobs.
- Stages all selected MP3 files before committing the batch.
- Rolls back staged files if the batch cannot be queued.
- Keeps new uploads blocked while any MP3 job is queued or processing.
- Adds selected-song count and total upload size feedback on the Music page.
- Keeps HTTP API at 1.24.
- Keeps private IPC at 15.

## v1.7.0 - Touch Lighting and Batch MP3 Uploads

This release introduced touch-sensor RGB LED feedback and multi-file MP3 uploads.

### Changes

- Pressing the TTP223B touch sensor now blinks the RGB LED.
- Touch blink defaults to white.
- Touch blink colour and brightness are configurable under Lighting, Global Controls.
- Added a high-priority temporary touch lighting scene.
- Added batch MP3 upload support on the Music page.
- Blocks new MP3 uploads until all queued and processing jobs are complete.
- Added shared browser-side MP3 metadata helper for Music and Stories.
- Audited installer and runtime directory creation.
- Confirmed no obsolete or unused directories are created.
- Increased HTTP API to 1.24.
- Kept private IPC at 15.

## Hardware Notes

RGB LED mapping:

| Channel | GPIO | Physical pin |
| --- | ---: | ---: |
| Red | GPIO5 | 29 |
| Green | GPIO6 | 31 |
| Blue | GPIO13 | 33 |
| Common cathode | GND | 30 |

Touch sensor mapping:

| Signal | GPIO | Physical pin |
| --- | ---: | ---: |
| OUT / SIG | GPIO20 | 38 |

The RGB LED is common-cathode. Each colour channel needs its own resistor.
