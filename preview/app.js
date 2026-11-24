const pageNames = ['01/阁楼时钟', '02/能量额度', '03/系统状态', '04/股票行情'];
const pageKeys = ['loft', 'quota', 'status', 'stock'];

let activePage = -1; // 默认不硬编码，随实机当前轮播页动态决定
let controlMode = 'mirror'; // 'mirror': 跟随实机轮播镜像, 'edit': 手动调试锁定
let carouselActive = true;
let isPollingStatus = false;
let pageCommandPending = null, pageCommandSending = false, pageCommandTimer = null;
let theme = {};
const loftSceneNames = ['阁楼', '近地空间站', '咖啡角', '月面基地', '书房', '星环火箭', '和室', '太空舱'];
let lastSceneSelection = null;
// 亮度滑块被手指按住的截止时刻：这期间状态轮询不许把滑块拽回去。
let brightDragUntil = 0;

const canvas = document.getElementById('screenCanvas');
const ctx = canvas.getContext('2d');
const imgData = ctx.createImageData(64, 64);
// Paint through a 64x64 backing canvas with putImageData (synchronous).
// createImageBitmap() used to queue one promise per frame against a shared
// imgData buffer: when frames arrived faster than the compositor painted,
// callbacks piled up and drew stale pixels, which made rapid switching feel
// frozen. One in-flight frame is all the display can ever show.
const frameCanvas = document.createElement('canvas');
frameCanvas.width = frameCanvas.height = 64;
const frameCtx = frameCanvas.getContext('2d');

// Bound every device-bound fetch. A stalled ESP32 used to hold a browser
// connection slot (browsers allow ~6 per host); enough stalled clicks and even
// the local /api/frame stream had nowhere to go.
function fetchTimeout(url, ms, options) {
  const ctrl = typeof AbortController !== 'undefined' ? new AbortController() : null;
  const timer = ctrl ? setTimeout(() => ctrl.abort(), ms) : null;
  const opts = ctrl ? {...(options || {}), signal: ctrl.signal} : options;
  return fetch(url, opts).finally(() => { if (timer) clearTimeout(timer); });
}

// 1. 本地像素流极速无缝渲染 (30~60 FPS 丝滑循环)
let frameInFlight = false;
async function renderLoop() {
  if (frameInFlight || document.hidden) { requestAnimationFrame(renderLoop); return; }
  frameInFlight = true;
  try {
    const res = await fetch('/api/frame');
    if (res.ok) {
      const data = await res.json();
      const pinnedScene = data.state?.loftSceneSelected ?? -1;
      updateSceneSelection(pinnedScene);
      const selected = pageKeys[activePage];
      const locked = liveDataEnabled && (liveMacLocked || hardwareMacLocked);
      const followsLockedHardware = hardwareMacLocked && failCount === 0
        && (selected === 'loft' || selected === 'stock');
      const lockedPage = followsLockedHardware ? selected : data.active;
      const curKey = pinnedScene >= 0 ? 'loft'
        : locked ? (lockedPage === 'stock' ? 'stock' : 'loft') : selected || data.active || 'quota';
      if (pinnedScene >= 0) document.getElementById('screenPageBadge').textContent = `${loftSceneNames[pinnedScene]} · 本地预览`;
      // 姿态动画（摇晃/倒转）占满整屏，优先于任何页面
      const hexStr = (data.state && data.state.motion && data.frames && data.frames.motion)
        || (data.frames && data.frames[curKey]) || (data.frames && data.frames[data.active]);
      if (hexStr) {
        drawHexFrame(hexStr);
      }
    }
  } catch (e) {
    // ignore temporary network gap
  } finally {
    frameInFlight = false;
  }
  requestAnimationFrame(renderLoop);
}

function drawHexFrame(hex) {
  const d = imgData.data;
  for (let i = 0; i < 4096; ++i) {
    const rgb565 = parseInt(hex.substr(i * 4, 4), 16);
    const r = ((rgb565 >> 11) & 0x1f) * 255 / 31;
    const g = ((rgb565 >> 5) & 0x3f) * 255 / 63;
    const b = (rgb565 & 0x1f) * 255 / 31;
    const idx = i * 4;
    d[idx] = r;
    d[idx + 1] = g;
    d[idx + 2] = b;
    d[idx + 3] = 255;
  }
  frameCtx.putImageData(imgData, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(frameCanvas, 0, 0, 320, 320);
}

let failCount = 0;
let liveDataEnabled = true;
let liveMacLocked = null;
let hardwareMacLocked = false;
let hardwarePollTimer = null;
let deviceSyncTimer = null;
let syncingDevice = false;
let lastLinkSig = '';   // dedupe the link snapshot pushed into the local engine

function scheduleHardwarePoll(delay = 1500) {
  clearTimeout(hardwarePollTimer);
  hardwarePollTimer = setTimeout(pollHardware, delay);
}

function scheduleDeviceSync(delay = 3000) {
  clearTimeout(deviceSyncTimer);
  deviceSyncTimer = setTimeout(syncDeviceData, delay);
}

// 2. 实时硬件状态轻量轮询 (每 2s 一次，仅传输 80 字节 JSON，无延迟无负荷)
async function pollHardware() {
  if (isPollingStatus || document.hidden) return;
  if (pageCommandSending || pageCommandPending) { scheduleHardwarePoll(1000); return; }
  isPollingStatus = true;
  try {
    const res = await fetchTimeout('/api/esp32/api/status', 4000);
    if (res.status === 503) return; // proxy is busy; this is not a Wi-Fi failure
    if (!res.ok) throw new Error('ESP32 离线');
    const d = await res.json();
    failCount = 0;
    hardwareMacLocked = d.mac_locked === true;
    if (typeof d.night === 'number') {
      if (d.night !== nightMode) updateNightUI(d.night);
      // Mirror the panel's override into the local engine once per load:
      // otherwise a refresh shows full colour while the device sits in forced
      // red, and the two consoles look like they disagree.
      if (!nightSynced && liveDataEnabled) {
        nightSynced = true;
        fetch('/api/action', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({op: 'night', value: d.night})
        }).catch(() => null);
      }
    }

    // 时钟墨色镜像：实机是什么色，预览就画什么色（自动轮换时也跟着走）
    if (liveDataEnabled && typeof d.ink === 'string') {
      const devInk = /^#[0-9a-f]{6}$/i.test(d.ink) && d.ink !== '#000000' ? d.ink.toLowerCase() : null;
      if (devInk && devInk !== lastDeviceInk) {
        lastDeviceInk = devInk;
        fetch('/api/action', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({op: 'ink', value: devInk})
        }).catch(() => null);
      }
      updateInkUI(d.ink_pin === true, devInk || '');
    }

    // 更新在线状态指示灯（三态：全通 / 有网无数据 / 无网）
    document.getElementById('espLed').className = 'indicator live';
    document.getElementById('espText').textContent = hardwareMacLocked ? '实机可达 · Mac 锁屏'
      : d.wifi === false ? '实机可达 · WiFi 未连'
      : d.mac_online === true ? '实机可达 · Mac 数据已连'
      : d.mac_online === false ? '实机可达 · Mac 数据未连' : '实机可达';
    document.getElementById('espHeap').textContent = Number.isFinite(d.rssi) ? `${d.rssi} dBm` : `${d.heap} KB 内存`;
    if (d.ip) document.getElementById('mIp').textContent = d.ip;

    // 链路镜像：把实机的 WiFi / 信号 / IP / 时钟来源 / 堆内存 / 姿态灌进本地引擎。
    // 否则网页版系统状态页永远假装"WiFi 在线、NTP 正常"，和真机对不上。
    if (liveDataEnabled) {
      const sig = `${d.wifi}|${d.rssi}|${d.ip}|${d.clock_src}|${d.heap}|${d.mpu}|${d.mpu_pose}|${d.tilt}`;
      if (sig !== lastLinkSig) {
        lastLinkSig = sig;
        fetch('/api/action', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            op: 'values',
            wifi: d.wifi === true,
            rssi: Number.isFinite(d.rssi) ? d.rssi : 0,
            ip: d.wifi === true && typeof d.ip === 'string' ? d.ip : '',
            clockSrc: d.clock_src === 'ntp' ? 1 : (d.clock_src === 'mac' ? 2 : 0),
            heapKb: Number.isFinite(d.heap) ? d.heap : 0,
            // 实机没装 MPU6050 时也把 false 传下去，预览才不会自作多情地画 POSE
            mpu: d.mpu === true,
            mpuPose: Number.isFinite(d.mpu_pose) ? d.mpu_pose : 0,
            tiltDeg: Number.isFinite(d.tilt) ? d.tilt : 0
          })
        }).catch(() => null);
      }
    }
    // 滑块显示实机自己的日间基准（bright_base），不是这个标签页上次拖到的值：
    // 设备断电重启后亮度回到编译默认 60，控制台必须跟着回去，否则两边各说各话。
    // 拖动中（含松手后 1.5s）不覆盖，避免和手指抢滑块。
    const base = Number.isFinite(d.bright_base) ? d.bright_base
      : (Number.isFinite(d.brightness) ? d.brightness : null);
    if (base !== null && performance.now() > brightDragUntil && Number(brightRange.value) !== base) {
      brightRange.value = base;
      theme.brightness = base;
    }
    // The device dims to 20 inside the sleep window; show that instead of
    // leaving the slider value looking like a lie.
    if (typeof d.brightness === 'number') {
      const target = Number(brightRange.value);
      brightNum.textContent = d.brightness === target
        ? `${target} / ${brightRange.max}`
        : `${target} / ${brightRange.max} · 实机 ${d.brightness}`;
    }

    // 锁定/跟随状态同步
    carouselActive = d.carousel;
    if (hardwareMacLocked || d.carousel) {
      controlMode = 'mirror';
    }

    if (controlMode === 'mirror') {
      if (d.page !== activePage) {
        activePage = d.page;
        updatePageButtons(d.page);
      }
      updateModeUI('mirror', d.page);
    } else {
      updateModeUI('edit', activePage);
    }
    if (hardwareMacLocked) {
      const status = document.getElementById('segStatus');
      if (status) status.textContent = '🔒 Mac 已锁屏 · 阁楼和股票轮播中';
      document.getElementById('segFollow').className = 'seg-btn disabled';
      document.getElementById('segLock').className = 'seg-btn disabled';
    }

    // 更新看板数值
    document.getElementById('mHeap').textContent = `${d.heap} KB`;
    document.getElementById('mUptime').textContent = formatUptime(d.uptime);
    // Protocol keeps "fun" (the Mac service speaks it), the UI says HOME.
    const modeText = d.mode === 'WORK' ? 'WORK' : 'HOME';
    document.getElementById('mMode').textContent = hardwareMacLocked ? '锁屏模式' : `${modeText} 模式`;

    document.getElementById('screenPageBadge').textContent = pageNames[d.page] || '02/能量额度';
    const mb = document.getElementById('screenModeBadge');
    mb.textContent = hardwareMacLocked ? 'LOCK' : modeText;
    mb.className = d.mode === 'WORK' ? 'mode-badge' : 'mode-badge fun';

  } catch (err) {
    failCount++;
    if (failCount < 3) {
      document.getElementById('espText').textContent = '实机响应慢，正在重试';
      return;
    }
    document.getElementById('espLed').className = 'indicator offline';
    document.getElementById('espText').textContent = '实机状态暂不可达';
    document.getElementById('espHeap').textContent = '等待状态响应';
    const status = document.getElementById('segStatus');
    if (status && controlMode === 'mirror') {
      status.textContent = '⚠️ 实机状态请求超时（将继续重试）';
    }
  } finally {
    isPollingStatus = false;
    scheduleHardwarePoll(Math.min(10000, 1500 * (1 + failCount)));
  }
}

// 2.1 权威额度数据同步：从 Mac 服务同步真实额度并注入本地渲染引擎
async function syncDeviceData() {
  if (!liveDataEnabled || document.hidden || syncingDevice) return;
  syncingDevice = true;
  try {
    const res = await fetch('/api/device');
    if (!res.ok) return;
    const dev = await res.json();
    if (typeof dev.mac_locked === 'boolean' && dev.mac_locked !== liveMacLocked) {
      const lockResult = await fetch('/api/action', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({op: 'mac', value: dev.mac_locked ? 'lock' : 'wake'})
      });
      if (lockResult.ok) liveMacLocked = dev.mac_locked;
    }
    if (dev.mac_locked) {
      for (const id of ['qVal', 'weekVal', 'refillTime']) document.getElementById(id).textContent = '--';
      document.getElementById('ageBadge').textContent = '锁屏 · 额度同步暂停';
    }
    const q = dev.data && dev.data.quota;
    if (q && q.available) {
      const q5h = q.five_hour ? Math.round(q.five_hour.remaining_percent) : -1;
      const qW = q.weekly ? Math.round(q.weekly.remaining_percent) : -1;
      const resetsAtStr = (q.five_hour && q.five_hour.resets_at) ? q.five_hour.resets_at.split(' ')[1] : '--:--';
      const resetEpoch = q.five_hour ? q.five_hour.resets_at_epoch : 0;
      const nowEpoch = dev.epoch || Math.floor(Date.now() / 1000);
      const ageSec = q.data_age_sec || 0;

      // 更新右侧看板
      if (q5h >= 0) document.getElementById('qVal').textContent = q5h;
      if (qW >= 0) document.getElementById('weekVal').textContent = `${qW}%`;
      document.getElementById('refillTime').textContent = resetsAtStr;
      document.getElementById('ageBadge').textContent = ageSec < 60 ? `${ageSec} 秒前更新` : `${Math.floor(ageSec / 60)} 分钟前更新`;

      // 关键！将真实的 5H 额度、周额度、回满时间同步给本地渲染引擎，确保虚拟屏幕与实机完全一致
      const actionBody = {
        op: 'values',
        quota5h: q5h,
        quotaWeek: qW,
        reset5hAt: Number.isSafeInteger(resetEpoch) ? resetEpoch : 0,
        ageMinutes: Math.floor(ageSec / 60),
        epoch: nowEpoch
      };
      await fetch('/api/action', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(actionBody)
      });
    }

    const stock = dev.data && dev.data.stock;
    const gold = stock?.gold?.price;
    const goldShut = !!stock?.gold?.closed;
    document.getElementById('goldPrice').textContent = Number.isFinite(gold)
      ? gold.toFixed(2) + (goldShut ? ' (收盘)' : '') : '--';
    const goldUsd = stock?.gold_usd?.price;
    document.getElementById('goldUsdPrice').textContent = Number.isFinite(goldUsd) ? goldUsd.toFixed(2) : '--';
    await fetch('/api/action', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'values', stock: stock || {available: false}})
    });
    if (stock && stock.available) {
      const sh = stock.sh, ix = stock.nasdaq;
      if (sh && document.getElementById('shPrice')) {
        document.getElementById('shPrice').textContent = (typeof sh.price === 'number') ? sh.price.toFixed(2) : '--';
        const pEl = document.getElementById('shPct');
        const sign = sh.pct >= 0 ? '+' : '';
        pEl.textContent = sign + sh.pct.toFixed(2) + '%';
        pEl.className = 's-pct ' + (sh.pct >= 0 ? 'up' : 'down');
      }
      if (ix && document.getElementById('ixicPrice')) {
        document.getElementById('ixicPrice').textContent = (typeof ix.price === 'number') ? ix.price.toFixed(1) : '--';
        const pEl = document.getElementById('ixicPct');
        const sign = ix.pct >= 0 ? '+' : '';
        pEl.textContent = sign + ix.pct.toFixed(2) + '%';
        pEl.className = 's-pct ' + (ix.pct >= 0 ? 'up' : 'down');
      }
    }
  } catch (e) {
    // ignore
  } finally {
    syncingDevice = false;
    if (liveDataEnabled) scheduleDeviceSync();
  }
}

function updatePageButtons(pageIdx) {
  for (let i = 0; i < 4; i++) {
    const btn = document.getElementById('rBtn' + i);
    if (btn) btn.className = (i === pageIdx) ? 'remote-btn active' : 'remote-btn';
  }
  document.getElementById('screenPageBadge').textContent = pageNames[pageIdx] || '';
}

function updateModeUI(mode, curPage) {
  const follow = document.getElementById('segFollow');
  const lock = document.getElementById('segLock');
  const status = document.getElementById('segStatus');
  if (!follow || !lock) return;

  if (mode === 'mirror') {
    follow.className = 'seg-btn active';
    lock.className = 'seg-btn';
    if (status) status.textContent = `🔄 跟随实机轮播中 · 实机展示: ${pageNames[curPage] || '未连接'}`;
  } else {
    follow.className = 'seg-btn';
    lock.className = 'seg-btn active';
    if (status) status.textContent = `🔒 手动锁定 · 停留在: ${pageNames[curPage] || '当前页'}`;
  }
}

// 跟随实机：恢复设备自动轮播，网页镜像跟随
window.segFollow = async function() {
  if (hardwareMacLocked || (liveDataEnabled && liveMacLocked)) {
    toast('Mac 锁屏期间轮播阁楼和股票');
    return;
  }
  if (controlMode === 'mirror' && carouselActive) return;
  try {
    await setCarousel(true);
  } catch (err) {
    toast(`恢复轮播失败: ${err.message}`);
  }
};

// 手动锁定：停住实机在指定页，网页同页调试
window.segLock = async function() {
  if (hardwareMacLocked || (liveDataEnabled && liveMacLocked)) {
    toast('Mac 锁屏期间轮播阁楼和股票');
    return;
  }
  if (controlMode === 'edit') return;
  const page = activePage < 0 ? 1 : activePage;
  await window.sendPage(page, pageKeys[page]);
};

// 3. 一键无线切换真机页面 (点击即进入手动锁定：停轮播 + 指定页)
// UI is optimistic: the click paints the new selection immediately and only
// then syncs the device in the background. The old flow awaited a slow device
// (RTT spikes to seconds) before touching the buttons, so fast clicks looked
// dead and users kept tapping, stacking requests until the page froze.
window.sendPage = function(pageIndex, key) {
  if (hardwareMacLocked || (liveDataEnabled && liveMacLocked)) {
    toast('Mac 锁屏期间轮播阁楼和股票');
    return;
  }
  // Optimistic UI — instant feedback, no network involved.
  controlMode = 'edit';
  carouselActive = false;
  activePage = pageIndex;
  updatePageButtons(pageIndex);
  updateModeUI('edit', pageIndex);
  toast(`已锁定：${pageNames[pageIndex]}`);
  // Coalesce rapid taps to the newest selection; never stack device requests.
  pageCommandPending = {pageIndex, key};
  clearTimeout(pageCommandTimer);
  pageCommandTimer = setTimeout(drainPageCommands, 120);
};
async function drainPageCommands() {
  if (pageCommandSending || !pageCommandPending) return;
  pageCommandSending = true;
  try {
    while (pageCommandPending) {
      const command = pageCommandPending; pageCommandPending = null;
      await sendPageOnce(command.pageIndex, command.key);
    }
  } finally { pageCommandSending = false; }
}
async function sendPageOnce(pageIndex, key) {
  // Local renderer first: it lives on localhost and is what the user sees.
  // Fire-and-forget so a busy engine never delays the hardware sync either.
  fetch('/api/action', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({op: 'page', value: key})
  }).catch(() => null);

  // Device sync is bounded: a stalled radio can hold at most one slot for
  // 3s, then we fall back to the Mac relay instead of waiting forever.
  try {
    const response = await fetchTimeout(`/api/esp32/page?set=${pageIndex}&hold=1`, 3000).catch(() => null);
    if (response?.status === 409) {
      controlMode = 'mirror';
      scheduleHardwarePoll(0);
      toast('Mac 锁屏期间轮播阁楼和股票');
      return;
    }
    if (!response?.ok) {
      const queued = await fetchTimeout('/api/device', 3000, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({op: 'page', value: key})
      }).catch(() => null);
      if (!queued?.ok) toast('实机暂不可达，已在本机预览切换');
    }
    scheduleHardwarePoll(450);
  } catch (err) {
    toast(`切页通信异常: ${err.message}`);
  }
}

// 4. 轮播开关切换
async function setCarousel(enabled) {
  const [direct, viaMac] = await Promise.all([
    fetchTimeout(`/api/esp32/carousel?set=${enabled ? 1 : 0}`, 3000).catch(() => null),
    enabled ? fetchTimeout('/api/device', 3000, {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'resume', value: true})
    }).catch(() => null) : Promise.resolve(null)
  ]);
  if (direct?.status === 409) throw new Error('Mac 已锁屏');
  if (!direct?.ok && !viaMac?.ok) throw new Error('实机未接收指令');
  if (direct?.ok) carouselActive = enabled;
  controlMode = enabled ? 'mirror' : 'edit';
  updateModeUI(controlMode, activePage);
  toast(direct?.ok ? '已恢复实机智能轮播' : '已交给 Mac，等待实机恢复轮播');
  scheduleHardwarePoll(200);
}

// 5. 屏幕亮度联动调节
const brightRange = document.getElementById('brightRange');
const brightNum = document.getElementById('brightNum');
let brightDebounce = null;
let brightnessSending = false;
let pendingBrightness = null;
let brightnessTimer = null;
let nextBrightnessAt = 0;
function schedulePanelBrightness() {
  if (brightnessSending || brightnessTimer !== null || pendingBrightness === null) return;
  brightnessTimer = setTimeout(() => {
    brightnessTimer = null;
    syncPanelBrightness();
  }, Math.max(0, nextBrightnessAt - performance.now()));
}
async function syncPanelBrightness() {
  const {value, final} = pendingBrightness;
  pendingBrightness = null;
  brightnessSending = true;
  nextBrightnessAt = performance.now() + 80;
  try {
    const direct = await fetch(`/api/esp32/brightness?val=${value}`).catch(() => null);
    if (final && !direct?.ok && pendingBrightness === null) {
      const queued = await fetch('/api/device', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({op: 'brightness', value})
      }).catch(() => null);
      if (!pendingBrightness) toast(queued?.ok
        ? '直连超时，已交给 Mac 等待实机同步'
        : '实屏未确认亮度，本地预览已更新');
    }
  } finally {
    brightnessSending = false;
    schedulePanelBrightness();
  }
}
brightRange.addEventListener('input', (e) => {
  const val = Number(e.target.value);
  brightDragUntil = performance.now() + 1500;
  brightNum.textContent = `${val} / ${brightRange.max}`;
  theme.brightness = val;
  pendingBrightness = {value: val, final: false};
  schedulePanelBrightness();
  clearTimeout(brightDebounce);
  brightDebounce = setTimeout(() => {
    fetch('/api/theme', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(theme)
    }).catch(() => {});
  }, 150);
});
brightRange.addEventListener('change', () => {
  brightDragUntil = performance.now() + 1500;
  pendingBrightness = {value: Number(brightRange.value), final: true};
  schedulePanelBrightness();
  // 松手只下发、不落盘：亮度是"这一刻想多亮"，不是外观设置。以前这里会 /api/save，
  // 顺手把 brightness 写进 display_theme.h，于是"昨晚调到 20"变成了第二天的开机默认。
  clearTimeout(brightDebounce);
});

// 5.1 夜间红光模式三态（0 自动 / 1 强制红光 / 2 强制彩色）
const nightLabels = ['自动 · 22:30–06:00', '强制红光 · 任何时间', '强制彩色 · 夜间不转红'];
const nightToasts = ['夜间模式：自动 (22:30–06:00 转红光)', '夜间模式：强制红光', '夜间模式：强制彩色'];
let nightMode = 0;
let nightSynced = false;  // local engine adopted the panel's override once

function updateNightUI(n) {
  nightMode = (n | 0) % 3;
  for (let i = 0; i < 3; i++) {
    const b = document.getElementById('nightBtn' + i);
    if (b) b.className = i === nightMode ? 'seg-btn active' : 'seg-btn';
  }
  const st = document.getElementById('nightStatus');
  if (st) st.textContent = nightLabels[nightMode] || nightLabels[0];
}

window.setNight = async function(n) {
  try {
    await fetch('/api/action', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'night', value: n})
    });
    updateNightUI(n);
    toast(nightToasts[n] || '夜间模式已切换');
    fetch(`/api/esp32/night?set=${n}`).catch(() => null); // 同步真机，失败无妨
  } catch (err) {
    toast(`夜间切换异常: ${err.message}`);
  }
};

// 5.2 阁楼时钟墨色：预览引擎与实机同时换（点圆点＝固定，自动＝继续轮换）
let inkPinned = false;
let lastDeviceInk = null;

function updateInkUI(pinned, value) {
  inkPinned = pinned;
  const auto = document.getElementById('inkAuto');
  if (auto) auto.className = pinned ? 'ink-btn' : 'ink-btn active';
  const hex = pinned && typeof value === 'string' ? value.toLowerCase() : '';
  document.querySelectorAll('.ink-dot').forEach(dot => {
    const match = (dot.getAttribute('onclick') || '').match(/#[0-9a-f]{6}/i);
    dot.classList.toggle('active', !!hex && !!match && match[0].toLowerCase() === hex);
  });
  const st = document.getElementById('inkStatus');
  if (st) st.textContent = hex ? `固定 ${hex}` : '自动 · 回来就换色';
}

window.setInk = async function(value) {
  const pinned = value !== 'auto' && value !== 'next';
  try {
    await fetch('/api/action', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'ink', value})
    });
  } catch (err) {
    toast(`换色失败: ${err.message}`);
    return;
  }
  updateInkUI(pinned, value);
  toast(pinned ? `时钟墨色固定为 ${value}` : value === 'next' ? '已换一种墨色' : '时钟墨色恢复自动轮换');
  // 同步真机；实机离线时只影响预览，不报错。设备侧收裸 6 位十六进制。
  const deviceValue = value.charAt(0) === '#' ? value.slice(1) : value;
  fetch(`/api/esp32/ink?set=${encodeURIComponent(deviceValue)}`).catch(() => null);
};

// 6. 预设场景体验与真实数据恢复
// 重力反应试看：MPU6050 还没到货，也能先在预览里看完五张脸。6 秒后自动结束。
const MOTION_UI = {
  1: {chip: 'chipLean',  hint: '演示：被拿起来往一边倾（LEAN）'},
  2: {chip: 'chipShake', hint: '演示：被摇晃（SHAKE）'},
  3: {chip: 'chipFlip',  hint: '演示：整屏倒转（FLIP）'},
  4: {chip: 'chipTap',   hint: '演示：被敲了一下（TAP）'},
  5: {chip: 'chipDizzy', hint: '演示：摇完还在缓（DIZZY）'},
  6: {chip: 'chipSettle', hint: '演示：扶正后松口气（SETTLE）'},
};
let motionTimer = null;
window.previewMotion = async function(kind) {
  const ui = MOTION_UI[kind];
  if (!ui) return;
  const btn = document.getElementById(ui.chip);
  document.querySelectorAll('.chip:not(.loft-scene-chip)').forEach(c => c.classList.remove('active'));
  if (btn) btn.classList.add('active');
  try {
    await fetch('/api/action', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'motion', value: kind})
    });
  } catch (err) {
    toast(`姿态动画播放失败: ${err.message}`);
    return;
  }
  toast(ui.hint);
  clearTimeout(motionTimer);
  motionTimer = setTimeout(async () => {
    try {
      await fetch('/api/action', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({op: 'motion', value: 0})
      });
    } catch (e) { /* 预览服务短暂离线时忽略 */ }
    if (btn) btn.classList.remove('active');
    if (liveDataEnabled) document.getElementById('chipLive').classList.add('active');
  }, 6000);
};

function updateSceneSelection(scene) {
  if (scene === lastSceneSelection) return;
  lastSceneSelection = scene;
  document.querySelectorAll('.loft-scene-chip').forEach(btn => {
    btn.classList.toggle('active', Number(btn.dataset.scene) === scene);
  });
  const hint = document.getElementById('sceneJumpHint');
  if (hint) hint.textContent = scene >= 0 ? `固定预览：${loftSceneNames[scene]} · 实机不变` : '仅切换本地预览';
  if (scene < 0) updatePageButtons(activePage);
}

window.selectLoftScene = async function(scene, quiet = false) {
  if (!Number.isInteger(scene) || scene < -1 || scene > 7) return;
  try {
    const res = await fetch('/api/action', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'scene', value: scene})
    });
    if (!res.ok) throw new Error((await res.json()).error || '切换失败');
    updateSceneSelection(scene);
    if (!quiet) toast(scene < 0 ? '预览已恢复跟随页面' : `正在预览：${loftSceneNames[scene]}，实机轮播不变`);
  } catch (err) { toast(`场景切换失败：${err.message}`); }
};

window.enableLiveData = async function() {
  await selectLoftScene(-1, true);
  document.querySelectorAll('.chip:not(.loft-scene-chip)').forEach(c => c.classList.remove('active'));
  document.getElementById('chipLive').classList.add('active');
  liveDataEnabled = true;
  liveMacLocked = null;
  scheduleDeviceSync(0);
  toast('已恢复实时数据同步');
};

window.setPreset = async function(name) {
  liveDataEnabled = false;
  updateNightUI(0); // 场景重置夜间为自动（与渲染引擎 preset 行为一致）
  document.querySelectorAll('.chip:not(.loft-scene-chip)').forEach(c => c.classList.remove('active'));
  event.target.classList.add('active');
  try {
    await fetch('/api/action', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({op: 'preset', value: name})
    });
    toast(`已加载情境场景：${name} (已暂停实时同步)`);
  } catch (err) {
    toast(`场景切换异常: ${err.message}`);
  }
};

function formatUptime(sec) {
  if (!sec && sec !== 0) return '--';
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  return `${m}分${s}秒`;
}

function toast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show';
  setTimeout(() => { t.className = 'toast'; }, 2200);
}

// 初始化加载配置
fetch('/api/config').then(r => r.json()).then(cfg => {
  theme = cfg.theme || {};
  // 滑块初值交给第一次 /api/status：以实机 bright_base 为准，而不是这里存的数字。
  brightNum.textContent = `${brightRange.value} / ${brightRange.max}`;
}).catch(() => {});

// 启动渲染与轮询
renderLoop();
scheduleHardwarePoll(0);
scheduleDeviceSync(0);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  scheduleHardwarePoll(0);
  scheduleDeviceSync(0);
});
