# Reflow Oven - project brief and hard-won pitfalls

Read this before touching the hardware. Nearly every item below cost hours to
find and looks like something else when you hit it.

---

## Goal

Turn a toaster oven into a PCB reflow oven. An STM32F103 reads a K-type
thermocouple through a MAX6675, drives a 160 W element through a solid state
relay, and runs a PI loop to hold temperature. A 7" Nextion panel is the
intended user interface; a browser app over SWD exists as a working stand-in.

The end goal is a proper reflow profile - soak, ramp, peak, cool - commanded
from the panel.

---

## Architecture

The PI loop lives **on the MCU** and owns the duty cycle. Hosts only supply a
setpoint and gains. That boundary is deliberate: a stalled browser or a slow
HTTP round trip can then never stretch or skip a control step.

```
browser  --HTTP-->  webapp/bridge.py  --TCL-->  OpenOCD  --SWD-->  board
```

The board has no network of its own and USB device mode is unavailable (see
the crystal note below), so SWD is the only live path. OpenOCD reads and
writes target memory through the DAP **while the core keeps running**, so the
UI never halts the control loop.

Two RAM blocks carry the traffic, `g_cmd` and `g_srv`, located by running
`nm` over the ELF rather than hard-coding addresses. Both use the same
discipline: the writer fills the fields and bumps `seq` **last**, and the
reader acts only on a block whose seq changed, so a half-written update can
never be acted on.

---

## Pitfalls

### Flashing and boot

- **No NRST on the debug header.** J2 carries only 3V3/SWDIO/SWCLK/GND, so a
  debugger cannot pull reset and `mode=UR` does not do what it normally does.
  Reset goes through the Cortex-M `SYSRESETREQ` bit instead - see
  `openocd.cfg`.
- **SW1 selects BOOT0, and the wrong position looks like a dead board.** With
  BOOT1/PB2 floating high, BOOT0 high means boot-from-SRAM and the part never
  executes flash; after reset the PC lands around `0x2000xxxx`. **SW1 belongs
  in the GND position.** Visible tell: D4 (on `Thermo_CS`) is lit whenever
  firmware is actually running.
- **CubeProgrammer leaves a hardware breakpoint armed** in SRAM, which traps
  the core the moment it runs. Issue `rbp all` / `rwp all` before resetting.
- **Interrupt masks survive a manual start.** If you ever start the core by
  hand rather than resetting it, a previous HardFault leaves `PRIMASK` set;
  SysTick then never ticks, `HAL_GetTick()` freezes, and every HAL timeout
  loop spins forever - including the HSE-ready wait, which looks like a
  hung boot.

### Clock

- **Y1 is not populated.** There is no HSE. Everything runs from HSI through
  the PLL at 64 MHz. Do not treat "HSE not ready" as a fault.
- Consequence: **USB device mode can never work** as populated, because the
  F103 needs 48 MHz from an HSE PLL. J1 is power-only despite D+/D- and the
  R2 pull-up being routed.

### Build system

- **Changing a `-D` flag used to silently keep stale objects.** Object files
  depend on sources, not on the command line, so `make MODE=server
  MAX_DUTY=10` after a `MAX_DUTY=30` build did nothing and shipped the old
  binary. Combined with the runner's flash-skip, a requested duty ceiling
  silently never existed and the heater overshot by 30 °C. Fixed with a flags
  stamp (`build/flags.stamp`) that every object depends on. If a `-D`
  controlled limit seems not to take effect, hash the binary before and after
  rather than trusting "Nothing to be done".

### Thermocouple

- **A part that answers chip select is not necessarily powered.** The MAX6675
  once returned `0x0000` forever while still releasing SO when CS went high
  and driving it low when CS went low. It was scavenging power through its
  I/O clamp diodes from CS - its **3V3 pin was lifted**. The symptom is
  indistinguishable from a missing clock. Check power *at the pin* before
  suspecting the clock or the code.
- `0x0000` and `0xFFFF` are **not** valid frames and are reported as
  `NO_SENSOR_REPLY`, never as 0 °C. An unplugged thermocouple sets D2 and
  reads `OPEN_THERMOCOUPLE` instead.
- The design was audited against the datasheet and is correct: pinout matches,
  SCK/CS/SO are continuous in copper, C18 decouples VCC, T- is grounded.
  **Gotcha when auditing this PCB:** it has 8 copper zones, and the syntax is
  `(zone (net 2) (net_name "GND")`. A segments-and-vias-only continuity check
  wrongly reports every pour-connected pad as unrouted - U4 pin 2 especially.

### Heater drive

- **The original IRF520N could not switch this load from 3.3 V logic.**
  R_DS(on) is specified at V_GS = 10 V and V_GS(th) is 2-4 V, so a 3.3 V gate
  leaves it near threshold. Replaced with an SSR-25DD, which works.
- **The module is a low-side switch**, so measuring V+ to GND reads 24 V
  whether or not the FET conducts. That measurement fooled us into clearing
  the driver when it was in fact the problem. Measure **drain to source**, or
  across the element.
- **SSRs cannot follow fast PWM.** An opto-isolated DC SSR takes roughly a
  millisecond to switch, so at 200 Hz it never switches cleanly. Use
  time-proportioned control: `PWM_HZ=1`. Duty resolution stays at 0.05 %
  because the period is always 2000 counts.

### The plant itself

Measured, not guessed:

| | |
|---|---|
| Time constant | ~162 s |
| Steady-state gain | **~3860 °C per 100 % duty** |
| Energy to temperature | ~24 °C per full-power-second |

Duty needed to **hold** a setpoint (ambient ~28 °C): **0.34 % at 41 °C**,
0.83 % at 60 °C, 1.9 % at 100 °C, 5.8 % at 250 °C.

The element keeps dumping heat for ~27 s after duty reaches zero, so
**overshoot is set by the duty you are at when you arrive**, not by how fast
you cut:

| arriving at | overshoot |
|---|---|
| 0.5 % | ~3 °C |
| 1.5 % | ~10 °C |
| 5 % | ~32 °C |
| 10 % | ~64 °C |

So textbook gains are actively dangerous here. Kp = 12 %/°C commands full
power at 8 °C of error on a plant that needs a third of one percent; a run to
a 41 °C setpoint peaked at **151 °C**. Start around **Kp 0.2, Ki 0.002,
`-MaxDuty 1.5`** and raise the ceiling only as the setpoint rises.

Gains are carried as micro-units (x1e6) rather than milli, because Ki of 0.002
is only two counts at a thousandth - not enough resolution to tune with.

**This is dead-time dominated.** No gain value fixes that; the duty ceiling
does the real work. Note also that the element position and insulation are
expected to change, so do not over-tune the current rig.

### Nextion panel

**Wire the panel straight to PA9 / PA10. That works.**

| Panel | Board |
|---|---|
| TX | **PA10** (TP5) - 5 V tolerant, no level shifter needed |
| RX | **PA9** (TP6) - 3.3 V drive is sufficient |
| GND | board GND |
| 5 V | its own supply - 430 mA typical, 1 A recommended |

**Do not use J4.** Its BSS138 shifters have the pins rotated one position, so
a signal arriving there lands on a MOSFET gate and there is simply no DC path
to the MCU. Everything else - baud, levels, grounds - was a red herring; this
was the whole problem, and it cost a lot of time. TP5/TP6 sit directly on the
MCU nets with nothing in between.

Confirmed working once moved: the panel answers `sendme` with
`66 00 FF FF FF` at 9600, and `dim=` visibly changes the backlight.

The panel is an **NX8048P070-011C**: 7", 800x480, capacitive, *Intelligent*
series, default baud 9600, **no USB power**.

See `Software/Display/README.txt` for the full object map - that file is the
contract, and `MODE=nextion` implements it. Four things about it shape the
firmware:

- **Search the HMI for `prints`, not `print`.** An earlier audit of this panel
  concluded that Start and Stop emitted nothing, because the grep only looked
  for `print`. They use `prints "p0b20"` / `prints "p0b21"` and always worked.
  A wrong "the panel sends nothing" sends you looking at wiring for no reason.
- **Page scope matters, but only for some objects now.** `xPV/xSP/xDuty/xErr`
  and `xSPset/xKp/xKi` are `vscope=global` and can be written from any page.
  `tState`, `tLed`, `tMode`, `tAlarm`, `tPI` and the waveform `s0` are still
  page-local, so writing them while another page is up returns `1A`. The
  firmware tracks the page from the nav codes `p0b10/11/12` and gates only
  those. Monitor is page **0** and the boot page - do not assume page 1.
- **9600 baud is only ~960 byte/s, and that is a real constraint.** Four
  `x.val=` writes per 250 ms cycle is already a quarter of the link. Send the
  numbers every cycle and the text and colours only when they change,
  otherwise the queue backs up and the screen lags behind the plant.
- **Never leave `bkcmd=1` on.** It makes the panel return a bare `0x01` after
  every command, and those acks are single bytes with no terminator. Four
  telemetry writes per 250 ms cycle fill a 64-byte frame buffer in about four
  seconds, after which every arriving byte is discarded and no touch code is
  ever parsed again. The failure is silent and deeply misleading: telemetry is
  TX-only so the screen keeps updating perfectly, and the dead-man stays happy
  because bytes *are* arriving - only the parser is jammed. It presents as
  "the buttons do nothing". Use `bkcmd=0`, and make buffer overflow reset the
  frame rather than drop the bytes that would have ended it.
- **Do not poll the UART from the control loop.** The MAX6675 conversion is
  220 ms of a 250 ms cycle and the telemetry writes are blocking on top of it,
  so a polled receive gets almost no time, drops bytes, and the dead-man trips
  at random - it looks exactly like a flaky panel or a bad wire. `MODE=nextion`
  claims `USART1_IRQn` (unused by this project's MSP) and receives into a ring
  buffer instead. A cycle that overruns also resyncs its deadline rather than
  chasing it, so it never spirals.
- **The waveform `add` command takes a single byte.** `add 1,0,<y>` cannot
  address rows above 255, so with the contract's 2.8 px/°C scale the trace
  clips at about 91 °C. Enough for bench work; a reflow profile will need
  either a coarser scale or `addt` bulk transfer.

### Measuring things - lessons

Two of our own instruments gave confidently wrong answers:

- **HAL calls inside a timing loop measure themselves.** A pin rise-time test
  using `HAL_GPIO_ReadPin` returned a constant 40 cycles regardless of the
  pin, because that *was* the loop overhead. Use raw register access
  (`GPIOA->IDR`, `GPIOA->BSRR`) and measure the floor explicitly.
- **`HAL_UART_Transmit_IT` needs the USART1 NVIC line**, which this firmware's
  MSP never enables. It queues a transfer that can never run, leaves `gState`
  stuck at `BUSY_TX`, and silently turns every later `HAL_UART_Transmit` into
  a no-op. Poll `TXE` and write `DR` directly.

---

## Safety

Present in firmware and worth preserving:

- Any sensor fault forces duty to 0. A controller that cannot see temperature
  must not drive a heater.
- Above `OVERTEMP` (default 120 °C) the output **latches off** until reset.
  Temperature keeps being published while latched - that is exactly when you
  want to see it.
- Every profile stage times out rather than sitting at full power forever.
- **Dead-man switch:** the firmware only heats while it receives a heartbeat,
  and the bridge only relays that heartbeat while a client has called the API
  within 2.5 s. Close the browser tab and the output stops within seconds.
  After a trip the firmware clears its own enable flag, so heating never
  resumes just because a browser reconnected - and `push_command` refreshes
  the heartbeat *before* the seq write, otherwise an explicit Start would be
  latched and cleared in the same cycle and nothing would ever re-arm.
- The host scripts force PA8 low through the debugger before and after every
  run, so neither a crash nor a halted core can leave the element energised.
- `MAX_DUTY` and `OVERTEMP` are compiled in; the UI cannot raise them.

The bridge binds to 127.0.0.1 only. Do not expose it - there is no auth and
the far end is a 160 W heater on mains-derived power.

---

## Suggested next steps

1. Run `MODE=nextion` on the bench and confirm the arming path end to end:
   Start turns `tState` green and raises duty, Stop drops it, Apply moves the
   setpoint. The semihosting log prints every touch code it decodes, so a
   button that does nothing is easy to tell from a button that was never sent.
2. Fix the trend waveform's 91 °C ceiling - see the `add` note above.
3. Re-characterise the plant once the element position and insulation are
   final, then set gains and the duty ceiling from that.
5. Implement proper reflow profiles on top of the working setpoint loop.
