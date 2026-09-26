#!/bin/sh
# Flash and start the MAX6675 monitor, then stream its semihosting output.
#
# With SW1 in the GND position (BOOT0 low) the part boots from flash normally,
# so a plain 'reset halt' is enough. Note the SWD header J2 carries only
# 3V3/SWDIO/SWCLK/GND -- there is no NRST line to the probe -- so this reset
# goes through the Cortex-M SYSRESETREQ bit (see openocd.cfg).
#
# Stale breakpoints are cleared first: CubeProgrammer's flash loader leaves one
# armed, and it otherwise traps the core the moment it starts running.
#
# Usage: ./run_monitor.sh [seconds] [--run-only]
set -e
HERE=$(dirname "$0")
SECS=${1:-15}

CLI="C:/ST/STM32CubeIDE_1.15.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107/tools/bin/STM32_Programmer_CLI.exe"
OCD="C:/ST/STM32CubeIDE_1.15.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.200.202505051030/tools/bin/openocd.exe"
SCR="C:/ST/STM32CubeIDE_1.15.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.debug.openocd_2.3.100.202501240831/resources/openocd/st_scripts"
BIN="$HERE/build/thermo_monitor.bin"

if [ "$2" != "--run-only" ]; then
  "$CLI" -c port=SWD mode=UR -w "$BIN" 0x08000000 -v >/dev/null
fi

exec "$OCD" -s "$SCR" -f "$HERE/openocd.cfg" -c "init" -c "halt" -c "rbp all" -c "rwp all" -c "reset halt" -c "arm semihosting enable" -c "resume" -c "sleep ${SECS}000" -c "halt" -c "mdw 0x20000000 8" -c "shutdown"
