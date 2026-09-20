# 抓取 ESP32-C3 串口开机日志（原生 USB Serial/JTAG）
# 用法: pwsh -File tools\boot-log.ps1 [-Port COM4] [-Seconds 10]
param(
    [string]$Port = 'COM4',
    [int]$Baud = 115200,
    [int]$Seconds = 10
)

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 200
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
} catch {
    Write-Error "无法打开 $Port : $_"
    exit 1
}

Start-Sleep -Milliseconds 300

# 经典复位时序：DTR 控制 GPIO0(BOOT)，RTS 控制 EN(RESET)
$sp.DtrEnable = $false   # GPIO0 = HIGH -> 正常启动，不进下载模式
$sp.RtsEnable = $true    # EN = LOW  -> 芯片复位
Start-Sleep -Milliseconds 150
$sp.RtsEnable = $false   # EN = HIGH -> 释放复位

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$buf = New-Object System.Text.StringBuilder
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk) { [void]$buf.Append($chunk) }
    } catch { }
    Start-Sleep -Milliseconds 50
}

$sp.Close()
Write-Output $buf.ToString()
