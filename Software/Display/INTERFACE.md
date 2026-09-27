# Nextion interface map

For `NX8048P070_011.HMI` (800x480, NX8048P070-011C). This is the contract
between the panel and the STM32 firmware, confirmed against the real hardware.

## Wiring

**Go straight from the panel to PA9 / PA10.** Do not use J4 - its BSS138
level shifters are wired with the pins rotated one position, so the signal
lands on a MOSFET gate and there is no DC path to the MCU at all. TP5 and TP6
sit directly on the MCU nets.

| Panel | Board |
|---|---|
| TX | **PA10** (TP5) - 5 V tolerant, no shifter needed |
| RX | **PA9** (TP6) - 3.3 V drive is enough |
| GND | board GND |
| 5 V | its own supply (430 mA typical, 1 A recommended) |

Confirmed working: the panel answers `sendme` with `66 00 FF FF FF`, and
`dim=` visibly changes the backlight.

## Startup

```
baud=9600      dim=100      recmod=0
printh 00 00 00 ff ff ff 88 ff ff ff    // emitted on power-up
```

## Variables are PAGE-LOCAL, not global

This is the main thing to know. Neither set resolves from the other page - a
`get` or a write from the wrong page returns `1A` (variable name invalid).

| Page | Variables | Who writes |
|---|---|---|
| `monitor` | `xPV` `xSP` `xDuty` `xErr` | MCU |
| `setup` | `xSPset` `xKp` `xKi` | user |

So the firmware tracks which page is loaded (from the nav codes) and only
writes `monitor` objects while `monitor` is up.

**Making these global in the Nextion Editor would simplify the firmware a
lot** - it could then update values regardless of the page the user is on.

## Scaling

| Variable | Scale | Example |
|---|---|---|
| `xPV` | **x100** | 25.75 C -> 2575 |
| `xSP`, `xErr`, `xDuty` | assumed x100 | confirm on screen |
| `xSPset` | x10 | 51.5 C -> 515 |
| `xKp`, `xKi` | x1000 | 2.800 -> 2800 |

`xPV` at x100 is confirmed: pushing 265 displayed 2.6, and Devin's own
placeholder was 2575. The others are inferred and worth a visual check.

## Touch events

Five ASCII characters plus **CR LF** - not the Nextion binary `0x65` format.

| Code | Meaning |
|---|---|
| `p0b00` | keypad |
| `p0b01` | (att-39) |
| `p0b10` | page monitor |
| `p0b11` | page setup |
| `p0b12` | page trend |

**Apply** does not send a code - it sends the whole parameter set as text:

```
SP=515,KP=2800,KI=55<CR><LF>
```

That is the better mechanism, because it sidesteps page scope entirely. The
firmware parses it directly; no polling needed.

## Known gap for Devin

**Start and Stop appear to emit nothing.** Pressing them produces no serial
traffic, while Apply and the nav buttons do. They need `print "<code>"` plus
`printh 0D 0A` adding in their touch-release events, the same way the nav
buttons do it. Any short unique string works - e.g. `p0b20` / `p0b21` - or
extend the Apply-style text form with something like `CMD=START`.

Until then the panel can set the setpoint and gains but cannot arm or disarm
the heater.

## Firmware side

Writes are plain ASCII terminated by `FF FF FF`:

```
xPV.val=2575<FF><FF><FF>
```

`MODE=nextion` in `Software/Embedded/Thermo_Monitor` implements all of the
above: it pushes live temperature, tracks the page, parses Apply, decodes and
logs every frame received, and deliberately does **not** heat. Once Start/Stop
emit codes, the panel becomes a second client of the existing PI loop -
`g_cmd` / `g_srv` already carry setpoint, gains, enable, temperature, duty and
state, so no control code has to change.
