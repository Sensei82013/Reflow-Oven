NX8048P070_011 Reflow-Oven HMI  (Nextion Intelligent 7.0", 800x480)  -- Phase 3 (Xfloat contract)
====================================================================================================

Files
  NX8048P070_011.tft            compiled firmware (Nextion Editor 1.68.1, 0 errors / 0 warnings), upload via SD card or USB-TTL
  NX8048P070_011_project.zip    NX8048P070_011.HMI + Arial24.zi / Arial40.zi / Arial96.zi (font IDs 0/1/2)
  sim_monitor.png               debug-simulator screenshot, monitor page populated by MCU commands

UART
  9600 baud, 8N1. Every MCU -> panel instruction ends with 0xFF 0xFF 0xFF.
  Panel -> MCU messages are ASCII lines ending with CR LF (0x0D 0x0A). "Send Component ID" is OFF everywhere.
  bkcmd is left at the editor default; the firmware sends bkcmd=1 itself.

Pages
  0 monitor   (boot page)   1 setup   2 trend   3 keybdB (auto-generated numeric keyboard)

Panel -> MCU  (all in Touch Release events, emitted with prints "...",0 + printh 0D 0A)
  START  (bStart, nav bar, every page)      p0b20\r\n
  STOP   (bStop, every page)                p0b21\r\n
  MONITOR tab                               p0b10\r\n   then "page monitor"
  SETUP tab                                 p0b11\r\n   then "page setup"
  TREND tab                                 p0b12\r\n   then "page trend"      -> only send "add" while trend is up
  APPLY  (setup page)                       SP=<sp>,KP=<kp>,KI=<ki>\r\n
         sp = xSPset.val = setpoint x10   (300    -> 30.0 C),  range 0..1000,   +/- step 5  (0.5 C)
         kp = xKp.val    = Kp x1000       (2500   -> 2.500),   range 0..200000, +/- step 10 (0.01)
         ki = xKi.val    = Ki x1000       (50     -> 0.050),   range 0..50000,  +/- step 1  (0.001)
         (integers converted to ASCII with covx; verified example: SP=300,KP=2500,KI=50\r\n)
         APPLY and the setup-page START additionally mirror the entered setpoint into the monitor
         SETPOINT tile optimistically:  monitor.xSP.val=xSPset.val*10   (x10 -> x100)
  Nothing else is sent by the panel. The +/- buttons and the keyboard only edit the values
  locally; the MCU only learns about them via APPLY.

MCU -> panel
  Monitor values are Xfloat components, ALL x100 (vvs1=2, vvs0=0 = no leading zeros), ALL vscope=global
  (write them from any page; boot value 0 -> "0.00"; on TC fault send 0):
    xPV.val=2575          temperature  25.75 C
    xSP.val=8500          setpoint     85.00 C     (also written by setup APPLY/START)
    xDuty.val=6230        duty         62.30 %
    xErr.val=235          error         2.35 C
    tPI.txt="41.2 / 21.1 %"   (P / I contribution, Text, local to monitor page)
  Header (every page, same names on every page):
    tState.txt="IDLE" | "HEATING" | "TC FAULT" | "OVER TEMP" | "WARNING"
    tLed.bco=33808 (gray, idle)  2016 (green, heating)  63488 (red, fault/over-temp)  64800 (orange, warning)
    tMode.txt="CONSTANT"
  Alarm banner (monitor page; hidden by default via preinitialize "vis tAlarm,0"):
    tAlarm.txt="OVER-TEMPERATURE - OUTPUT LATCHED OFF"
    vis tAlarm,1          show    /    vis tAlarm,0   hide
  Setup page (xSPset/xKp/xKi are global, writable from any page):
    xSPset.val=300   xKp.val=2500   xKi.val=50
  Trend page waveform s0 (component id 1, x=40 y=130 w=720 h=280, dis=100, grid 70x70):
    add 1,0,<y>           channel 0 = temperature (blue 1055)
    add 1,1,<y>           channel 1 = setpoint    (gray 33808)
    Scaling for the 280 px tall 0..100 C axis: y = clamp(round(temp_c * 2.8), 0, 279).
    NOTE: the Nextion "add" value is a single byte (0..255), so rows 256..279 are not
    addressable and temperatures above 91 C clip at row 255. The "100 C" axis label is
    therefore placed at row 255 (y=142) rather than the very top edge. If the full 0..100 C
    span must be drawable, use y = round(temp_c * 2.55) instead and move tYmax to y=130.
    Send one pair per sample while page trend is up (p0b12 received, no p0b10/p0b11 since).
    cle 1,255             clear both channels
  Page-local components (tPI, tAlarm, tState, tLed, s0) only accept writes while their page is
  shown; otherwise the panel returns an "invalid component" error (0x02/0x1A) when bkcmd>0.
  Gate on the last p0b1x code received.

Component summary
  monitor: tHdr tTitle tMode tState tLed bStop tPVlbl xPV tSPlbl xSP tSPu tDutylbl xDuty tDutyu
           tErrlbl xErr tErru tPI tAlarm bMon bSetup bTrend bStart
  setup:   header/nav as above, tSPsetlbl xSPset bSPm bSPp, tKplbl xKp bKpm bKpp,
           tKilbl xKi bKim bKip, bApply, va0 (string scratch for covx)
  trend:   header/nav as above, s0 (id 1), tYmax tYmin, legend tLeg1 tLeg2 (above the plot)

Colors (RGB565 decimal): background 63422, header navy 6603, white 65535, gray 33808,
  green 2016 / dark green 1408, red 63488, orange 64800, blue 1055.
