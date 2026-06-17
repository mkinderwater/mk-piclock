# mk-piclock Project Description

## Overview

**mk-piclock** is a Raspberry Pi based alarm clock project built around a small grayscale screen, speaker, web GUI, and custom face/message system.

It is designed as a kid-friendly bedside clock that can show the time, play alarms, display expressive face sprites, show short screen messages, and manage everything from a phone or browser.

## Core Hardware

- Raspberry Pi, likely Pi Zero or Pi Zero 2 W
- 256x64 grayscale OLED screen using SSD1322 over SPI
- MAX98357A I2S audio amplifier and speaker output
- Local web interface served by the clock daemon
- Custom 3D printed enclosure

## Main Software

The current build is a native C daemon named:

```text
mk-piclock-c
```

It handles:

- OLED drawing
- Alarm scheduling
- MP3 playback
- Web GUI
- Asset uploads
- Message display
- Logging
- Config saving
- Hardware control

The GUI files live under:

```bash
/opt/mk-piclock/web/
```

The config and logs live under:

```bash
/opt/mk-piclock/config/
```

## Main Features

- Large clock display
- Alarm setup with per-day options
- MP3 alarm music uploads
- Multiple MP3 uploads at once
- Random music support
- Volume ramping
- Face sprite display
- Bedtime-safe face mode
- Screen Message page
- Phone-friendly web interface
- Live log page in the GUI
- Uploaded font support
- Backup and restore style config workflow
- Cached assets for better long-term performance

## Screen Message Feature

The **Screen Message** page lets a user select a face and type a short message directly into a screen-shaped preview.

The preview reflects the real screen layout:

- Face on the left
- Message on the right
- Text vertically centered
- Text wraps into up to 3 centered lines

This lets someone send a quick note to the clock from a phone.

## Current State

The latest package is:

```text
mk-piclock-v1.5.23-final-optimization
```

It is a good freeze point.

Recent improvements include:

- HTTP worker limit
- No stalled-client GUI freeze
- Atomic config saves
- RAM face cache
- Cached face, music, and font lists
- No repeated OLED heap allocation
- Warning-clean compile
- Cleaner route handling
- Mobile GUI polish

## Plain-Language Summary

**mk-piclock** is now a stable, self-contained Raspberry Pi alarm clock appliance with a phone-friendly control panel, custom visuals, MP3 alarms, logging, and a polished screen message system.
