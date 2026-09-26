# Build (if needed), flash (only if the chip does not already have this
# image), start OpenOCD, and serve the web app.
#
#   .\run_webapp.ps1                          # Kp 12, Ki 0.6, 1 Hz PWM
#   .\run_webapp.ps1 -Kp 6 -Ki 0.2 -Setpoint 40
#   .\run_webapp.ps1 -NoBuild                 # skip the build entirely
#   .\run_webapp.ps1 -ForceFlash              # reflash even if unchanged
#
# Then open http://127.0.0.1:8770/ - the page is served by the bridge, not
# from a file, so its fetch() calls reach the board.
#
# -Setpoint/-Kp/-Ki only seed the loop; the page changes all three at runtime.
# The MCU owns the duty cycle - the page never sends one, it only watches.
# -MaxDuty and -OverTemp are compiled in as hard limits the page cannot raise.
#
# Ctrl-C stops everything. The firmware stops heating on its own if this
# script or the browser goes away, because the heartbeat stops.

param(
    [double]$Setpoint = 30,
    [double]$Kp = 0.2,
    [double]$Ki = 0.002,
    [int]$PwmHz = 1,
    # Build-time safety ceiling, NOT a web input. This element can carry the
    # thermocouple 80 C past setpoint on stored heat alone, so the loop should
    # not be allowed full power while commissioning.
    [double]$MaxDuty = 100,
    [int]$OverTemp = 120,
    [switch]$NoBuild,
    [switch]$ForceFlash
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$cli  = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107\tools\bin\STM32_Programmer_CLI.exe'
$ocd  = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.200.202505051030\tools\bin\openocd.exe'
$scr  = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.debug.openocd_2.3.100.202501240831\resources\openocd\st_scripts'
$bash = 'C:\Program Files\Git\bin\bash.exe'
$bin  = Join-Path $here 'build\thermo_monitor.bin'
$cfg  = Join-Path $here 'openocd.cfg'
$py   = (Get-Command python -ErrorAction SilentlyContinue).Source

if (-not $py) { throw "python not found on PATH." }

if (-not $NoBuild) {
    # Only PWM_HZ is baked in. Setpoint and both gains are runtime values the
    # page can change, so they never reach the compiler.
    $mk = "make MODE=server PWM_HZ=$PwmHz MAX_DUTY=$MaxDuty OVERTEMP=$OverTemp"
    $unix = ($here -replace '\\', '/') -replace '^([A-Za-z]):', '/$1'
    Write-Host "Building: $mk" -ForegroundColor Cyan
    & $bash -lc "cd '$unix' && export PATH=/c/ST/STM32CubeIDE_1.15.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.make.win32_2.1.300.202402091052/tools/bin:`$PATH && $mk" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }
}

if (-not (Test-Path $bin)) { throw "$bin not found - run without -NoBuild once." }

# Read back what is actually on the chip and compare. A needless erase/program
# cycle costs a couple of seconds on every start and resets the board out from
# under whatever was running.
function Test-FlashCurrent {
    $len = (Get-Item $bin).Length
    $readback = Join-Path $env:TEMP 'reflow_readback.bin'
    Remove-Item $readback -ErrorAction SilentlyContinue
    & $cli -c port=SWD mode=UR -r 0x08000000 $len $readback 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $readback)) { return $false }
    $a = [System.IO.File]::ReadAllBytes($bin)
    $b = [System.IO.File]::ReadAllBytes($readback)
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}

if ($ForceFlash) {
    $needFlash = $true
} else {
    Write-Host "Checking what is on the chip ..." -ForegroundColor Cyan
    $needFlash = -not (Test-FlashCurrent)
}

if ($needFlash) {
    Write-Host "Flashing ..." -ForegroundColor Cyan
    & $cli -c port=SWD mode=UR -w $bin 0x08000000 -v | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Flashing failed (exit $LASTEXITCODE)." }
} else {
    Write-Host "Firmware already up to date - skipping flash." -ForegroundColor Green
}

# OpenOCD stays resident with its TCL port open; the bridge drives it from
# there. No 'sleep' here - the target runs free and is read through the DAP.
$ocdArgs = @(
    '-s', $scr, '-f', $cfg,
    '-c', 'tcl_port 6666', '-c', 'gdb_port disabled', '-c', 'telnet_port disabled',
    '-c', 'init', '-c', 'halt', '-c', 'rbp all', '-c', 'rwp all',
    '-c', 'reset halt', '-c', 'arm semihosting enable', '-c', 'resume'
) | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }

$ocdLog = Join-Path $env:TEMP 'reflow_webapp_ocd.log'
Write-Host "Starting OpenOCD ..." -ForegroundColor Cyan
$ocdProc = Start-Process -FilePath $ocd -ArgumentList ($ocdArgs -join ' ') `
    -NoNewWindow -PassThru -RedirectStandardError $ocdLog -RedirectStandardOutput "$ocdLog.out"
Start-Sleep -Milliseconds 1500
if ($ocdProc.HasExited) {
    Get-Content $ocdLog -Tail 20
    throw "OpenOCD exited immediately (exit $($ocdProc.ExitCode))."
}

try {
    Write-Host "`n  Open http://127.0.0.1:8770/  -  Ctrl-C to stop`n" -ForegroundColor Green
    & $py (Join-Path $here 'webapp\bridge.py') --setpoint $Setpoint --kp $Kp --ki $Ki
}
finally {
    Write-Host "Stopping OpenOCD ..." -ForegroundColor Cyan
    if (-not $ocdProc.HasExited) { Stop-Process -Id $ocdProc.Id -Force -ErrorAction SilentlyContinue }
}
