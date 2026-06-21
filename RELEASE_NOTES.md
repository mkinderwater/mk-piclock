# mk-piclock v1.6.10 Release Notes

Release package: `mk-piclock-v1.6.10-network-status.zip`

## Changes from v1.6.9

- The core tracks Wi-Fi as connected or disconnected only. It does not read, store, or expose an SSID.
- Audio-thread completion uses `pthread_cond_t` and `pthread_cond_timedwait()`; no `usleep()` spin-wait remains.
- Normal IPC and touch stop requests return immediately.
- Track replacement and daemon shutdown still wait efficiently for decoder completion when required.
- Deleting all music no longer holds the core IPC listener while the current decoder exits.

## Compatibility

- Public API remains v1.0.
- Private core IPC remains binary IPC v4.
- Existing configuration and asset directories are unchanged.

## Packaging documentation update

- Added `pinouts.md` with the complete Raspberry Pi GPIO, SSD1322 OLED, MAX98357A amplifier, speaker, and TTP223B touch wiring.

- Final MAX98357A wiring leaves SD / EN unconnected and keeps DIN on GPIO21; the small playback-start click is accepted without extra mute or persistent-stream logic.
- Documents GPIO-header system power on physical pins 4 and 9, with separate amplifier power connectors on pins 2 and 14.
