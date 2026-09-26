# MAX6675 thermocouple live monitor

A small standalone firmware for the Reflow Oven board (STM32F103C8T6) that does
nothing but read the MAX6675 and report the temperature. It never drives the
relay.

## Pin map (from `Reflow Project Kicad.xml`)

| Signal | MCU pin | Net / test point |
|---|---|---|
| MAX6675 `~CS` | PA4 | `Thermo_CS`, TP2 |
| MAX6675 `SCK` | PA5 | `Thermo_SCK`, TP3 |
| MAX6675 `SO`  | PA6 | `Thermo_SO`, TP4 |
| USART1 TX | PA9 | TP6 |
| USART1 RX | PA10 | TP5 |
| Relay | PA8 | J6 (held low here), indicator D3 |

Two indicator LEDs are useful while debugging: **D4 (red) sits on `Thermo_CS`
through R5**, so it is lit whenever CS is high — i.e. whenever this firmware is
actually running and holding the MAX6675 deselected. D4 dark on USB power means
the chip is not executing the flash image at all. **D3 (red) is on the relay
line.** Note D3/D4/R5/R10 postdate `Reflow Project Kicad.xml`; they exist only
in the current `.kicad_sch`.

## Output

The same line goes out three ways, so it can be watched with or without extra
hardware:

1. **USART1, 115200 8N1** on PA9/TP6.
2. **ARM semihosting** over SWD, so an attached ST-Link shows it with no
   USB-serial adapter. Guarded on `C_DEBUGEN`, so the board still runs
   standalone with no debugger.
3. **A telemetry struct at `0x20000000`** that a debugger can poll while the
   core keeps running: `magic("TMP1"), seq, raw, milli_c, fault, uptime_ms,
   sysclk_hz, on_hse`. `seq` is written last, so a changed `seq` means the
   rest is consistent.

Format is `seq,raw,tempC,status`, twice a second.

## Build and run

From Git Bash:

```bash
make
./run_monitor.sh 30                # flash, start, stream 30s of output
./run_monitor.sh 30 --run-only     # skip flashing
```

From Windows PowerShell, which cannot execute `.sh` files, use the equivalent
`.ps1` (note the leading `.\` — PowerShell does not run commands from the
current directory without it):

```powershell
.\run_monitor.ps1 -Seconds 30
.\run_monitor.ps1 -RunOnly
```

Building still needs `make`, so either use Git Bash or build in STM32CubeIDE.
Both runners use the STM32CubeIDE 1.15.0 bundled toolchain, CubeProgrammer and
OpenOCD; no separate installs are needed.

**Only the ST-Link needs to be plugged in.** It supplies 3.3 V to the board
through J2 pin 1 (CubeProgrammer and OpenOCD both report the target voltage,
~3.22 V here) and carries the semihosting output. USB is optional, and powering
from both at once is fine. USB alone gives you a running board but no console —
watch D4, or hang a USB-serial adapter on PA9/TP6 at 115200 8N1.

## Heater control

`control.c` drives the element through the IRF520N on PA8 (the "Relay" net,
J6), which is TIM1_CH1, so duty cycle is real hardware PWM at 200 Hz. D3 sits
on the same net and dims with duty.

```powershell
.\run_profile.ps1 -Mode pi                  # 30/40/50 C profile, PI control
.\run_profile.ps1 -Mode pi -Kp 8 -Ki 0.4    # retune without editing source
.\run_profile.ps1 -Mode step -StepDuty 100  # open-loop step, for tuning
.\run_profile.ps1 -Mode hold -StepDuty 100  # park the output to probe it
.\run_profile.ps1 -Mode pi -Csv out.csv     # also save the rows
```

One script because PowerShell 5.1 has no `&&`; it shells the `make` out to Git
Bash, flashes, runs, and captures. Gains are `-D` macros, so retuning is a
command-line argument rather than a source edit.

### Tuning it for this element

A 41 C setpoint with Kp 12 / Ki 0.6 peaked at **151 C**. Fitting that run
gives the plant, and the numbers explain everything:

| | |
|---|---|
| time constant | ~162 s |
| steady-state gain | **~3860 C per 100% duty** |
| energy delivered | 4.75 full-power-seconds |
| resulting rise | 113 C, i.e. ~24 C per full-power-second |

Duty actually needed to *hold* a setpoint (ambient ~28 C):

| setpoint | duty |
|---|---|
| 41 C | **0.34%** |
| 60 C | 0.83% |
| 100 C | 1.9% |
| 250 C | 5.8% |

The element keeps dumping heat for ~27 s after duty reaches zero, so overshoot
is set by the duty you are *at* when you arrive - not by how fast you cut:

| arriving at | overshoot |
|---|---|
| 0.5% | ~3 C |
| 1.5% | ~10 C |
| 5% | ~32 C |
| 10% | ~64 C |

That is the whole story. Kp 12 commands 100% duty at 8 C of error on a plant
that needs a third of one percent, and by the time the probe reacts the energy
is already committed.

**Suggested starting point:**

```powershell
.un_webapp.ps1 -Kp 0.2 -Ki 0.002 -MaxDuty 1.5
```

Use `-MaxDuty 0.5` if you want overshoot in the low single digits and are
happy with a slower approach. Raise the ceiling as the setpoint rises - 250 C
needs about 6%. `-MaxDuty` and `-OverTemp` are compiled in; the page cannot
raise them.

Gains are carried as micro-units (x1e6) rather than milli, because Ki of 0.002
is only two counts at a thousandth - not enough resolution to tune with. The
page's step sizes match: 0.01 for Kp, 0.001 for Ki.

**The real fix is less power.** This element is roughly a hundred times
oversized for bench setpoints; the entire useful control range is 0-2% duty,
which is also an awkward pulse width for an SSR at 1 Hz. Running the element
from a lower supply - 12 V quarters the power, 6 V cuts it sixteen-fold -
moves the working range into a comfortable 5-25% band and makes the loop far
better behaved. A real oven with a chamber full of thermal mass will behave
differently again, so treat all of this as calibration of the bench rig.

### Safety

In the firmware: any sensor fault forces duty to 0 (a controller that cannot
see temperature must not drive a heater); above 120 C the output latches off
until reset; every stage times out rather than sitting at full power forever;
the profile switches off and reports DONE.

In the host script: PA8 is forced low through the debugger *before and after*
every run, by enabling the GPIOA clock, clearing the output bit and switching
the pin to a push-pull output while the core is halted. That way neither a
crash nor a halted core can leave the element energised. Reset (SW2) also
drops the gate.

### Status: no heat reaches the thermocouple

The driver stage is fine. Two things were measured that rule it out: PA8's own
pin readback matches the commanded duty exactly, and there is **24 V across
the element at 100% duty**. A MOSFET failing to enhance would drop that supply
across itself and leave the element near 0 V, so the IRF520N is conducting -
an earlier note here blamed its gate threshold and was wrong.

The clean measurement is a cold start: three minutes with the heater off, then
six minutes at 100% duty.

| Segment | Mean |
|---|---|
| heater off, settled (120-180 s) | 27.71 C |
| 100% duty, after 5-6 min (480-540 s) | 27.42 C |
| **steady-state rise** | **-0.29 C** |

Zero, within the MAX6675's 0.25 C quantisation. Six minutes at full power
moves the sensor by nothing at all.

**Two earlier readings of this were wrong, both for the same reason.** The
first step test used a 30-second window and showed nothing, which was read as
"no heating". A later fit gave +15.3 m C/s and was read as "heating, but
slowly". That slope was residual heat from the *previous* run still working
through the rig - this bench holds heat between tests, and the 6-minute run
was climbing 0.4 C/s during its own heater-*off* baseline. Only a cold start
with a long settled baseline measures the heater rather than the last test.
Use `-BaseSeconds 180`.

So: the element dissipates power, and essentially none of it arrives at the
thermocouple. In likely order:

- **Thermal coupling.** If the element is warm to the touch while the probe
  reads ambient, the probe is not seeing it. Still air is an excellent
  insulator; a millimetre of it is enough to hide a low-power element. The tip
  needs physical contact - clamped, taped with foil or kapton, thermal paste.
- **Delivered power.** Measure the element's resistance; 24 V across R gives
  R/576 watts. That the IRF520N passes the current with only 3.3 V on its gate
  suggests the current, and therefore the power, is small.
- **Losses.** An element in open air with no enclosure sheds most of what it
  makes. The real oven will behave completely differently.

No gain value fixes a plant whose output never reaches the sensor. The stage
timeout exists so the firmware reports that rather than sitting at 100%
forever.

### Driving it with an SSR instead

An opto-isolated DC SSR (e.g. SSR-25DD, 3-32 V in, 5-200 V DC out, 25 A) is a
fine replacement for the IRF520N module here - 6.7 A against a 25 A rating is
comfortable, and a resistive element needs no flyback path. Three things to
get right:

1. **Drop the PWM frequency.** An SSR's opto output takes on the order of a
   millisecond to switch. At the 200 Hz default that is a large fraction of
   every 5 ms period spent in transition: it never switches cleanly and the
   SSR heats. Use time-proportioned control instead - `-PwmHz 1`. A plant with
   a minutes-long time constant does not notice, and duty resolution stays at
   0.05% because the period is always 2000 counts.
2. **3.3 V input is the bottom of the range.** PA8 supplies 3.3 V; "3-32 V"
   includes it with no margin at all. If it switches unreliably, buffer it -
   VBUS (5 V) is on J4 pin 3 - or keep the MOSFET module as the buffer stage.
   Input current is typically 7-15 mA, within PA8's budget. D3 + R10 stay in
   parallel on the Relay net and are harmless.
3. **Heatsink it, and watch polarity.** At 6.7 A even a 0.3 V drop is ~2 W.
   DC SSRs are polarity-sensitive on *both* input and output.

Note this part is DC-only. A mains-powered oven needs the AC version.

The cheaper fix is a logic-level MOSFET (IRLZ44N, IRLB8721) in place of the
IRF520N, which keeps 200 Hz switching. The SSR buys isolation and headroom.

### Commissioning a working switch

3.6 ohm at 24 V is 160 W - far more than these setpoints need, and enough to
overshoot hard before a 250 ms loop can react. Start capped and gentle:

```powershell
.\run_profile.ps1 -Mode step -PwmHz 1 -StepDuty 10 -BaseSeconds 180 -HeatSeconds 240 -Seconds 500 -Csv step10.csv
.\run_profile.ps1 -Mode pi -PwmHz 1 -MaxDuty 20 -DwellSeconds 60 -Csv pi.csv
```

The step test at low duty gives a rise curve to pick gains from without
cooking anything. Raise `-MaxDuty` only once the response is known.

## Web app

`run_webapp.ps1` builds the server firmware, flashes it, starts OpenOCD and
serves a small control page:

```powershell
.\run_webapp.ps1                      # 1 Hz PWM, initial duty cap 20%
.\run_webapp.ps1 -MaxDuty 60 -Setpoint 45
.\run_webapp.ps1 -NoBuild             # skip the build entirely
.\run_webapp.ps1 -ForceFlash          # reflash even if unchanged
```

Then open <http://127.0.0.1:8770/>. Ctrl-C stops everything.

**It only flashes when the chip does not already hold this image.** The binary
is read back and compared first, so restarting the app does not erase/program
on every launch or reset the board out from under whatever was running.
`-ForceFlash` overrides, and the build is incremental so an unchanged tree is
a no-op too.

**The duty cap is a runtime value.** `-MaxDuty` seeds it; the page changes it
live, and the page's fields adopt whatever the bridge started with so they
never misreport the live cap. `MAX_DUTY` in the Makefile is a different thing:
a build-time hard ceiling for a firmware that physically cannot exceed some
duty. It defaults to 100 so the UI control is honest - lower it only when you
want that guard baked in.

It shows live temperature, setpoint, duty and error, charts temperature
against setpoint and duty on a second axis-free chart below, and lets you set
a setpoint, cap the duty, start and stop.

### How it reaches the board

The board has no network of its own - no Ethernet, and USB device mode is out
while Y1 is unpopulated - so the only live path is SWD:

```
browser <-- HTTP --> webapp/bridge.py <-- TCL --> OpenOCD <-- SWD --> board
```

The firmware (`MODE=server`) runs the PI loop itself at 4 Hz. OpenOCD reads
and writes target memory through the DAP **while the core keeps running**, so
the control loop is never halted by the UI. The bridge finds both blocks by
running `nm` over the ELF, so the layout is never duplicated between firmware
and host.

**The page sends three numbers and nothing else: setpoint, Kp, Ki.** Duty is
computed on the MCU and only reported back, so a stalled browser or a slow
HTTP round trip can never stretch or skip a control step.

Both blocks use the same discipline: the writer fills the fields and bumps
`seq` last; the reader only acts on a block whose seq changed. A half-written
update cannot be acted on.

### The loop

Textbook PI with two details worth knowing:

- **The integral stores accumulated error (C.s), not an output contribution.**
  The I term is always `Ki * integ_err`, so retuning Ki does not leave history
  that means something different than it did a moment ago.
- **Ki changes are bumpless.** When Ki moves, `integ_err` is rescaled so the I
  term comes out where it already was. Without that, nudging Ki while hot
  steps the output - and on this element that is a real temperature
  excursion, not a cosmetic glitch.
- Anti-windup is conditional integration: accumulate only when the result
  would not be against a stop.

P and I are published separately from the duty they sum to, and the page plots
all three. Seeing which term is doing the work is the whole game when tuning
by hand.

### Safety

- **Dead-man switch.** The firmware only heats while it receives a heartbeat,
  and the bridge only relays that heartbeat while a client has called the API
  within 2.5 s. Close the tab and the output stops within a few seconds.
  Verified: enable -> `running`, client silent 9 s -> `host timeout` at 0%.
- **Explicit re-arm.** After a dead-man trip the firmware clears its own
  enable flag, so heating never resumes just because a browser reconnected -
  it takes a new Start. `push_command` refreshes the heartbeat *before* the
  seq write, otherwise that Start would be latched and cleared in the same
  cycle and nothing would ever re-arm it.
- Sensor fault forces duty to 0; over 120 C latches off until reset; duty is
  capped by `-MaxDuty` on top of whatever the loop asks for.
- Setpoint and duty are range-checked in the bridge as well as clamped in
  firmware. The browser is the least trustworthy part of the chain.

The bridge binds to 127.0.0.1 only. Don't expose it - there is no auth, and
the far end is a 160 W heater.

## Board quirks this firmware works around

These are properties of the board, not of the code, and they cost real time to
rediscover:

- **No NRST on the debug header.** J2 carries only 3V3/SWDIO/SWCLK/GND. `NRST`
  reaches only pushbutton SW2. A debugger therefore cannot pull reset, and
  `mode=UR` / plain `reset` do not do what they normally would.
- **SW1 sets the boot mode, and the +3.3V position bricks the board.** SW1 is
  an SPDT between +3.3V (pin 1) and GND (pin 3), feeding BOOT0 through R1;
  BOOT1/PB2 is unconnected and floats high, so BOOT0 high selects boot-from-
  SRAM and the chip never executes flash — after a reset the PC lands in SRAM.
  **SW1 belongs in the GND position**, where it is now; the board then boots
  the firmware on its own, on USB power, with no debugger attached.
- **Stale breakpoints.** CubeProgrammer's flash loader leaves a hardware
  breakpoint armed in SRAM, which traps the core as soon as it runs. The run
  script issues `rbp all` / `rwp all` before resetting.
- **Interrupt masks survive a manual start.** Only relevant if you start the
  core by hand rather than resetting it: a previous HardFault leaves `PRIMASK`
  set, SysTick then never ticks, `HAL_GetTick()` freezes, and every HAL
  timeout loop — including the HSE-ready wait — spins forever.
- **J4, the 5 V serial header, does not work - its level shifters are
  miswired.** (An earlier note here said U2's `OE` was tied low; that was read
  off the stale `.xml` netlist. **U2 does not exist on the current board at
  all** - it was replaced by two BSS138s, Q1 and Q2.)

  A BSS138 bidirectional shifter wants gate on the +3.3V *rail*, source on the
  3.3V signal, drain on the 5V signal. As built, the three connections are
  rotated one position (symbol pins are 1=G, 2=S, 3=D):

  | | gate (pad 1) | source (pad 2) | drain (pad 3) |
  |---|---|---|---|
  | Q2 (TX) | `3.3_USART1_TX_v1` (PA9) | `/USART1_TX_5V` (J4.2) | dead end -> R8 -> VBUS |
  | Q1 (RX) | `3.3_USART1_RX_v1` (PA10) | `/USART1_RX_5V` (J4.1) | dead end -> R7 -> VBUS |

  So on TX the part runs as a source follower: a high comes out at roughly
  3.3V - Vgs(th), about 2 V, and a low leaves the pin floating because nothing
  pulls J4.2 down. On RX the gate is the MCU's *input*: when the far end pulls
  the source low the FET conducts, but that only drags the dangling drain
  down - PA10 stays at 3.3 V through R6 and never sees a low. Both directions
  are broken.

  Usable UART is therefore at **TP6 (PA9, TX) and TP5 (PA10, RX)**, both plain
  3.3 V with 10k pull-ups (R9, R6). To make J4 live at 3.3 V instead, bridge
  pads 1-2 of Q1 and Q2 (1.90 mm apart, one 0603 zero-ohm each); R7/R8 then
  just hold the floating drains at 5 V, which is harmless.

## Clocking: no crystal fitted

**Y1 is not populated**, so there is no HSE. The firmware runs from the
internal oscillator instead — HSI/2 through PLL x16 = 64 MHz — and says so at
startup:

```
SYSCLK 64000000 Hz (internal HSI, Y1 not populated), sampling every 500 ms
```

HSI is inside the MCU, so nothing needs routing. The only cost is accuracy:
roughly +/-1% at room temperature and a few percent across the full range,
against tens of ppm for a crystal. That does not matter here — the MAX6675
does its own conversion and SCK has no minimum rate — and it is comfortably
inside what a 115200 UART tolerates.

What it does rule out is **USB device mode**, which needs 48 MHz derived from
an HSE PLL and cannot be synthesised from HSI on an F103. So J1 is power-only
unless Y1 gets populated, even though D+/D- and the R2 pull-up are routed.

If the crystal is ever fitted, build with `-DUSE_HSE=1` (or flip `USE_HSE` in
`Core/Src/main.c`) to get 72 MHz from it; the code still falls back to HSI if
it fails to start, rather than hanging.

## Schematic and PCB audit of U4

Checked against the MAX6675 datasheet pinout (1 GND, 2 T-, 3 T+, 4 VCC,
5 SCK, 6 ~CS, 7 SO, 8 N.C.). **The design is correct** - the fault is not in
the routing:

| Pin | Datasheet | Board | Copper |
|---|---|---|---|
| 1 | GND | GND | GND pour, F.Cu + B.Cu |
| 2 | T- | GND | GND pour, F.Cu + B.Cu |
| 3 | T+ | `Net-(U4-T+)` -> J5.1 | track |
| 4 | VCC | +3.3V | track |
| 5 | SCK | `Thermo_SCK` -> PA5, TP3 | track |
| 6 | ~CS | `Thermo_CS` -> PA4, TP2 | track |
| 7 | SO | `Thermo_SO` -> PA6, TP4 | track |
| 8 | N.C. | unconnected | none (correct) |

- **Continuity verified in copper**, not just in the netlist: for each of
  `Thermo_SCK`, `Thermo_CS` and `Thermo_SO`, the MCU pad, the test point and
  the U4 pad all land on a single connected island. SCK specifically runs
  U1.15 -> TP3 -> U4.5 unbroken.
- **T- is grounded**, as the datasheet's typical application circuit requires.
  It connects through the pour rather than a track, which is why a
  segments-only check wrongly reports it as unrouted - the board has 8 zones,
  including GND pours on both layers.
- **Decoupling is present**: C18 (100n) sits 5.3 mm from U4 across +3.3V/GND,
  satisfying the datasheet's bypass recommendation.
- Not fitted, and optional per the datasheet: a filter capacitor across
  T+/T-. Worth adding only if readings turn out noisy once the part responds.

One note for whoever reflows U4: pins 1 and 2 tie straight into a pour on both
layers with `connect_pads yes` and no thermal relief, so they sink a lot of
heat. That makes a hand-soldering iron much less effective on that side.

### Pin-to-pin short test

`probe_shorts()` rules out the other defect that fits the symptom: a bridge
between the adjacent pins 5/6/7. If CS and SO were bridged, SO would simply
mirror CS - high while deselected, low throughout a transfer - which looks
exactly like all-zeros data. The test pulls SO *down* while CS is high; the
MAX6675 releases SO then, so a 1 would mean CS is driving it through a bridge.
Measured result:

```
short: CS->SO=0  SCK->SO=0/1  SCK->CS=0/0
short: no pin-to-pin short found
```

No bridge. (`SCK->CS` reading 0/0 rather than 0/1 is just D4+R5 loading the CS
net - a 1k5 LED leg beats the MCU's ~40k internal pull-up. Harmless, since the
firmware drives CS push-pull.)

## Resolved: U4 VCC pin was lifted

**Fixed 2026-09-19.** The MAX6675's 3V3 pin (4) was lifted off its pad. The
part still answered chip select the whole time — SO released when CS went
high and was driven low when CS went low — because the die was scavenging
power through its I/O clamp diodes from CS. That is enough to work the output
stage but not the shift register, so every word came back `0x0000`.

Worth remembering, because it is a genuinely misleading failure: **a part that
responds to CS is not necessarily powered.** The symptom is indistinguishable
from a missing clock, and the design audit above was needed to get there —
schematic and PCB were both correct.

After reseating the pin:

```
SYSCLK 64000000 Hz (internal HSI, Y1 not populated), sampling every 500 ms
bus: SO(CS=1)=1 SO(CS=0)=0  SCK readback hi=1 lo=0
bus: SO per clock = 0000001101111001
1,0x0381,28.00,OK
2,0x0371,27.50,OK
3,0x0389,28.25,OK
```

Readings sit within +/-0.25 C, which is one MAX6675 LSB — quantisation, not
noise. `RUN_BUS_PROBE` is now 0; set it back to 1 if the sensor ever goes
quiet again.

### If it goes quiet again

The startup diagnostics distinguish the likely causes without a meter:

| `bus:` line | Meaning |
|---|---|
| `SO(CS=1)=1 SO(CS=0)=0`, all-zero words | Part answers CS but cannot shift — VCC or SCK not reaching the die |
| `SO(CS=1)=1 SO(CS=0)=1` | Nothing driving SO at all — part absent, or CS/SO open |
| `short:` reports a bridge | Solder bridge between the adjacent pins 5/6/7 |
| Words read `0xFFFF` | SO floating; reported as `NO_SENSOR_REPLY` |
| D2 set in the word | Thermocouple unplugged; reported as `OPEN_THERMOCOUPLE` |

`0x0000` and `0xFFFF` are both rejected rather than reported as 0 C.

With a meter, probe the U4 pins themselves rather than the test points — the
test points only prove the trace, not the joint. Pin 4 to pin 1 should read
3.3 V, and pin 6 should read 3.3 V while idle. For the clock:

```bash
make SCK_TEST=1 && ./run_monitor.sh 5
```

holds CS low and drives SCK as a 1 kHz square wave, which a DC meter reads as
roughly half of 3.3 V, so **TP3 and U4 pin 5 should both sit near 1.6 V**.
TP3 at 1.6 V with pin 5 stuck at 0 V or 3.3 V localises the break to the chip.
Rebuild with plain `make` afterwards.
