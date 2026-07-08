# mk-piclock v1.7.0 Pinouts

This document covers the Raspberry Pi GPIO header, the confirmed working smaller 3.12-inch 256x64 SSD1322 OLED, MAX98357A I2S amplifier, speaker, TTP223B touch sensor, and common-cathode RGB LED used by mk-piclock.

The software uses **BCM GPIO numbering**. Physical pin numbers refer to the Raspberry Pi 40-pin header.

## Complete Connection Table

| Device | Device pin / pad | Device signal | Raspberry Pi signal | BCM GPIO | Physical pin | Direction |
| --- | --- | --- | --- | ---: | ---: | --- |
| External 5 V supply | + | +5 V | Pi 5 V input | - | 4 | Supply to Pi |
| External 5 V supply | - | GND | Pi ground | - | 9 | Supply to Pi |
| SSD1322 OLED | Pin 1 | VSS | Ground | - | 6 | Common ground |
| SSD1322 OLED | Pin 2 | VCC_IN | 3.3 V | - | 1 | Pi to OLED |
| SSD1322 OLED | Pin 4 | D0 / CLK | SPI0 SCLK | 11 | 23 | Pi to OLED |
| SSD1322 OLED | Pin 5 | D1 / DIN | SPI0 MOSI | 10 | 19 | Pi to OLED |
| SSD1322 OLED | Pin 14 | D/C# | Data / command | 25 | 22 | Pi to OLED |
| SSD1322 OLED | Pin 15 | RES# | Reset | 27 | 13 | Pi to OLED |
| SSD1322 OLED | Pin 16 | CS# | SPI0 CE0 | 8 | 24 | Pi to OLED |
| MAX98357A amp | VIN pad | VIN | 5 V | - | 2 | Pi to amp |
| MAX98357A amp | GND pad | GND | Ground | - | 14 | Common ground |
| MAX98357A amp | BCLK pad | BCLK | PCM clock | 18 | 12 | Pi to amp |
| MAX98357A amp | LRC / WS pad | LRC / LRCLK / WS | PCM frame sync | 19 | 35 | Pi to amp |
| MAX98357A amp | DIN pad | DIN | PCM data | 21 | 40 | Pi to amp |
| MAX98357A amp | SD / EN pad | SD / EN | Not connected | - | - | Module default |
| TTP223B touch | VCC pad | VCC | 3.3 V | - | 17 | Pi to sensor |
| TTP223B touch | GND pad | GND | Ground | - | 39 | Common ground |
| TTP223B touch | OUT / SIG pad | OUT / SIG | Digital input | 20 | 38 | Sensor to Pi |
| RGB LED | Red lead | Red anode through resistor | Digital PWM output | 5 | 29 | Pi to LED |
| RGB LED | Green lead | Green anode through resistor | Digital PWM output | 6 | 31 | Pi to LED |
| RGB LED | Blue lead | Blue anode through resistor | Digital PWM output | 13 | 33 | Pi to LED |
| RGB LED | Common lead | Common cathode | Ground | - | 30 | Common ground |
| Speaker | `+` terminal | Speaker + | MAX98357A `SPK+` | - | - | Amp to speaker |
| Speaker | `-` terminal | Speaker - | MAX98357A `SPK-` | - | - | Amp to speaker |

The external regulated 5 V supply powers the Raspberry Pi through physical pin 4, with supply ground on physical pin 9. The amplifier uses physical pins 2 and 14, so no power connector is physically shared. Raspberry Pi pins 2 and 4 are still electrically connected to the same internal 5 V rail, and all ground pins are common. Do not connect USB power while the header supply is connected.

The build has two 3.3 V consumers: OLED VCC and TTP223B VCC. They use separate Raspberry Pi header pins. Leave the MAX98357A SD / EN pad unconnected.

## Raspberry Pi 40-Pin Header Map

```text
                         Raspberry Pi GPIO header

OLED Pin 2 VCC  <- 3.3V       (1)  (2)  5V          -> MAX98357A VIN
                              (3)  (4)  5V          <- EXTERNAL +5V INPUT
                              (5)  (6)  GND         -> OLED Pin 1 VSS
                              (7)  (8)
EXTERNAL GND    -> GND         (9) (10)
                             (11) (12) GPIO18       -> MAX98357A BCLK
OLED Pin 15 RST <- GPIO27    (13) (14) GND          -> MAX98357A GND
                             (15) (16)
TTP223B VCC     <- 3.3V      (17) (18)
OLED Pin 5 DIN  <- GPIO10    (19) (20) GND
                 GPIO9      (21) (22) GPIO25       -> OLED Pin 14 D/C#
OLED Pin 4 CLK  <- GPIO11    (23) (24) GPIO8        -> OLED Pin 16 CS#
                 GND        (25) (26)
                             (27) (28)
RGB red lead    <- GPIO5     (29) (30) GND          -> RGB common cathode
RGB green lead  <- GPIO6     (31) (32)
RGB blue lead   <- GPIO13    (33) (34) GND
MAX98357A LRC   <- GPIO19    (35) (36)
                             (37) (38) GPIO20       -> TTP223B OUT / SIG
TTP223B GND     <- GND       (39) (40) GPIO21       -> MAX98357A DIN
```

Pin 1 is the 3.3 V corner pin. Confirm the header orientation before applying power.

## SSD1322 OLED

This exact wiring is confirmed working with the smaller 3.12-inch 256x64 SSD1322 module used by mk-piclock.

| OLED pin | OLED signal | Raspberry Pi |
| ---: | --- | --- |
| 1 | VSS | GND, physical pin 6 |
| 2 | VCC_IN | 3.3 V, physical pin 1 |
| 4 | D0 / CLK | GPIO11 / SPI0 SCLK, physical pin 23 |
| 5 | D1 / DIN | GPIO10 / SPI0 MOSI, physical pin 19 |
| 14 | D/C# | GPIO25, physical pin 22 |
| 15 | RES# | GPIO27, physical pin 13 |
| 16 | CS# | GPIO8 / SPI0 CE0, physical pin 24 |

The core opens `/dev/spidev0.0`, which uses SPI0 CE0.

```text
SPI device: /dev/spidev0.0
SPI mode:   0
SPI speed:  4 MHz
DC:         BCM GPIO25
RST:        BCM GPIO27
```

Notes:

- Treat this table as the authoritative OLED pinout.
- OLED pins 3 and 6 through 13 are not connected by mk-piclock.
- The OLED does not use SPI MISO. GPIO9, physical pin 21, remains unused.
- Connect CS to CE0. Do not tie CS low when using `/dev/spidev0.0`.
- Follow the signal labels printed on the module. Seller photographs may show a different header orientation.
- Power the OLED from 3.3 V.
- The module must be configured for 4-wire SPI.

## MAX98357A I2S Amplifier

The MAX98357A breakout normally labels pads rather than numbered pins.

```text
MAX98357A pad          Raspberry Pi
-----------------------------------------------
VIN                 -> 5 V, physical pin 2
GND                 -> GND, physical pin 14
BCLK                -> GPIO18, physical pin 12
LRC / LRCLK / WS    -> GPIO19, physical pin 35
DIN                 -> GPIO21, physical pin 40
SD / EN             -> Not connected
```

Required `/boot/firmware/config.txt` settings:

```ini
dtparam=audio=off
dtoverlay=max98357a,no-sdmode
```

`no-sdmode` means mk-piclock does not control the amplifier SD or EN input from a GPIO. On the tested module, leaving SD / EN unconnected allows the module's onboard bias network to enable playback and select its default mono mix. Do not connect SD / EN to a Raspberry Pi GPIO or directly to 3.3 V.

### Audio Startup Click

A small click when playback starts is accepted in this release. SD / EN remains unconnected, and mk-piclock does not add amplifier switching, a persistent silence stream, or other click-suppression logic.

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

## TTP223B Touch Sensor

The TTP223B breakout normally labels pads rather than numbered pins.

```text
TTP223B pad           Raspberry Pi
-----------------------------------------------
VCC                -> 3.3 V, physical pin 17
GND                -> GND, physical pin 39
OUT / SIG          -> GPIO20, physical pin 38
```

The core reads GPIO20 as an active-high input and applies software debounce.

```text
Short press:        stop current audio
Hold 3 seconds:     play random uploaded music
Ten short taps:     play random story when Story Mode is enabled
Any touch press:    blink the RGB LED using the selectable touch colour
```

Power the TTP223B from **3.3 V**. Do not power it from 5 V while OUT is directly connected to a Raspberry Pi GPIO.

## Common-Cathode RGB LED

```text
RGB LED lead             Raspberry Pi
-----------------------------------------------
Red anode, resistor   -> GPIO5, physical pin 29
Green anode, resistor -> GPIO6, physical pin 31
Blue anode, resistor  -> GPIO13, physical pin 33
Common cathode        -> GND, physical pin 30
```

Use one 220 to 330 ohm current-limiting resistor on each colour channel. The software drives the three anodes active-high using 200 Hz batched trailing-edge PWM with 32 stable duty levels. This wiring is for a common-cathode LED. A common-anode module requires inverted output logic and is not supported by this release.

Four-lead RGB LEDs do not all use the same physical lead order. Treat the colour names and the common cathode as authoritative, then confirm the actual lead order from your LED/module datasheet or with the Lighting page tests.

The Lighting page includes direct red, green, blue, and white wiring tests. These tests temporarily bypass the master switch, global brightness ceiling, and colour calibration so each physical channel can be checked independently.

## GPIOs Used by mk-piclock

| BCM GPIO | Physical pin | Purpose |
| ---: | ---: | --- |
| 5 | 29 | RGB LED red |
| 6 | 31 | RGB LED green |
| 8 | 24 | OLED SPI CE0 / CS |
| 10 | 19 | OLED SPI MOSI |
| 11 | 23 | OLED SPI clock |
| 13 | 33 | RGB LED blue |
| 18 | 12 | MAX98357A BCLK |
| 19 | 35 | MAX98357A LRC / LRCLK |
| 20 | 38 | TTP223B OUT |
| 21 | 40 | MAX98357A DIN |
| 25 | 22 | OLED DC |
| 27 | 13 | OLED reset |

## Pre-Power Checklist

- Confirm the external supply is a regulated 5 V source suitable for the Pi and amplifier load.
- Connect external +5 V to physical pin 4 and external ground to physical pin 9.
- Do not connect USB power while the GPIO header supply is connected.
- Confirm pin 1 orientation on the Raspberry Pi header.
- Confirm every module shares Raspberry Pi ground.
- Confirm OLED pin 2 is connected to 3.3 V, physical pin 1.
- Confirm OLED pin 1 is connected to ground, physical pin 6.
- Confirm OLED D/C# is GPIO25, physical pin 22.
- Confirm OLED RES# is GPIO27, physical pin 13.
- Confirm the touch sensor VCC wire is on 3.3 V.
- Confirm the touch sensor OUT / SIG wire is on GPIO20, physical pin 38.
- Confirm the RGB LED is common-cathode.
- Confirm each LED colour has its own current-limiting resistor.
- Confirm the amplifier SD / EN pad is not connected.
- Confirm amplifier DIN is connected to GPIO21, physical pin 40, and is not tied to 3.3 V.
- Confirm the speaker connects to `SPK+` and `SPK-`, not ground.
- Check for solder bridges before applying power.
- Enable SPI and the MAX98357A overlay before starting the services.
