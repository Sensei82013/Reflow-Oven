# Nextion interface map

Extracted from `NX8048P070_011.HMI` (Devin's design, 800x480, NX8048P070-011C).
This is the contract between the panel and the STM32 firmware.

## Startup

```
baud=9600            // the panel runs at 9600
dim=100
recmod=0             // passive mode - panel only speaks when spoken to,
                     // except for touch events and the boot message
printh 00 00 00 ff ff ff 88 ff ff ff   // emitted on power-up
page 0
```

The boot message matters for debugging: if you power-cycle the panel while
listening on PA10 and see nothing, the receive path is broken - the panel
sends that string unconditionally.

## Pages

`page 0` (splash) then `monitor`, `setup`, `trend`.

## Variables the MCU writes (process values)

| Variable | Meaning | Scaling |
|---|---|---|
| `xPV` | measured temperature | x10, so 28.5 C -> 285 |
| `xSP` | active setpoint | x10 |
| `xDuty` | duty cycle | x10, so 1.5 % -> 15 |
| `xErr` | setpoint minus temperature | x10, signed |

## Variables the MCU reads (user input)

| Variable | Meaning | Range in HMI | Scaling |
|---|---|---|---|
| `xSPset` | setpoint being edited | 0 - 1000 | x10, so 0 - 100.0 C |
| `xKp` | proportional gain | 0 - 20000 | x1000, so 0 - 20.000 |
| `xKi` | integral gain | 0 - 50000 | x1000, so 0 - 50.000 |

Read them with `get xSPset.val`, which returns `71 <int32 LE> FF FF FF`.

Those ranges line up with the limits already enforced in `webapp/bridge.py`
(setpoint 0-100 C, Ki max 50), so the two front ends agree.

## Text objects the MCU can write

`tState`, `tMode`, `tAlarm` (visibility toggled by the HMI), `tLed`,
`tHdr`, `tTitle`, `tNav`, `tPI`, `tLeg1`, `tLeg2`, `tYmax`, `tYmin`,
and the label/unit set `tPVlbl` `tSPlbl` `tDutylbl` `tErrlbl` `tKplbl`
`tKilbl` `tSPsetlbl` `tSPu` `tDutyu` `tErru`.

## Buttons

`bStart`, `bStop`, `bApply`, `bMon`, `bSetup`, `bTrend`,
`bSPp` / `bSPm`, `bKpp` / `bKpm`, `bKip` / `bKim`.

## Touch events

Sent as **5 ASCII characters followed by CR LF** - not the Nextion binary
`0x65` format. Same convention the 2021 firmware used.

| Code | Action in the HMI |
|---|---|
| `p0b00` | keypad (`keybdB`) |
| `p0b01` | (att-39) |
| `p0b10` | `page monitor` |
| `p0b11` | `page setup` |
| `p0b12` | `page trend` |

Note the panel changes page by itself for the navigation buttons; the MCU
only needs to track which page is active if it wants to avoid writing to
objects that are not loaded.

Codes for `bStart` / `bStop` / `bApply` were not recoverable from the
compiled strings - confirm them in the Nextion Editor, or watch them arrive
once the link works (`MODE=hmi` dumps every byte received).

## Firmware side

Writes are plain ASCII terminated by `FF FF FF`, e.g.

```
xPV.val=285<FF><FF><FF>
tState.txt="RUNNING"<FF><FF><FF>
```

The existing `g_cmd` / `g_srv` RAM blocks already carry exactly these fields,
so the panel can drive the same PI loop the web app does with no change to
the control code or its safety envelope.
