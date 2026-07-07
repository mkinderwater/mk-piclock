# mk-piclock v1.7.0 Pinouts

## OLED Display

| OLED Pin | Function | Raspberry Pi GPIO | Physical Pin |
| --- | --- | ---: | ---: |
| VSS | Ground | GND | 6 |
| VCC_IN | 3.3 V power | 3.3 V | 1 |
| D0 / CLK | SPI clock | GPIO11 | 23 |
| D1 / DIN | SPI MOSI | GPIO10 | 19 |
| D/C# | Data / command | GPIO25 | 22 |
| RES# | Reset | GPIO27 | 13 |
| CS# | SPI CE0 | GPIO8 | 24 |

## MAX98357A Audio Amplifier

| MAX98357A Pin | Function | Raspberry Pi GPIO | Physical Pin |
| --- | --- | ---: | ---: |
| VIN | 5 V power | 5 V | 2 or 4 |
| GND | Ground | GND | 9 or 14 |
| BCLK | I2S bit clock | GPIO18 | 12 |
| LRC / WS | I2S word select | GPIO19 | 35 |
| DIN | I2S data | GPIO21 | 40 |

## TTP223B Touch Sensor

| Touch Pin | Function | Raspberry Pi GPIO | Physical Pin |
| --- | --- | ---: | ---: |
| VCC | 3.3 V power | 3.3 V | 17 |
| GND | Ground | GND | 20 |
| OUT / SIG | Touch input | GPIO17 | 11 |

## RGB LED

| LED Channel | Function | Raspberry Pi GPIO | Physical Pin |
| --- | --- | ---: | ---: |
| Red | Red channel | GPIO5 | 29 |
| Green | Green channel | GPIO6 | 31 |
| Blue | Blue channel | GPIO13 | 33 |
| Common | Common cathode / ground | GND | 30 |

The RGB LED is wired as common cathode. Connect the common leg to ground, and connect each colour leg through its own resistor to the listed GPIO pin.
