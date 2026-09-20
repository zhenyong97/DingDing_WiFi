const popup = document.getElementById('popup');
const popupText = document.getElementById('message');
const form = document.getElementById('info_form');

form.onsubmit = async (event) => {
    event.preventDefault();
    const data = new URLSearchParams(new FormData(form));
    try {
        const response = await fetch('/', {
            method: 'POST',
            body: data,
        });
        if (response.status === 200) {
            toast(await response.text());
        } else {
            toast("发送失败 " + response.statusText);
        }
    } catch (e) {
        // 提交成功后板子会重建热点，手机此时必然掉线，fetch 会直接抛错。
        // 这其实是"成功"的典型表现，所以给个正向提示，而不是静默失败。
        toast("已提交。热点正在重建，请重新连接 WiFi 确认新名字");
    }
};

function toast(message) {
    popup.style.visibility = 'visible';
    popupText.textContent = message;
    setTimeout(() => {
        popup.style.visibility = 'hidden';
    }, 10000);
}

/* ==================== 扫描附近 WiFi ==================== */

const scanBtn = document.getElementById('scan_btn');
const scanStatus = document.getElementById('scan_status');
const scanResult = document.getElementById('scan_result');
const ssidInput = document.getElementById('ssid');
const macInput = document.getElementById('mac');

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function bandOf(channel) {
    if (channel >= 1 && channel <= 14) return '2.4G';
    if (channel >= 36) return '5G';
    return '?';
}

function signalText(rssi) {
    if (rssi >= -55) return '很强';
    if (rssi >= -67) return '良好';
    if (rssi >= -75) return '一般';
    return '偏弱';
}

async function getJson(url, timeoutMs) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeoutMs || 5000);
    try {
        const res = await fetch(url, { cache: 'no-store', signal: ctrl.signal });
        return await res.json();
    } finally {
        clearTimeout(timer);
    }
}

function triggerScan() {
    // 故意不 await：扫描是异步的，触发完立刻返回，下面靠轮询取结果。
    return fetch('/scan?start=1', { cache: 'no-store' }).catch(() => { });
}

async function doScan() {
    scanBtn.disabled = true;
    scanResult.textContent = '';
    scanStatus.textContent = '正在扫描…（约 2–6 秒；扫描期间热点可能短暂卡顿，属正常现象）';

    await triggerScan();

    const deadline = Date.now() + 20000;
    let data = null;

    while (Date.now() < deadline) {
        await sleep(800);
        try {
            const res = await getJson('/scan');
            if (res.status === 'done') {
                data = res;
                break;
            }
            if (res.status === 'idle') {
                // 扫描被判超时/没起来，再触发一次
                await triggerScan();
            }
        } catch (e) {
            // 扫描会让 AP 短暂抖动，这里失败属预期，继续轮询即可
        }
    }

    scanBtn.disabled = false;

    if (!data) {
        scanStatus.textContent = '扫描超时了。让手机重连一下这个热点，然后再点一次「开始扫描」。';
        return;
    }

    renderNetworks(data.networks || []);
}

function renderNetworks(list) {
    const seen = new Set();
    const rows = list
        .filter((n) => n && n.bssid)
        .filter((n) => {
            if (seen.has(n.bssid)) return false;
            seen.add(n.bssid);
            return true;
        })
        .sort((a, b) => (b.rssi || -999) - (a.rssi || -999));

    scanResult.textContent = '';

    if (rows.length === 0) {
        scanStatus.textContent = '没扫到任何热点，再点一次「开始扫描」试试。';
        return;
    }

    scanStatus.textContent = `扫到 ${rows.length} 个 2.4GHz 热点，点右边按钮选一个填进表单：`;

    rows.forEach((n) => {
        const item = document.createElement('div');
        item.className = 'ap';

        const info = document.createElement('div');
        info.className = 'ap-info';

        const name = document.createElement('div');
        name.className = 'ap-name';
        name.textContent = n.ssid ? n.ssid : '（隐藏网络）';
        info.appendChild(name);

        const meta = document.createElement('div');
        meta.className = 'ap-meta';
        meta.textContent = `${n.bssid} · 信道 ${n.channel} ${bandOf(n.channel)} · ${n.rssi}dBm ${signalText(n.rssi)} · ${n.secure ? '加密' : '开放'}`;
        info.appendChild(meta);

        item.appendChild(info);

        const pick = document.createElement('button');
        pick.type = 'button';
        pick.className = 'ap-pick';
        pick.textContent = n.ssid ? '填入' : '只填MAC';
        pick.onclick = () => {
            if (n.ssid) ssidInput.value = n.ssid;
            macInput.value = n.bssid;
            toast(n.ssid ? `已填入：${n.ssid} / ${n.bssid}` : `已填入 MAC：${n.bssid}（隐藏网络，SSID 请手动填）`);
            window.scrollTo({ top: 0, behavior: 'smooth' });
        };
        item.appendChild(pick);

        scanResult.appendChild(item);
    });
}

scanBtn.onclick = doScan;
