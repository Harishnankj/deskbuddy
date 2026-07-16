$cpu = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
$os = Get-CimInstance Win32_OperatingSystem
$totalRam = $os.TotalVisibleMemorySize
$freeRam = $os.FreePhysicalMemory
$ram = [math]::Round((($totalRam - $freeRam) / $totalRam) * 100)

$gpu = (Get-Counter '\GPU Engine(*_3D)\Utilization Percentage' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty CounterSamples | Measure-Object -Property CookedValue -Max).Maximum
if (-not $gpu) { $gpu = 0 }
$gpu = [math]::Round($gpu)

$cpuTemp = -1
try {
    $tempObj = Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction Stop
    $cpuTemp = [math]::Round(($tempObj.CurrentTemperature / 10) - 273.15)
} catch {
    $cpuTemp = -1
}

$gpuTemp = -1
try {
    $nv = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($nv) {
        $nvTemp = nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits
        $gpuTemp = [int]$nvTemp.Trim()
    }
} catch {
    $gpuTemp = -1
}

$stats = @{
    cpu = [int]$cpu
    ram = [int]$ram
    gpu = [int]$gpu
    cpuTemp = [int]$cpuTemp
    gpuTemp = [int]$gpuTemp
}

$stats | ConvertTo-Json -Compress
