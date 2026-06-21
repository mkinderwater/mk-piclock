# mk-piclock v1.6.10 Pinouts

This document covers the Raspberry Pi GPIO header, SSD1322 OLED, MAX98357A I2S amplifier, speaker, and TTP223B touch sensor used by mk-piclock.

The software uses **BCM GPIO numbering**. Physical pin numbers refer to the Raspberry Pi 40-pin header.

## Complete connection table

| Device | Device signal | Raspberry Pi signal | BCM GPIO | Physical pin | Direction |
| --- | --- | --- | ---: | ---: | --- |
| SSD1322 OLED | VCC | 3.3 V | - | 1 | Pi to OLED |
| SSD1322 OLED | GND | Ground | - | 6 | Common ground |
| SSD1322 OLED | DIN / MOSI / D1 | SPI0 MOSI | 10 | 19 | Pi to OLED |
| SSD1322 OLED | CLK / SCLK / D0 | SPI0 SCLK | 11 | 23 | Pi to OLED |
| SSD1322 OLED | CS | SPI0 CE0 | 8 | 24 | Pi to OLED |
| SSD1322 OLED | DC / A0 | Data/command | 25 | 22 | Pi to OLED |
| SSD1322 OLED | RST / RES | Reset | 27 | 13 | Pi to OLED |
| MAX98357A amp | VIN | 5 V | - | 2 | Pi to amp |
| MAX98357A amp | GND | Ground | - | 14 | Common ground |
| MAX98357A amp | BCLK | PCM clock | 18 | 12 | Pi to amp |
| MAX98357A amp | LRC / LRCLK / WS | PCM frame sync | 19 | 35 | Pi to amp |
| MAX98357A amp | DIN | PCM data | 21 | 40 | Pi to amp |
| MAX98357A amp | SD / EN | Not connected | - | - | Controlled by `no-sdmode` |
| TTP223B touch | VCC | 3.3 V | - | 17 | Pi to sensor |
| TTP223B touch | GND | Ground | - | 39 | Common ground |
| TTP223B touch | OUT / SIG | Digital input | 20 | 38 | Sensor to Pi |
| Speaker | `+` | MAX98357A `SPK+` | - | - | Amp to speaker |
| Speaker | `-` | MAX98357A `SPK-` | - | - | Amp to speaker |

The selected power and ground pins keep the wiring easy to identify. Equivalent Raspberry Pi power or ground pins may be used.

## Raspberry Pi 40-pin header map

```text
                         Raspberry Pi GPIO header

OLED VCC       <- 3.3V       (1)  (2)  5V          -> MAX98357A VIN
                              (3)  (4)  5V
                              (5)  (6)  GND         -> OLED GND
                              (7)  (8)
                 GND         (9) (10)
                             (11) (12) GPIO18       -> MAX98357A BCLK
OLED RST       <- GPIO27    (13) (14) GND          -> MAX98357A GND
                             (15) (16)
TTP223B VCC    <- 3.3V      (17) (18)
OLED MOSI      <- GPIO10    (19) (20) GND
                 GPIO9     (21) (22) GPIO25       -> OLED DC
OLED SCLK      <- GPIO11    (23) (24) GPIO8        -> OLED CS
                 GND       (25) (26)
                             (27) (28)
                             (29) (30) GND
                             (31) (32)
                             (33) (34) GND
MAX98357A LRC  <- GPIO19    (35) (36)
                             (37) (38) GPIO20       -> TTP223B OUT
TTP223B GND    <- GND       (39) (40) GPIO21       -> MAX98357A DIN
```

Pin 1 is the 3.3 V corner pin. Confirm the header orientation before applying power.

## SSD1322 OLED

The core opens `/dev/spidev0.0`, which uses SPI0 CE0.

```text
SSD1322 OLED          Raspberry Pi
-----------------------------------------------
VCC                -> 3.3 V, physical pin 1
GND                -> GND, physical pin 6
DIN / MOSI / D1    -> GPIO10, physical pin 19
CLK / SCLK / D0    -> GPIO11, physical pin 23
CS                 -> GPIO8 CE0, physical pin 24
DC / A0            -> GPIO25, physical pin 22
RST / RES           -> GPIO27, physical pin 13
```

OLED settings compiled into `mk-piclock.c`:

```text
SPI device: /dev/spidev0.0
SPI mode:   0
SPI speed:  4 MHz
DC:         BCM GPIO25
RST:        BCM GPIO27
```

Notes:

- The OLED does not use SPI MISO. GPIO9, physical pin 21, remains unused.
- Connect CS to CE0. Do not tie CS low when using `/dev/spidev0.0` unless the specific module requires it and no other SPI device shares the bus.
- SSD1322 module header order varies. Follow the signal labels printed on the module, not the position shown in a seller photograph.
- This project wiring uses 3.3 V for the OLED. Confirm the module is configured for 4-wire SPI.

## MAX98357A I2S amplifier

```text
MAX98357A             Raspberry Pi
-----------------------------------------------
VIN                -> 5 V, physical pin 2
GND                -> GND, physical pin 14
BCLK               -> GPIO18, physical pin 12
LRC / LRCLK / WS   -> GPIO19, physical pin 35
DIN                -> GPIO21, physical pin 40
SD / EN            -> Not connected
```

Required `/boot/firmware/config.txt` settings:

```ini
dtparam=audio=off
dtoverlay=max98357a,no-sdmode
```

`no-sdmode` means mk-piclock does not use a separate GPIO for the amplifier SD or EN input. Leave that pin unconnected unless the specific breakout board requires a fixed pull-up documented by its manufacturer.

## Speaker

Connect the speaker only to the MAX98357A output:

```text
MAX98357A SPK+  -> Speaker +
MAX98357A SPK-  -> Speaker -
```

Important:

- Do not connect either speaker terminal to Raspberry Pi ground.
- The MAX98357A speaker output is differential.
- Use one compatible 4 to 8 ohm speaker.
- Disconnect power before changing speaker or amplifier wiring.

## TTP223B touch sensor

```text
TTP223B               Raspberry Pi
-----------------------------------------------
VCC                -> 3.3 V, physical pin 17
GND                -> GND, physical pin 39
OUT / SIG          -> GPIO20, physical pin 38
```

The core reads GPIO20 as an active-high input and applies software debounce.

```text
Short press:   stop current audio
Hold 3 seconds: play random uploaded music
```

Power the TTP223B from **3.3 V**. Do not power it from 5 V while OUT is directly connected to a Raspberry Pi GPIO.

## GPIOs used by mk-piclock

| BCM GPIO | Physical pin | Purpose |
| ---: | ---: | --- |
| 8 | 24 | OLED SPI CE0 / CS |
| 10 | 19 | OLED SPI MOSI |
| 11 | 23 | OLED SPI clock |
| 18 | 12 | MAX98357A BCLK |
| 19 | 35 | MAX98357A LRC / LRCLK |
| 20 | 38 | TTP223B OUT |
| 21 | 40 | MAX98357A DIN |
| 25 | 22 | OLED DC |
| 27 | 13 | OLED reset |

## Pre-power checklist

- Confirm pin 1 orientation on the Raspberry Pi header.
- Confirm every module shares Raspberry Pi ground.
- Confirm the OLED VCC wire is on 3.3 V, not a GPIO.
- Confirm the touch sensor VCC wire is on 3.3 V.
- Confirm the speaker connects to `SPK+` and `SPK-`, not ground.
- Check for solder bridges before applying power.
- Enable SPI and the MAX98357A overlay before starting the services.
