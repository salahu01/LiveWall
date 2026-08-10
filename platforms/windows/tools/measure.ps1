<#
.SYNOPSIS
    Samples LiveWall's real cost, so the low-resource claim is checkable rather
    than asserted.

.DESCRIPTION
    Run it once with the desktop visible, then again with a full-screen window
    covering the desktop. The second run is the number that matters — it should
    read 0.0% CPU and a working set back near the idle baseline.

    Memory is reported as private working set, which is what Task Manager's
    Details tab shows under "Memory (private working set)" and the closest
    counter to the "footprint" figure the macOS README quotes. The Processes
    tab's "Memory" column is a different number that folds in shared graphics
    pages this process does not own.

    CPU is a percentage of one core, measured as the delta in process CPU time
    over the sample interval. On a 12-core machine, 3% of a core is 0.25% of the
    machine.

.EXAMPLE
    ./tools/measure.ps1
    ./tools/measure.ps1 -Samples 60
#>
[CmdletBinding()]
param(
    [int]$Samples = 20,
    [double]$IntervalSeconds = 1.0
)

$ErrorActionPreference = 'Stop'

$process = Get-Process -Name 'LiveWall' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $process) {
    Write-Error "LiveWall is not running."
    exit 1
}

Write-Host ("pid {0} - {1} samples, {2}s apart" -f $process.Id, $Samples, $IntervalSeconds)
Write-Host ("{0,8}  {1,12}  {2,8}" -f 'sample', 'working set', 'cpu%')

$previousCpu = $process.TotalProcessorTime
$totalCpu = 0.0
$peakMemory = 0

for ($i = 1; $i -le $Samples; $i++) {
    Start-Sleep -Seconds $IntervalSeconds
    $process.Refresh()

    if ($process.HasExited) {
        Write-Warning "LiveWall exited during measurement."
        break
    }

    $currentCpu = $process.TotalProcessorTime
    # A percentage of one core: CPU seconds consumed divided by wall seconds
    # elapsed. Get-Counter's "% Processor Time" would divide by the core count
    # and report a number 12x smaller on a 12-core machine, which is a different
    # claim from the one the macOS README makes.
    $cpuPercent = (($currentCpu - $previousCpu).TotalSeconds / $IntervalSeconds) * 100
    $previousCpu = $currentCpu

    $memory = $process.PrivateMemorySize64
    if ($memory -gt $peakMemory) { $peakMemory = $memory }
    $totalCpu += $cpuPercent

    Write-Host ("{0,8}  {1,12}  {2,8:N2}" -f $i, ("{0:N1} MB" -f ($memory / 1MB)), $cpuPercent)
}

Write-Host ""
Write-Host ("mean cpu: {0:N2}%  of one core" -f ($totalCpu / $Samples))
Write-Host ("peak memory: {0:N1} MB" -f ($peakMemory / 1MB))
Write-Host ""
Write-Host "For GPU draw, open Task Manager > Performance > GPU, or run:"
Write-Host "  Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage'"
