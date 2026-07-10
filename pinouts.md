# mk-piclock v1.7.6 Pinouts

> Confirmed wiring for the Raspberry Pi Zero W and Raspberry Pi Zero 2 W builds of mk-piclock Kids.

mk-piclock uses:

- Raspberry Pi Zero W or Zero 2 W
- 3.12-inch 256x64 SSD1322 OLED
- MAX98357A I2S amplifier
- 4-ohm, 3-watt speaker
- TTP223B capacitive touch sensor
- Common-cathode RGB LED

The software uses **BCM GPIO numbering**. Physical pin numbers refer to the Raspberry Pi 40-pin header.

> [!CAUTION]
> Disconnect power before changing wiring. Confirm the header orientation and every power connection before starting the clock.

---

## Complete connection table

| Device | Device pin or pad | Device signal | Raspberry Pi signal | BCM GPIO | Physical pin | Direction |
|:--|:--|:--|:--|--:|--:|:--|
| External 5 V supply | `+` | +5 V | Pi 5 V input | - | 4 | Supply to Pi |
| External 5 V supply | `-` | GND | Pi ground | - | 9 | Supply to Pi |
| SSD1322 OLED | Pin 1 | VSS | Ground | - | 6 | Common ground |
| SSD1322 OLED | Pin 2 | VCC_IN | 3.3 V | - | 1 | Pi to OLED |
| SSD1322 OLED | Pin 4 | D0 / CLK | SPI0 SCLK | 11 | 23 | Pi to OLED |
| SSD1322 OLED | Pin 5 | D1 / DIN | SPI0 MOSI | 10 | 19 | Pi to OLED |
| SSD1322 OLED | Pin 14 | D/C# | Data / command | 25 | 22 | Pi to OLED |
| SSD1322 OLED | Pin 15 | RES# | Reset | 27 | 13 | Pi to OLED |
| SSD1322 OLED | Pin 16 | CS# | SPI0 CE0 | 8 | 24 | Pi to OLED |
| MAX98357A amplifier | VIN pad | VIN | 5 V | - | 2 | Pi to amplifier |
| MAX98357A amplifier | GND pad | GND | Ground | - | 14 | Common ground |
| MAX98357A amplifier | BCLK pad | BCLK | PCM clock | 18 | 12 | Pi to amplifier |
| MAX98357A amplifier | LRC / WS pad | LRC / LRCLK / WS | PCM frame sync | 19 | 35 | Pi to amplifier |
| MAX98357A amplifier | DIN pad | DIN | PCM data | 21 | 40 | Pi to amplifier |
| MAX98357A amplifier | SD / EN pad | SD / EN | Not connected | - | - | Module default |
| TTP223B touch sensor | VCC pad | VCC | 3.3 V | - | 17 | Pi to sensor |
| TTP223B touch sensor | GND pad | GND | Ground | - | 39 | Common ground |
| TTP223B touch sensor | OUT / SIG pad | OUT / SIG | Digital input | 20 | 38 | Sensor to Pi |
| RGB LED | Red lead | Red anode through resistor | Digital PWM output | 5 | 29 | Pi to LED |
| RGB LED | Green lead | Green anode through resistor | Digital PWM output | 6 | 31 | Pi to LED |
| RGB LED | Blue lead | Blue anode through resistor | Digital PWM output | 13 | 33 | Pi to LED |
| RGB LED | Common lead | Common cathode | Ground | - | 30 | Common ground |
| Speaker | `+` terminal | Speaker + | MAX98357A `SPK+` | - | - | Amplifier to speaker |
| Speaker | `-` terminal | Speaker - | MAX98357A `SPK-` | - | - | Amplifier to speaker |

---

## Power

The clock uses a regulated **5 V, 2 A or better** supply.

The enclosure power input feeds the Raspberry Pi through:

```text
External +5 V  -> Physical pin 4
External GND   -> Physical pin 9
```

The amplifier uses physical pins 2 and 14. Physical pins 2 and 4 share the Raspberry Pi 5 V rail. All Raspberry Pi ground pins are common.

### USB cable recommendation

Use a **USB-A to USB-C cable** with the enclosure power input. The simple USB-C power board does not perform full USB-C current negotiation, so some USB-C to USB-C supplies may not provide power correctly.

> [!WARNING]
> Do not connect power to the Raspberry Pi USB port while the external 5 V header supply is connected. Never power the clock from two sources at once.

The OLED and touch sensor are the two 3.3 V devices. They use separate Raspberry Pi header pins.

---

## Raspberry Pi 40-pin header map

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

---

## SSD1322 OLED

This wiring is confirmed for the smaller 3.12-inch 256x64 SSD1322 module used by mk-piclock.

| OLED pin | OLED signal | Raspberry Pi connection |
|--:|:--|:--|
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

### OLED notes

- Treat the numbered table above as the authoritative OLED pinout.
- OLED pins 3 and 6 through 13 are not used by mk-piclock.
- The OLED does not use SPI MISO. GPIO9, physical pin 21, remains unused.
- Connect CS# to CE0. Do not tie CS# low.
- Follow the signal labels printed on the module. Seller photos may show a different header orientation.
- Power the OLED from 3.3 V.
- Configure the module for 4-wire SPI.

---

## MAX98357A I2S amplifier

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

`no-sdmode` means mk-piclock does not control the amplifier SD or EN input with a GPIO. Leave SD / EN unconnected. The tested module uses its onboard bias network to enable playback and select its default mono mix.

Do not connect SD / EN to a Raspberry Pi GPIO or directly to 3.3 V.

### Verify the Linux audio device

No `alsa-utils` package is required. Check the kernel audio devices directly:

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
```

The output should include the MAX98357A or `bcm2835-i2s` device.

### Audio startup click

A small click when audio starts or stops is expected. The amplifier remains enabled, and mk-piclock does not use SD / EN switching or a permanent silence stream.

---

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
- The current build uses a 4-ohm, 3-watt speaker.
- Disconnect power before changing the speaker or amplifier wiring.

---

## TTP223B touch sensor

```text
TTP223B pad           Raspberry Pi
-----------------------------------------------
VCC                -> 3.3 V, physical pin 17
GND                -> GND, physical pin 39
OUT / SIG          -> GPIO20, physical pin 38
```

The core reads GPIO20 as an active-high input and applies software debounce.

### Touch behaviour

| Action | Result |
|:--|:--|
| Tap while audio is playing | Stops the current alarm, song, or story |
| Hold and release | Plays random uploaded music |
| Continue holding | Opens the OLED network diagnostics screen |
| Tap while diagnostics is open | Closes the diagnostics screen |
| Ten short taps | Plays a random story when Story Mode is enabled |
| Any accepted touch | Blinks the RGB LED using the selected touch colour |

The diagnostic hold does not start music and is not counted toward Story Mode. The diagnostic screen also closes automatically.

Power the TTP223B from **3.3 V**. Do not power it from 5 V while OUT is connected directly to a Raspberry Pi GPIO.

---

## Common-cathode RGB LED

```text
RGB LED lead             Raspberry Pi
-----------------------------------------------
Red anode, resistor   -> GPIO5, physical pin 29
Green anode, resistor -> GPIO6, physical pin 31
Blue anode, resistor  -> GPIO13, physical pin 33
Common cathode        -> GND, physical pin 30
```

Use one **220 to 330 ohm resistor** on each colour channel.

The software drives the three anodes active-high using 200 Hz batched trailing-edge PWM with 32 stable duty levels. This wiring is for a common-cathode LED. Common-anode LEDs require inverted output logic and are not supported.

Four-lead RGB LEDs do not all use the same lead order. Treat the colour names and common cathode as authoritative. Confirm the actual lead order from the LED datasheet or with the **Lighting** page tests.

The red, green, blue, and white wiring tests temporarily bypass the master switch, brightness ceiling, and colour calibration so each physical channel can be checked directly.

---

## GPIOs used by mk-piclock

| BCM GPIO | Physical pin | Purpose |
|--:|--:|:--|
| 5 | 29 | RGB LED red |
| 6 | 31 | RGB LED green |
| 8 | 24 | OLED SPI CE0 / CS# |
| 10 | 19 | OLED SPI MOSI |
| 11 | 23 | OLED SPI clock |
| 13 | 33 | RGB LED blue |
| 18 | 12 | MAX98357A BCLK |
| 19 | 35 | MAX98357A LRC / LRCLK |
| 20 | 38 | TTP223B OUT / SIG |
| 21 | 40 | MAX98357A DIN |
| 25 | 22 | OLED D/C# |
| 27 | 13 | OLED reset |

### Unused pins near project wiring

| BCM GPIO | Physical pin | Status |
|--:|--:|:--|
| 9 | 21 | SPI0 MISO is not used by the OLED |

---

## Required boot configuration

Add these settings under the existing `[all]` section in `/boot/firmware/config.txt`:

```ini
# mk-piclock hardware configuration

dtparam=spi=on
dtparam=audio=off
dtoverlay=max98357a,no-sdmode
gpu_mem=16
```

Do not add a second `[all]` heading.

Reboot after changing the boot configuration:

```bash
sudo reboot
```

Verify SPI:

```bash
ls -l /dev/spidev0.0
```

Verify audio without `alsa-utils`:

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
```

---

## Pre-power checklist

- Confirm the supply is regulated 5 V and rated for at least 2 A.
- Use a USB-A to USB-C cable for the enclosure power input.
- Connect external +5 V to physical pin 4 and external ground to physical pin 9.
- Do not connect direct Raspberry Pi USB power while the header supply is connected.
- Confirm pin 1 orientation on the Raspberry Pi header.
- Confirm all devices share Raspberry Pi ground.
- Confirm OLED pin 2 is connected to 3.3 V, physical pin 1.
- Confirm OLED pin 1 is connected to ground, physical pin 6.
- Confirm OLED D/C# is GPIO25, physical pin 22.
- Confirm OLED RES# is GPIO27, physical pin 13.
- Confirm OLED CS# is GPIO8, physical pin 24.
- Confirm the touch sensor VCC wire is on 3.3 V.
- Confirm the touch sensor OUT / SIG wire is on GPIO20, physical pin 38.
- Confirm the RGB LED is common-cathode.
- Confirm each LED colour has its own current-limiting resistor.
- Confirm the amplifier SD / EN pad is not connected.
- Confirm amplifier DIN is GPIO21, physical pin 40, and is not tied to 3.3 V.
- Confirm the speaker connects to `SPK+` and `SPK-`, not ground.
- Check for solder bridges and loose strands before applying power.
- Enable SPI and the MAX98357A overlay before starting the services.

---

## Quick fault checks

### OLED is blank

```bash
ls -l /dev/spidev0.0
sudo systemctl --no-pager --full status mk-piclock-core.service
sudo journalctl -b -u mk-piclock-core.service -n 100 --no-pager
```

Confirm OLED power is 3.3 V and that DC, reset, CS, MOSI, and clock match the table above.

### No audio

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
sudo systemctl --no-pager --full status mk-piclock-core.service
```

Confirm `dtoverlay=max98357a,no-sdmode` is active, SD / EN is disconnected, and the speaker is connected across `SPK+` and `SPK-`.

### Touch does not respond

Confirm:

```text
VCC       -> 3.3 V, physical pin 17
GND       -> Physical pin 39
OUT / SIG -> GPIO20, physical pin 38
```

Then review the core log:

```bash
sudo journalctl -b -u mk-piclock-core.service -n 100 --no-pager
```

### RGB colour is wrong

Use the direct red, green, blue, and white tests under **Lighting**. If two colours are reversed, swap only those two anode wires. Keep the common cathode on ground.
