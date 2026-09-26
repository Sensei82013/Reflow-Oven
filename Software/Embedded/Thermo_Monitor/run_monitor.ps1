# PowerShell equivalent of run_monitor.sh, for running from the Windows
# PowerShell prompt (PowerShell cannot execute .sh files directly).
#
#   .\run_monitor.ps1              # flash, start, stream 15s of output
#   .\run_monitor.ps1 -Seconds 60  # stream for longer
#   .\run_monitor.ps1 -RunOnly     # don't reflash, just restart and stream
#
# Only the ST-Link needs to be connected: it powers the board through J2 pin 1
# and carries the semihosting output. USB is optional.

param(
    [int]$Seconds = 15,
    [switch]$RunOnly,
    # OpenOCD writes everything to stderr, and PowerShell 5.1 turns that into
    # NativeCommandError the moment you pipe the output anywhere. Pass
    # -LogFile to have OpenOCD write to a file instead, which is then printed
    # as ordinary strings you can pipe, filter or keep.
    [string]$LogFile
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$cli = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107\tools\bin\STM32_Programmer_CLI.exe'
$ocd = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.200.202505051030\tools\bin\openocd.exe'
$scr = 'C:\ST\STM32CubeIDE_1.15.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.debug.openocd_2.3.100.202501240831\resources\openocd\st_scripts'
$bin = Join-Path $here 'build\thermo_monitor.bin'
$cfg = Join-Path $here 'openocd.cfg'

if (-not (Test-Path $bin)) {
    throw "$bin not found - run 'make' first (from Git Bash, or the STM32CubeIDE build)."
}

if (-not $RunOnly) {
    Write-Host "Flashing $bin ..." -ForegroundColor Cyan
    & $cli -c port=SWD mode=UR -w $bin 0x08000000 -v | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Flashing failed (exit $LASTEXITCODE)." }
}

# Clear the breakpoint CubeProgrammer's flash loader leaves armed, reset, then
# let it run with semihosting serviced so the temperature lines come through.
$ocdArgs = @(
    '-s', $scr, '-f', $cfg,
    '-c', 'init',
    '-c', 'halt',
    '-c', 'rbp all',
    '-c', 'rwp all',
    '-c', 'reset halt',
    '-c', 'arm semihosting enable',
    '-c', 'resume',
    '-c', "sleep $($Seconds * 1000)",
    '-c', 'halt',
    '-c', 'mdw 0x20000000 8',
    '-c', 'shutdown'
)
Write-Host "Running for $Seconds s ..." -ForegroundColor Cyan

if ($LogFile) {
    # OpenOCD writes everything, including the semihosting output, to stderr.
    # Start-Process captures it to a file without PowerShell turning each line
    # into an ErrorRecord, so the result can be piped and filtered normally.
    $out = "$LogFile.stdout"
    # Start-Process does not quote array elements, and both the script path and
    # the multi-word '-c' commands contain spaces, so quote them here.
    $quoted = $ocdArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $ocd -ArgumentList ($quoted -join ' ') -Wait -NoNewWindow `
                       -RedirectStandardError $LogFile -RedirectStandardOutput $out -PassThru
    Get-Content $LogFile, $out -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { Write-Host "openocd exited $($p.ExitCode)" -ForegroundColor Yellow }
} else {
    & $ocd @ocdArgs
}
