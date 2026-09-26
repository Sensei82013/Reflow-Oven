# Build, flash and run a heater profile, capturing the CSV it prints.
#
#   .\run_profile.ps1 -Mode step            # open-loop step, for tuning
#   .\run_profile.ps1 -Mode pi              # closed-loop PI profile
#   .\run_profile.ps1 -Mode pi -Kp 8 -Ki 0.4
#   .\run_profile.ps1 -Mode monitor         # no heating, just temperature
#
# One command end to end, because PowerShell 5.1 has no '&&' to chain with.
# `make` only exists inside Git Bash, so the build is shelled out to it.
#
# SAFETY: PA8 is forced low through the debugger both before and after every
# run, so neither a crash nor a halted core can leave the element energised.
# The firmware has its own cutoffs (sensor fault, over-temperature, stage
# timeout), but keep the 24 V supply within reach anyway.

param(
    [ValidateSet('step', 'pi', 'hold', 'monitor', 'hmi', 'tx', 'rx')]
    [string]$Mode = 'pi',
    [int]$Seconds = 0,
    [double]$Kp = 0,
    [double]$Ki = 0,
    [double]$StepDuty = 0,
    [double]$MaxDuty = 0,
    [int]$PwmHz = 0,
    [int]$BaseSeconds = 0,
    [int]$HeatSeconds = 0,
    [int]$CoolSeconds = 0,
    [int]$DwellSeconds = 0,
    [string]$Csv,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$cli  = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107\tools\bin\STM32_Programmer_CLI.exe'
$ocd  = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.200.202505051030\tools\bin\openocd.exe'
$scr  = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.debug.openocd_2.3.100.202501240831\resources\openocd\st_scripts'
$bash = 'C:\Program Files\Git\bin\bash.exe'
$bin  = Join-Path $here 'build\thermo_monitor.bin'
$cfg  = Join-Path $here 'openocd.cfg'

if ($Seconds -le 0) {
    if ($Mode -eq 'step')    { $Seconds = 80 }
    elseif ($Mode -eq 'hold') { $Seconds = 60 }
    elseif ($Mode -eq 'hmi')  { $Seconds = 45 }
    elseif ($Mode -eq 'tx')   { $Seconds = 120 }
    elseif ($Mode -eq 'rx')   { $Seconds = 120 }
    elseif ($Mode -eq 'pi')  { $Seconds = 200 }
    else                     { $Seconds = 30 }
}

# Park PA8 as a driven low with the core halted: enable the GPIOA clock, clear
# the output bit, then switch the pin from its reset state to a push-pull
# output. Done in that order so the pin never glitches high on the way.
$killArgs = @(
    '-c', 'reset halt',
    '-c', 'mww 0x40021018 0x00000004',   # RCC_APB2ENR: IOPAEN
    '-c', 'mww 0x40010814 0x00000100',   # GPIOA_BRR:   PA8 = 0
    '-c', 'mww 0x40010804 0x44444442'    # GPIOA_CRH:   PA8 = output PP 2MHz
)

function Invoke-Ocd([string[]]$cmds, [string]$logPath) {
    $all = @('-s', $scr, '-f', $cfg, '-c', 'init', '-c', 'halt',
             '-c', 'rbp all', '-c', 'rwp all') + $cmds + @('-c', 'shutdown')
    # Start-Process does not quote array elements, and paths and multi-word
    # '-c' commands both contain spaces.
    $quoted = $all | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $out = "$logPath.stdout"
    $p = Start-Process -FilePath $ocd -ArgumentList ($quoted -join ' ') -Wait -NoNewWindow `
                       -RedirectStandardError $logPath -RedirectStandardOutput $out -PassThru
    return $p.ExitCode
}

if (-not $NoBuild) {
    if (-not (Test-Path $bash)) { throw "Git Bash not found at $bash - build with 'make' manually." }
    $mk = 'make -B'
    if ($Mode -ne 'monitor') { $mk += " MODE=$Mode" }
    if ($Kp -gt 0)           { $mk += " KP=$Kp" }
    if ($Ki -gt 0)           { $mk += " KI=$Ki" }
    if ($StepDuty -gt 0)     { $mk += " STEP_DUTY=$StepDuty" }
    if ($MaxDuty -gt 0)      { $mk += " MAX_DUTY=$MaxDuty" }
    if ($PwmHz -gt 0)        { $mk += " PWM_HZ=$PwmHz" }
    if ($BaseSeconds -gt 0)  { $mk += " BASE_S=$BaseSeconds" }
    if ($HeatSeconds -gt 0)  { $mk += " HEAT_S=$HeatSeconds" }
    if ($CoolSeconds -gt 0)  { $mk += " COOL_S=$CoolSeconds" }
    if ($DwellSeconds -gt 0) { $mk += " DWELL_S=$DwellSeconds" }

    $unix = ($here -replace '\\', '/') -replace '^([A-Za-z]):', '/$1'
    Write-Host "Building: $mk" -ForegroundColor Cyan
    & $bash -lc "cd '$unix' && export PATH=/c/ST/STM32CubeIDE_1.15.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.make.win32_2.1.300.202402091052/tools/bin:`$PATH && $mk" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }
}

$log = Join-Path $env:TEMP "reflow_$Mode.log"

Write-Host "Heater off before starting ..." -ForegroundColor Cyan
Invoke-Ocd $killArgs "$log.pre" | Out-Null

Write-Host "Flashing ..." -ForegroundColor Cyan
& $cli -c port=SWD mode=UR -w $bin 0x08000000 -v | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Flashing failed (exit $LASTEXITCODE)." }

Write-Host "Running '$Mode' for up to $Seconds s - keep the 24 V supply in reach." -ForegroundColor Yellow
$runArgs = @('-c', 'reset halt', '-c', 'arm semihosting enable', '-c', 'resume',
             '-c', "sleep $($Seconds * 1000)", '-c', 'halt') + $killArgs
Invoke-Ocd $runArgs $log | Out-Null

$lines = Get-Content $log, "$log.stdout" -ErrorAction SilentlyContinue
$lines
Write-Host "Heater off." -ForegroundColor Green

if ($Csv) {
    # Keep the header and any row that starts with a millisecond timestamp.
    $rows = $lines | Where-Object { $_ -match '^\s*(t_ms,|\d+,)' } | ForEach-Object { $_.Trim() }
    Set-Content -Path $Csv -Value $rows -Encoding utf8
    Write-Host "$($rows.Count) rows -> $Csv" -ForegroundColor Green
}
