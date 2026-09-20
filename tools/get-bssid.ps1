# 查询「公司 WiFi 热点的 BSSID」——也就是 ESP32 配置页里 MAC 那一栏该填的值。
#
# 为什么要这个脚本：
#   netsh wlan show interfaces / show networks 读取 BSSID 需要
#   (1) 管理员权限  (2) 系统「位置服务」打开，直接跑会报 error 5 或提示 location permission。
#   脚本会自动请求提权。
#
# 用法（pwsh = PowerShell 7；没装 pwsh 就用 powershell，Win 自带的 5.1 也能跑）:
#   powershell -ExecutionPolicy Bypass -File tools\get-bssid.ps1
#   powershell -ExecutionPolicy Bypass -File tools\get-bssid.ps1 -SSID 你的SSID   # 只关心某个 SSID
#   powershell -ExecutionPolicy Bypass -File tools\get-bssid.ps1 -NoElevate       # 不提权，只看能看到的部分
#
# 输出的 MAC 填进 http://192.168.4.1 配置页的 MAC 输入框（必须是 aa:bb:cc:dd:ee:ff 共 17 字符）。

param(
    [string]$SSID = '',
    [switch]$NoElevate,
    [switch]$NoPause
)

$ErrorActionPreference = 'Continue'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Band([string]$channel) {
    $c = 0
    if (-not [int]::TryParse($channel, [ref]$c)) { return '?' }
    if ($c -ge 1 -and $c -le 14) { return '2.4G' }
    if ($c -ge 36) { return '5G' }
    return '?'
}

# ---- 当前连着的那个 WiFi（最准的一个值） ----
function Get-CurrentWlan {
    $raw = (netsh wlan show interfaces 2>&1 | Out-String)
    $rec = [PSCustomObject]@{ SSID = ''; BSSID = ''; Channel = ''; Band = ''; Radio = ''; Raw = $raw }
    foreach ($l in ($raw -split "`r?`n")) {
        if ($l -match '^\s*BSSID\s*:\s*(\S+)') { $rec.BSSID = $Matches[1]; continue }
        if ($l -match '^\s*SSID(?:\s*名称)?\s*:\s*(.+?)\s*$') { $rec.SSID = $Matches[1]; continue }
        if ($l -match '^\s*(?:信道|Channel)\s*:\s*(\d+)') { $rec.Channel = $Matches[1]; continue }
        if ($l -match '^\s*(?:无线电类型|Radio type)\s*:\s*(.+?)\s*$') { $rec.Radio = $Matches[1]; continue }
    }
    $rec.Band = Get-Band $rec.Channel
    return $rec
}

# ---- 扫描到的所有热点（没连上也能看到 BSSID） ----
function Get-NearbyWlan {
    $raw = (netsh wlan show networks mode=bssid 2>&1 | Out-String)
    if ($raw -match 'location permission|位置权限|位置服务|Location services') {
        return [PSCustomObject]@{ Error = 'location'; Raw = $raw; Items = @() }
    }
    $items = @()
    $cur = ''
    foreach ($l in ($raw -split "`r?`n")) {
        if ($l -match '^\s*SSID\s+\d+\s*:\s*(.*)$') { $cur = $Matches[1].Trim(); continue }
        if ($l -match '^\s*BSSID\s+\d+\s*:\s*(\S+)') {
            $items += [PSCustomObject]@{ SSID = $cur; BSSID = $Matches[1]; Channel = ''; Band = ''; Signal = ''; Radio = '' }
            continue
        }
        if ($items.Count -gt 0) {
            $last = $items[$items.Count - 1]
            if ($l -match '^\s*(?:信道|Channel)\s*:\s*(\d+)') { $last.Channel = $Matches[1]; $last.Band = Get-Band $Matches[1]; continue }
            if ($l -match '^\s*(?:信号|Signal)\s*:\s*(\S+)') { $last.Signal = $Matches[1]; continue }
            if ($l -match '^\s*(?:无线电类型|Radio type)\s*:\s*(.+?)\s*$') { $last.Radio = $Matches[1]; continue }
        }
    }
    return [PSCustomObject]@{ Error = ''; Raw = $raw; Items = $items }
}

# ---- 以前连过的网络（注册表，仅供参考：这里是网关 MAC，不一定等于 BSSID） ----
function Get-HistoryNetwork {
    $base = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\NetworkList'
    $names = @{}
    Get-ChildItem "$base\Profiles" -ErrorAction SilentlyContinue | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        if ($p -and $p.ProfileName) { $names[$_.PSChildName] = $p.ProfileName }
    }
    $out = @()
    Get-ChildItem "$base\Signatures\Unmanaged" -ErrorAction SilentlyContinue | ForEach-Object {
        $s = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        if ($s -and $s.DefaultGatewayMac) {
            $mac = (($s.DefaultGatewayMac | ForEach-Object { $_.ToString('X2') }) -join ':')
            $out += [PSCustomObject]@{ SSID = $names[$s.ProfileGuid]; GatewayMAC = $mac }
        }
    }
    return $out
}

function Show-LocationHint {
    Write-Host ''
    Write-Host '⚠ 系统「位置服务」没开，netsh 拿不到 BSSID。请打开：' -ForegroundColor Yellow
    Write-Host '   设置 → 隐私和安全性 → 位置 → 打开「位置服务」，并打开「让桌面应用访问你的位置」' -ForegroundColor Yellow
    Write-Host '   然后重新运行本脚本：' -ForegroundColor Yellow
    Write-Host '   start ms-settings:privacy-location' -ForegroundColor DarkGray
}

# ============================ 主流程 ============================

if (-not (Test-Admin) -and -not $NoElevate) {
    Write-Host '读取 WLAN BSSID 需要管理员权限，正在请求提权（会弹 UAC）...' -ForegroundColor Yellow
    $exe = (Get-Process -Id $PID).Path
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($SSID) { $argList += @('-SSID', $SSID) }
    try {
        Start-Process -FilePath $exe -Verb RunAs -ArgumentList $argList | Out-Null
    } catch {
        Write-Host "提权被取消或失败：$_" -ForegroundColor Red
    }
    return
}

Write-Host ''
Write-Host '================ 1. 本机当前连接的热点（最推荐用这个） ================' -ForegroundColor Cyan
$cur = Get-CurrentWlan
if ($cur.Raw -match 'elevation|管理员') {
    Write-Host '  netsh 需要管理员权限。请右键 PowerShell → 以管理员身份运行，或去掉 -NoElevate 重新执行。' -ForegroundColor Yellow
} elseif (-not $cur.BSSID) {
    Write-Host '  本机现在没有连 WiFi（或没连上）。没有关系 —— 看下面第 2 段，或直接用手机看。' -ForegroundColor Yellow
} else {
    Write-Host ("  SSID     : {0}" -f $cur.SSID)
    Write-Host ("  BSSID    : {0}" -f $cur.BSSID) -ForegroundColor Green
    Write-Host ("  频段/信道: {0} / {1}   ({2})" -f $cur.Band, $cur.Channel, $cur.Radio)
    Write-Host ''
    Write-Host ('  → 要填进 ESP32 配置页的值： ' + $cur.BSSID) -ForegroundColor Green
}

Write-Host ''
Write-Host '================ 2. 附近扫到的热点（没连上也能看到 BSSID） ================' -ForegroundColor Cyan
$near = Get-NearbyWlan
if ($near.Error -eq 'location') {
    Show-LocationHint
} else {
    $rows = $near.Items
    if ($SSID) { $rows = $rows | Where-Object { $_.SSID -like "*$SSID*" } }
    if ($rows.Count -eq 0) {
        Write-Host '  没有扫到匹配的热点（检查 SSID 过滤条件，或打开位置服务后重试）。' -ForegroundColor Yellow
    } else {
        # 同一个 SSID 的多频段：先列 2.4G，方便挑
        $rows = $rows | Sort-Object SSID, Band
        $rows | Format-Table SSID, BSSID, Band, Channel, Signal, Radio -AutoSize | Out-String | Write-Host
        Write-Host '  提示：ESP32-C3 只支持 2.4GHz。但钉钉只比对 SSID + BSSID 字符串、不看频段，' -ForegroundColor DarkGray
        Write-Host '        所以「和钉钉登记的那个一致」才是唯一标准 —— 通常是手机连公司 WiFi 时显示的 BSSID。' -ForegroundColor DarkGray
    }
}

Write-Host ''
Write-Host '================ 3. 历史连接记录（仅供参考，不等于 BSSID） ================' -ForegroundColor Cyan
$hist = Get-HistoryNetwork
if (-not $hist -or $hist.Count -eq 0) {
    Write-Host '  读不到（需要管理员权限，或系统里没有历史记录）。' -ForegroundColor DarkGray
} else {
    if ($SSID) { $hist = $hist | Where-Object { $_.SSID -like "*$SSID*" } }
    $hist | Sort-Object SSID | Format-Table SSID, GatewayMAC -AutoSize | Out-String | Write-Host
    Write-Host '  ⚠ 这里是「默认网关的 MAC」，和 WiFi 的 BSSID 常常不同（多频段/多网口的路由器尤其如此），' -ForegroundColor Yellow
    Write-Host '     只能当线索，不要直接拿去填。' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '================ 手机端怎么读（最省事） ================' -ForegroundColor Cyan
Write-Host '  Android : 设置 → WLAN → 点当前这个网络 → 详情/BSSID              （这个值是 AP 的）'
Write-Host '  iPhone  : 设置 → 无线局域网 → 点右边 ⓘ →「Wi-Fi 地址」         （⚠ 这个值是手机的，不是 BSSID！）'
Write-Host '  iPhone 读不到 BSSID：用 Mac 连同一 WiFi，按住 Option 点菜单栏 WiFi 图标即可看到 BSSID。'
Write-Host '  路由器后台：无线设置页里的「BSSID / 本机 MAC / 2.4G MAC」也是同一个东西。'

if (-not $NoPause) {
    Write-Host ''
    Read-Host '按回车关闭'
}
