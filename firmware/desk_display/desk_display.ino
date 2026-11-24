/* ESP32 autonomous desk display: local clock, skyline and pixel loft.
 * Mac provides quota and expiring commands; no network call runs in loop(). */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <time.h>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <Wire.h>
#include <base64.h>
#include <atomic>
#include <cmath>
#include "display_controller.h"
#include "state_protocol.h"
#include "wifi_recovery.h"
#include "ota_buffer.h"
#include "display_renderer.h"
using desk::Snapshot;
#if __has_include("config.h")
#include "config.h"
#endif
#ifndef DESK_WIFI_SSID
#define DESK_WIFI_SSID ""
#endif
#ifndef DESK_WIFI_PASS
#define DESK_WIFI_PASS ""
#endif
#ifndef DESK_MAC_HOST
#define DESK_MAC_HOST ""
#endif
#ifndef DESK_API_TOKEN
#define DESK_API_TOKEN ""
#endif
#ifndef DESK_TIMEZONE_OFFSET
#define DESK_TIMEZONE_OFFSET (8 * 3600)
#endif
#ifndef DESK_WORK_START_MINUTE
#define DESK_WORK_START_MINUTE (9 * 60)
#endif
#ifndef DESK_WORK_END_MINUTE
#define DESK_WORK_END_MINUTE (18 * 60)
#endif
#ifndef DESK_WORK_DAYS
#define DESK_WORK_DAYS 0x3e
#endif
#ifndef DESK_BUTTON_ENABLED
#define DESK_BUTTON_ENABLED 0
#endif
#ifndef HEARTBEAT_LED_PIN
#define HEARTBEAT_LED_PIN 2
#endif

#define BTN_PIN 33

#ifndef MPU_SDA_PIN
#define MPU_SDA_PIN 5
#endif
#ifndef MPU_SCL_PIN
#define MPU_SCL_PIN 15
#endif

struct MpuMotion {
  bool present = false;
  uint8_t i2cAddr = 0x68;
  uint32_t lastSample = 0;
  uint8_t readErrors = 0;
  uint8_t flipSamples = 0;
  float tiltCos = 1.0f;
  bool calibrated = false;
  uint16_t calibCount = 0;
  float baseAx = 0, baseAy = 0, baseAz = 1.0f;
  float sumAx = 0, sumAy = 0, sumAz = 0;

  bool isDizzy = false;
  uint32_t lastFastMotion = 0;
  bool isUpsideDown = false;
  // Gravity resolved onto the mounted plane (-100..100). The reaction faces
  // lean and look the way the hand tips the panel.
  int8_t leanX = 0, leanY = 0;
  bool leanConsumed = false;
  bool leanArmed = true;
  uint8_t leanTaps = 0;
  uint32_t leanLastTipAt = 0;
  int8_t leanLastDir = 0;
  int8_t leanSign = 1;         // mirrored chip? /lean?set=-1 flips it live
  bool leanSwap = false;       // chip rotated 90°? /lean?swap=1 swaps the axes

  // Double tap gesture
  uint32_t lastTapAt = 0;
  uint32_t firstTapAt = 0;
  uint8_t tapCount = 0;
  bool tapArmed = true;

  // Pitch nudge gesture (向后倒一下翻页)
  bool pitchArmed = true;
  uint8_t pitchSamples = 0;

  // Held up in hands detection (拿起来检测)
  bool isHeldUp = false;
  uint8_t heldSamples = 0;
  uint8_t deskStationaryFrames = 0; // 桌面绝对死寂稳定帧数计数（滞环判断放下）
  uint8_t heldLevelFrames = 0;      // 回平保底计数：震动桌面上死寂判据可能永不成立

  // Boot-time static zero alignment
  uint16_t stillFrames = 0;

  // Light Nudge / Flick gesture
  int8_t nudgeDir = 0;
  uint32_t nudgeAt = 0;
  uint32_t lastPageTurnAt = 0;
};
MpuMotion mpu;
// Which reaction face is up right now, and since when. One owner, so the
// renderer never has to guess: FLIP wins over everything (the panel really is
// upside down), then the shake hangover, then a tap that had no page to
// switch, then LEAN — with a short exit delay so a knock does not flash a face.
struct GestureState { uint8_t mood = 0; uint32_t startedAt = 0; uint32_t tapAt = 0; uint32_t leanUntil = 0; uint32_t settleUntil = 0; };
GestureState gesture;
// Manual override for /mood?set=N: replays a face without the sensor, which is
// how the panel pixels get verified before the MPU6050 arrives (and after).
std::atomic<uint8_t> moodOverride{0};

MatrixPanel_I2S_DMA* display = nullptr;
desk::Controller controller;
desk::Button button;
QueueHandle_t snapshots = nullptr;
struct BrightnessCommand { uint8_t value; uint64_t issuedAt; };
QueueHandle_t brightnessCommands = nullptr;
// Brightness policy, three facts and nothing else:
//   1. Boot = the compiled level (DESK_THEME_BRIGHTNESS, 70). No flash write,
//      no NVS, no "restore last session" — plug in and it is 60 every time.
//   2. The 22:30–06:00 sleep window caps that level at NIGHT_BRIGHTNESS; it
//      never overwrites it, so colour returns to 60 at 06:00 by itself.
//   3. Only /brightness (console slider, curl, deskctl via the Mac snapshot)
//      moves the day level, and only in RAM. A drag is a temporary wish, not a
//      new default: that is what kept biting us as "开机亮度不是 60".
constexpr uint8_t NIGHT_BRIGHTNESS = 20;
std::atomic<uint8_t> panelBrightness{0};
std::atomic<bool> panelMacLocked{false};
std::atomic<bool> panelMacOnline{false};
std::atomic<int> panelMacHttp{0};
std::atomic<bool> resumeRequested{false};
std::atomic<uint32_t> lastWebControlAt{0};
std::atomic<uint32_t> visiblePageSince{0};
// WiFi self-heal telemetry: reconnect count and the last 802.11 disconnect
// reason, both exposed through /api/status so a flaky link is diagnosable
// without plugging in USB.
std::atomic<uint16_t> wifiReconnects{0};
std::atomic<bool> wifiEverHadIp{false};
std::atomic<bool> webResetRequested{false};
std::atomic<uint32_t> webLastBeat{0};
std::atomic<bool> otaBusy{false};
std::atomic<uint32_t> otaLastActivity{0};
extern size_t otaTotal;
extern size_t otaAccepted;
std::atomic<uint8_t> wifiLastReason{0};
// What the radio actually runs, not what we asked for: 0 = WIFI_PS_NONE,
// 1 = MIN_MODEM, 2 = MAX_MODEM, 255 = never read. Modem sleep is the single
// biggest HTTP latency knob on this board, so it belongs in /api/status.
std::atomic<uint8_t> wifiPsMode{255};
// How many times the link dropped since boot, and why the chip last restarted.
// Together they answer "WiFi 总是断": a rising count with reason 200 is beacon
// loss (the power-save symptom), 2/15/204 point at the AP or the passphrase,
// and reset 4 (brownout) means the panel is dragging the 5 V rail down.
std::atomic<uint16_t> wifiDropCount{0};
// The panel keeps its own wall clock: SNTP runs on the ESP32's radio, so the
// loft clock and the 22:30 sleep window keep working with the Mac powered off.
// The Mac epoch is a second opinion only. This counter proves a real SNTP
// answer arrived (the SDK latches sync status back to RESET after each one).
std::atomic<uint32_t> sntpSyncCount{0};

void onSntpSync(struct timeval*) {
  sntpSyncCount.store(sntpSyncCount.load() + 1);
}

// China-friendly NTP pool first: pool.ntp.org is frequently unreachable from
// home broadband here, and a silent SNTP failure used to leave the clock blank.
void startSntp() {
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTime(0, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
}

// 0 = no clock, 1 = own SNTP, 2 = Mac snapshot only (NTP blocked).
uint8_t clockSourceNow() {
  if (sntpSyncCount.load()) return 1;
  return controller.hasClock ? 2 : 0;
}
std::atomic<uint32_t> wifiLastDropAt{0};  // uptime seconds of the last deauth

// Fixed-size mailbox: net task owns HTTP/JSON; loop owns every UI variable.
// No shared Strings, no mutex held while drawing, no network button queue.
Snapshot data;
uint64_t messageUntil = 0, messageStarted = 0;
int64_t quotaAt = 0;
int quota5h = -1, quotaWeek = -1;
bool quotaStale = true;
bool dirty = true;
uint32_t lastDraw = 0;

desk::Theme theme;
desk::Page activeDisplayPage(const desk::ViewState& view);
// USB diagnostics report device state, never Wi-Fi passwords or API tokens.
void reportState() {
  desk::ViewState view;
  view.quota5h = quota5h;
  const char* pages[] = {"loft", "quota", "status", "stock"};
  const char* sources[] = {"auto", "mac", "manual"};
  Serial.printf("STATE mode=%s page=%s source=%s wifi=%s ip=%s service=%s http=%d "
                "clock=%lld quota5h=%d quotaWeek=%d stale=%d brightness=%u message=%d "
                "manual_ms=%u heap=%u dma_max=%u\n",
                controller.mode == desk::WORK ? "work" : "fun",
                pages[activeDisplayPage(view)], sources[controller.source],
                data.wifi ? "connected" : "disconnected", data.ip,
                controller.serviceOnline ? "online" : "offline", data.httpCode,
                (long long)controller.epoch(), quota5h, quotaWeek, quotaStale,
                unsigned(theme.brightness), int(controller.uptime() < messageUntil && data.message[0]),
                controller.manualRemainingMs(), ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

struct CarouselState {
  volatile bool enabled = true;
  volatile int manualPage = -1;  // -1 auto/carousel, 0..3 page lock (Page enum)
  volatile uint32_t manualUntil = 0; // millis timestamp when manual lock expires
  volatile uint32_t lastSwitch = 0;
  // Slot values are Page enum values directly: 0=LOFT, 1=QUOTA, 2=STATUS, 3=STOCK.
  volatile uint8_t currentSlot = 0;
  volatile int lastQuota = -1;
  volatile uint32_t lastQuotaChange = 0;
};
CarouselState carousel;

desk::Page activeDisplayPage(const desk::ViewState& view) {
  if (panelMacLocked.load()) {
    return carousel.currentSlot == desk::STOCK_PAGE ? desk::STOCK_PAGE : desk::LOFT_PAGE;
  }
  if (carousel.manualPage >= 0) {
    return (desk::Page)(carousel.manualPage % 4);
  }
  if (controller.source == desk::MANUAL && controller.manualRemainingMs() > 0) {
    return desk::currentPage(controller, view);
  }
  if (!wifiEverHadIp.load()) return desk::STATUS_PAGE;
  if (controller.mode == desk::FUN && !controller.serviceOnline) {
    return desk::LOFT_PAGE;
  }
  return (desk::Page)(carousel.currentSlot % 4);
}

bool probeMpuAt(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0x75); // WHO_AM_I
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom(int(addr), 1, true) == 1) {
    const int chipId = Wire.read();
    const bool isMpu6500 = chipId == 0x70;
    if ((chipId & 0x7e) != 0x68 && !isMpu6500) return false;
    Wire.beginTransmission(addr);
    Wire.write(0x6B); // PWR_MGMT_1
    Wire.write(0x00); // wake up, internal 8 MHz oscillator (clears SLEEP)
    if (Wire.endTransmission() != 0) return false;
    // CONFIG: DLPF 44 Hz. The reset default is DLPF off (260 Hz), which passes
    // enough hand tremor through that the "is it lying still?" test kept
    // failing and a tap could fire twice.
    Wire.beginTransmission(addr); Wire.write(0x1A); Wire.write(0x03);
    if (Wire.endTransmission() != 0) return false;
    if (isMpu6500) {
      // The 6500 has a separate accelerometer filter (41 Hz at setting 3).
      Wire.beginTransmission(addr); Wire.write(0x1D); Wire.write(0x03);
      if (Wire.endTransmission() != 0) return false;
    }
    // GYRO_CONFIG: FS_SEL=1 → ±500 °/s, 65.5 LSB/(°/s). The reset default is
    // ±250 °/s, where a reading of 250 is the ceiling — and the SHAKE trigger
    // below asks for 280, so with the default range the shake animation could
    // NEVER fire. This is the one line that makes it possible.
    Wire.beginTransmission(addr); Wire.write(0x1B); Wire.write(0x08);
    if (Wire.endTransmission() != 0) return false;
    Serial.printf("[MPU] WHO_AM_I=0x%02X initialized at address 0x%02X\n", unsigned(chipId), addr);
    mpu.i2cAddr = addr;
    return true;
  }
  return false;
}

bool probeMpu() {
  if (probeMpuAt(0x68)) return true;
  if (probeMpuAt(0x69)) return true;
  return false;
}

void initMpu() {
  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN, 400000);
  Wire.setTimeOut(25);
  mpu.present = probeMpu();
  Serial.println(mpu.present ? "[MPU6050] Connected" : "[MPU6050] Absent or init failed");
}

// One place decides whether a local input may touch the carousel, so the tap
// and the tilt can never disagree about the rules.
bool canTurnPage() {
  return true;
}

void turnPage(int8_t direction, uint32_t now) {
  if (now - mpu.lastPageTurnAt < 350) return; // 350ms debounce
  mpu.lastPageTurnAt = now;

  desk::ViewState view; view.quota5h = quota5h;
  desk::Page cur = activeDisplayPage(view);
  desk::Page next = direction > 0
      ? desk::nextCarouselPage(cur, controller.mode)
      : desk::previousCarouselPage(cur, controller.mode);

  carousel.manualPage = next;
  carousel.manualUntil = now + 45000; // 45秒手动驻留，抵抗 Mac snapshot 覆盖
  carousel.currentSlot = next;
  controller.page = next;
  visiblePageSince.store(now);
  dirty = true;
  Serial.printf("[GESTURE] Turn page: %d -> %d (dir=%d)\n", int(cur), int(next), int(direction));
}

void updateMpu(uint32_t now) {
  if (!mpu.present) {
    // Plug-and-play: keep probing so a hot-plugged MPU6050 enables tap-to-switch
    // without a reboot; a sensor that dropped out comes back the same way.
    static uint32_t lastProbe = 0;
    if (now - lastProbe >= 5000) {
      lastProbe = now;
      if (probeMpu()) {
        mpu.present = true;
        dirty = true;
        Serial.println("[MPU6050] Sensor appeared; tap-to-switch enabled");
      }
    }
    return;
  }
  if (mpu.isDizzy && now - mpu.lastFastMotion >= 1800) {
    mpu.isDizzy = false;
    dirty = true;
  }
  if (now - mpu.lastSample < 20) return;
  mpu.lastSample = now;

  Wire.beginTransmission(mpu.i2cAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(int(mpu.i2cAddr), 14, true) != 14) {
    if (++mpu.readErrors >= 5) {
      // AD0 pin might have floated or switched between GND (0x68) and 3V3 (0x69)
      const uint8_t altAddr = (mpu.i2cAddr == 0x68) ? 0x69 : 0x68;
      if (probeMpuAt(altAddr)) {
        mpu.readErrors = 0;
        return;
      }
    }
    if (mpu.readErrors >= 10) {
      mpu = MpuMotion{};
      dirty = true;
      Serial.println("[MPU6050] Lost sensor; restored normal display.");
    }
    return;
  }
  mpu.readErrors = 0;
  auto axis = []() -> int16_t {
    const int high = Wire.read();
    return int16_t((high << 8) | Wire.read());
  };
  const float ax = axis() / 16384.0f, ay = axis() / 16384.0f, az = axis() / 16384.0f;  // ±2 g
  axis(); // temperature
  const float gx = axis() / 65.5f, gy = axis() / 65.5f, gz = axis() / 65.5f;           // ±500 °/s
  const float omega = sqrtf(gx*gx + gy*gy + gz*gz);
  const float mag = sqrtf(ax*ax + ay*ay + az*az);

  if (!mpu.calibrated) {
    if (omega > 25 || mag < 0.75f || mag > 1.25f) {
      mpu.calibCount = 0;
      mpu.sumAx = mpu.sumAy = mpu.sumAz = 0;
      return;
    }
    mpu.sumAx += ax;
    mpu.sumAy += ay;
    mpu.sumAz += az;
    if (++mpu.calibCount >= 30) {
      const float norm = sqrtf(mpu.sumAx*mpu.sumAx + mpu.sumAy*mpu.sumAy + mpu.sumAz*mpu.sumAz);
      if (norm > 20.0f) {
        mpu.baseAx = mpu.sumAx / norm;
        mpu.baseAy = mpu.sumAy / norm;
        mpu.baseAz = mpu.sumAz / norm;
        mpu.calibrated = true;
        Serial.printf("[MPU6050] Level calibrated: [%.2f, %.2f, %.2f]\n", mpu.baseAx, mpu.baseAy, mpu.baseAz);
      } else {
        mpu.calibCount = 0;
        mpu.sumAx = mpu.sumAy = mpu.sumAz = 0;
      }
    }
    return;
  }

  if (omega > 280) {
    mpu.lastFastMotion = now;
    if (!mpu.isDizzy) { mpu.isDizzy = true; dirty = true; }
  }

  // ---- 1. Touch / Tap Detection (瞬间响应'别碰我'，过滤轻微平移) ----
  const float deltaMag = fabsf(mag - 1.0f);
  const bool isFastRotation = omega >= 45;

  // 平稳静止时重新武装
  if (deltaMag < 0.10f && omega < 20) {
    mpu.tapArmed = true;
  }

  // 严格互斥：一旦侧向倾斜看'要滑了'或处于倒立/拿起状态，锁死触碰，绝不触发'别碰我'
  if (std::abs(int(mpu.leanX)) >= 16 || mpu.isUpsideDown || mpu.isHeldUp) {
    mpu.tapArmed = false;
  }

  // 触碰判定：纯机械冲击加速度 >= 0.14g（敏感度微调优化），排除旋转动作（omega < 45），排除侧倾/倒立/拿起
  const bool touched = (deltaMag >= 0.14f) && !isFastRotation && (std::abs(int(mpu.leanX)) < 12) && !mpu.isUpsideDown && !mpu.isHeldUp;
  if (touched && mpu.tapArmed && !mpu.isDizzy) {
    mpu.tapArmed = false;
    if (now - mpu.lastTapAt > 400) { // 400ms 机械防抖
      mpu.lastTapAt = now;
      gesture.tapAt = now;
      dirty = true;
    }
  }

  // Filter the normalized gravity angle, and only trust near-static samples.
  if (mag < 0.80f || mag > 1.20f || omega > 90) {
    mpu.flipSamples = 0;
    mpu.leanArmed = false;
    mpu.leanTaps = 0;
    return;
  }
  const float cosine = (ax*mpu.baseAx + ay*mpu.baseAy + az*mpu.baseAz) / mag;
  mpu.tiltCos += (cosine - mpu.tiltCos) * 0.3f;

  // 仅在开机前 25 秒内做底座静态零点校准（消除底座自带的约 7° 模具初始斜角）
  // 超过 25 秒后永久锁定基准向量，避免后续斜靠或倒置导致零点漂移
  if (now < 25000 && omega < 4.0f && fabsf(mag - 1.0f) < 0.04f && std::abs(int(mpu.leanX)) < 10 && unsigned(mpuTiltDeg()) < 15) {
    if (++mpu.stillFrames >= 50) {
      mpu.baseAx += (ax - mpu.baseAx) * 0.02f;
      mpu.baseAy += (ay - mpu.baseAy) * 0.02f;
      mpu.baseAz += (az - mpu.baseAz) * 0.02f;
      const float norm = sqrtf(mpu.baseAx*mpu.baseAx + mpu.baseAy*mpu.baseAy + mpu.baseAz*mpu.baseAz);
      if (norm > 0.01f) {
        mpu.baseAx /= norm; mpu.baseAy /= norm; mpu.baseAz /= norm;
      }
    }
  } else {
    mpu.stillFrames = 0;
  }

  // Gravity in the mounted plane (-100..100) with low-pass filter
  {
    const float nx = mpu.baseAx, ny = mpu.baseAy, nz = mpu.baseAz;
    float e1x, e1y, e1z;
    if (fabsf(nz) < 0.9f) { e1x = -ny; e1y = nx; e1z = 0.0f; }
    else                  { e1x = 0.0f; e1y = -nz; e1z = ny; }
    const float n1 = sqrtf(e1x*e1x + e1y*e1y + e1z*e1z);
    if (n1 > 0.01f) {
      e1x /= n1; e1y /= n1; e1z /= n1;
      const float e2x = ny*e1z - nz*e1y, e2y = nz*e1x - nx*e1z, e2z = nx*e1y - ny*e1x;
      const float lx = (ax*e1x + ay*e1y + az*e1z) * 100.0f;
      const float ly = (ax*e2x + ay*e2y + az*e2z) * 100.0f;
      const int8_t alongX = int8_t(std::max(-100.0f, std::min(100.0f, lx)));
      const int8_t alongY = int8_t(std::max(-100.0f, std::min(100.0f, ly)));
      const int8_t targetX = int8_t((mpu.leanSwap ? alongY : alongX) * mpu.leanSign);
      const int8_t targetY = mpu.leanSwap ? alongX : alongY;

      // 一阶低通滤波：敲击的瞬态毫秒尖峰被平滑过滤，物理手持倾斜平滑跟踪
      mpu.leanX = int8_t(mpu.leanX + (targetX - mpu.leanX) * 0.25f);
      mpu.leanY = int8_t(mpu.leanY + (targetY - mpu.leanY) * 0.25f);
    }
  }

  // ---- 物理倒置检测 (触发 180° 翻转'放我下来 / 我要掉啦') ----
  const bool flipped = mpu.isUpsideDown ? mpu.tiltCos < -0.8192f : mpu.tiltCos < -0.9063f;
  if (flipped == mpu.isUpsideDown) {
    mpu.flipSamples = 0;
  } else if (++mpu.flipSamples >= 3) {
    mpu.flipSamples = 0;
    mpu.isUpsideDown = flipped;
    dirty = true;
  }

  // ---- 2. Pitch Nudge (专属向后倒一下翻页，门限调优至 8，2 帧滤波，轻推即切，绝不拖泥带水) ----
  // a) 严格要求没有左右偏斜（|leanX| < 8，彻底排除任何左右碰动和侧倾！）
  // b) 明确前后轴向偏角（|leanY| >= 8，轻推一下即可轻松触发）
  // c) 2 帧滤波（约 60~80ms），既滤除毫秒级敲击微震，又保证后推响应灵敏！
  const bool isPureBackTilt = (std::abs(int(mpu.leanX)) < 8) && (std::abs(int(mpu.leanY)) >= 8);
  const bool isLeveled = (std::abs(int(mpu.leanX)) < 6) && (std::abs(int(mpu.leanY)) < 6);

  if (isLeveled) {
    mpu.pitchArmed = true; // 回正后重新武装
    mpu.pitchSamples = 0;
  } else if (isPureBackTilt && mpu.pitchArmed && omega < 90 && !mpu.isUpsideDown && !mpu.isDizzy && !mpu.isHeldUp) {
    if (++mpu.pitchSamples >= 2) { // 持续 2 帧确认人手向后推倒动作
      mpu.pitchSamples = 0;
      mpu.pitchArmed = false; // 翻页后锁死，后仰拿在手里绝不连翻！
      gesture.tapAt = 0;      // 彻底清除触碰表情，页面干净展示
      gesture.mood = 0;
      turnPage(1, now);
    }
  } else {
    mpu.pitchSamples = 0;
  }

  // ---- 拿起来与真正放回桌面检测（基于人手生理微震颤与桌面绝对静止滤波的滞环状态机） ----
  // a) 桌面绝对死寂判定：姿态回平，角速度接近零（< 2.0°/s），且无手部生理微震颤（deltaMag < 0.04g）
  const bool isLeveledOnDesk = (std::abs(int(mpu.leanX)) < 7) && (std::abs(int(mpu.leanY)) < 7) && (unsigned(mpuTiltDeg()) < 10);
  const bool isStationary = isLeveledOnDesk && (omega < 2.0f) && (deltaMag < 0.04f);

  if (isStationary) {
    if (mpu.deskStationaryFrames < 255) mpu.deskStationaryFrames++;
  } else {
    mpu.deskStationaryFrames = 0;
  }
  // 连续稳定 10 帧（约 300ms）绝对死寂，确认真机稳稳落回桌面上
  const bool confirmedOnDesk = mpu.deskStationaryFrames >= 10;

  // b) 触发拿起条件：姿态偏离基准 >= 33°，且非倒置。
  // 阈值稳稳高于所有桌面手势动作区间（LEAN 表情 ≥20°、双倾翻页 ≥22°、
  // 后倒翻页 |leanY|>=8）：轻微后仰即自然进入悬空拿起状态。
  const bool liftTrigger = (unsigned(mpuTiltDeg()) >= 33) && (!mpu.isUpsideDown);

  if (!mpu.isHeldUp) {
    // 桌面未拿起状态 -> 判定是否被拿起
    if (liftTrigger) {
      if (++mpu.heldSamples >= 3) { // 持续约 100ms 确认拿起
        mpu.heldSamples = 0;
        mpu.isHeldUp = true;
        dirty = true;
      }
    } else {
      mpu.heldSamples = 0;
    }
  } else {
    // 已处于手持悬空状态 -> 滞环保持！
    // 只要没有在桌面上稳定死寂确认放手离开，哪怕在空中平放端着，也坚决维持 isHeldUp = true！
    mpu.heldSamples = 0;
    // 回平保底通道：姿态持续回平约 1.1s（36 帧）即认定已放下。
    // 真实桌面有打字/风扇微震时 omega<2°/s 且 deltaMag<0.04g 的死寂判据
    // 可能永远凑不满 10 帧，没有这条路设备会永远卡在"放我下来"。
    // 代价是手端平 1.1s 也会退出——但表情让位给页面无害，再抬起来会重新触发。
    if (isLeveledOnDesk) {
      if (mpu.heldLevelFrames < 255) mpu.heldLevelFrames++;
    } else {
      mpu.heldLevelFrames = 0;
    }
    if (confirmedOnDesk || mpu.heldLevelFrames >= 36) {
      mpu.isHeldUp = false;
      mpu.heldLevelFrames = 0;
      dirty = true;
    }
  }

  // ---- 3. Tilt-to-Turn (双倾切页，遵循 AGENTS.md 约定) ----
  const int sideways = int(mpu.leanX);
  const bool bigTip = std::abs(sideways) >= 34; // 倾角约 20°~22°
  const bool levelNow = std::abs(sideways) < 12;

  // 拿起状态下严禁任何手势翻页：左右倾斜纯展示表情，双倾计数直接清零加锁；
  // 后倒翻页在 pitch nudge 分支里同样有 !isHeldUp 守卫。
  if (mpu.isUpsideDown || mpu.isDizzy || mpu.isHeldUp) {
    mpu.leanArmed = false;
    mpu.leanTaps = 0;
  } else if (bigTip && mpu.leanArmed && omega < 65) {
    mpu.leanArmed = false; // 一次倾斜只算一下
    const int8_t dir = sideways > 0 ? 1 : -1;
    if (mpu.leanTaps && now - mpu.leanLastTipAt < 1600 && dir == mpu.leanLastDir) {
      if (++mpu.leanTaps >= 2) {
        mpu.leanTaps = 0;
        mpu.leanConsumed = true; // 翻页瞬间脸立刻让位于新页面
        gesture.leanUntil = 0;
        turnPage(dir, now);
      }
    } else {
      mpu.leanTaps = 1;
      mpu.leanLastDir = dir;
    }
    mpu.leanLastTipAt = now;
  }
  if (levelNow) {
    mpu.leanArmed = true;
    mpu.leanConsumed = false;
    if (mpu.leanTaps && now - mpu.leanLastTipAt >= 1600) mpu.leanTaps = 0;
  }
}


// Pick a different palette entry each time the clock page returns, so the
// hue rotates without touching the firmware (palette lives in the shared
// renderer header so the Mac preview draws from the same list). A console
// pick from /ink?set=rrggbb pins the hue and silences the rotation.
void rotateClockInk() {
  if (desk::clockInkPinned()) return;
  desk::loftClockInk() = desk::pickClockInk(esp_random());
}

// Tilt away from the calibrated mount pose, in whole degrees. acos() on the
// filtered cosine; clamped because a loud sample can push it a hair past ±1.
uint8_t mpuTiltDeg() {
  const float cosTilt = std::clamp(mpu.tiltCos, -1.0f, 1.0f);
  return uint8_t(std::lround(std::acos(cosTilt) * 57.29578f));
}

// One code for the POSE row and the status JSON, so both always agree:
// 0 absent / 1 calibrating / 2 level / 3 tilted / 4 flipped / 5 shaken.
uint8_t mpuPoseCode() {
  if (!mpu.present) return 0;
  if (!mpu.calibrated) return 1;
  if (mpu.isUpsideDown || mpu.isHeldUp) return 4;
  if (mpu.isDizzy) return 5;
  return mpuTiltDeg() >= 8 ? 3 : 2;
}

// Which face the panel should wear this frame. Also stamps the start time so
// each mood plays its own entrance (the tap hops, the flip slips and is caught).
uint8_t activeGestureMood(uint32_t now) {
  uint8_t mood = moodOverride.load();
  if (!mood && mpu.present && mpu.calibrated) {
    // 互斥优先级（2026-11-24 定稿）：FLIP > LEAN > HELD > DIZZY > TAP > SETTLE。
    // LEAN 是"子姿态动态覆盖"：桌面倾斜与空中倾斜共用同一条倾斜表情；
    // 空中回正只留 100ms 短尾（桌面是 900ms），回正时设备仍悬空，
    // 立即回落到 HELD，不能说"接住了"。
    if (mpu.isUpsideDown) mood = desk::GESTURE_FLIP;
    else if (!mpu.leanConsumed) {
      // 仅判定侧向 tiltX，前后俯仰不触发侧倾，确保后倒翻页动作纯净不冲突
      const int tiltX = std::abs(int(mpu.leanX));
      if (tiltX >= 18 && (now - mpu.lastTapAt > 400))
        gesture.leanUntil = now + (mpu.isHeldUp ? 100 : 900);
      if (gesture.leanUntil && int32_t(now - gesture.leanUntil) < 0) mood = desk::GESTURE_LEAN;
    }
    if (!mood && mpu.isHeldUp) mood = desk::GESTURE_HELD;
    if (!mood && mpu.isDizzy) {
      // Shaking is not the same face as the hangover: while the gyro is still
      // hot it is SHAKE, and only once the hand stops does it turn into DIZZY.
      mood = (now - mpu.lastFastMotion < 700) ? desk::GESTURE_SHAKE : desk::GESTURE_DIZZY;
    }
    if (!mood && gesture.tapAt && now - gesture.tapAt < 1100) mood = desk::GESTURE_TAP;
    if (!mood && gesture.settleUntil && int32_t(now - gesture.settleUntil) < 0) mood = desk::GESTURE_SETTLE;
  }
  // 从倒转或拿起来放回桌面平放时，顺滑衔接 1.2 秒'接住了 / 好险'
  if ((gesture.mood == desk::GESTURE_FLIP || gesture.mood == desk::GESTURE_HELD) &&
      mood != desk::GESTURE_FLIP && mood != desk::GESTURE_HELD && mpu.calibrated)
    gesture.settleUntil = now + 1200;
  if (mood != gesture.mood) {
    gesture.mood = mood;
    gesture.startedAt = now;
    dirty = true;
  }
  return mood;
}

// One place that turns the live radio/board state into what the SYSTEM page
// prints, so the on-panel screenshot and the real frame can never disagree.
void fillLinkState(desk::ViewState& view) {
  view.wifi = data.wifi; view.httpCode = data.httpCode;
  // A dropped link has no address: printing the last known one on the SYSTEM
  // page would look like a live connection. Blank it instead.
  strlcpy(view.ip, data.wifi ? data.ip : "", sizeof(view.ip));
  view.rssi = data.wifi ? int8_t(WiFi.RSSI()) : 0;
  view.heapKb = uint16_t(ESP.getFreeHeap() / 1024u);
  view.clockSrc = clockSourceNow();
  // MPU6050 slot: live tilt away from the calibrated mount pose. Everything is
  // read locally, so the row answers the moment the module is soldered in.
  view.mpuPresent = mpu.present;
  view.tiltDeg = mpuTiltDeg();
  view.mpuPose = mpuPoseCode();
  view.otaInProgress = otaBusy.load() || (otaLastActivity.load() && millis() - otaLastActivity.load() < 5000u);
  view.otaProgressPct = otaTotal ? uint8_t(std::clamp<uint32_t>(otaAccepted * 100u / otaTotal, 0, 100)) : 0;
}

void drawCurrentPage() {
  desk::ViewState view;
  view.quota5h = quota5h;
  const desk::Page page = activeDisplayPage(view);
  // A gesture owns the whole panel for as long as it reads: the face is the
  // answer to "what did I just do to it", so it must not share the screen.
  const uint8_t mood = gesture.mood;
  // Uniform brightness on every page: the old loft-only x0.6 dim shifted how
  // artwork hues read on the panel and made preview/device comparison noisy.
  // Sleep window keeps its red-only art but drops the backlight hard so the
  // room stays dark; full colour returns to the day level untouched.
  // Applied AFTER Renderer::draw() latches redOnlyNight(), so the first frame
  // after a 22:30/06:00 flip is already at the right level (no 100ms flash).
  auto applyBrightness = []() {
#ifdef DESK_COLOR_TEST
    const uint8_t want = 25;  // wiring-verdict flashes: easy on eyes
#else
    const uint8_t day = uint8_t(theme.brightness);
    const uint8_t night = day > NIGHT_BRIGHTNESS ? NIGHT_BRIGHTNESS : day;
    const uint8_t want = desk::redOnlyNight() ? night : day;
#endif
    static int applied = -1;
    if (want != applied) {
      display->setBrightness8(want);
      applied = want;
      panelBrightness.store(want);
    }
  };
  // One blit for both painters: the RGB565 canvas lets the sky sample the
  // finished artwork, and keeps preview and panel on the same pixels.
  auto blit = [](const uint16_t* pixels) {
    display->fillScreen(0);
    for (int y=0;y<64;++y) for (int x=0;x<64;++x) {
      const uint16_t ink = pixels[y*64+x];
      if (ink) display->drawPixel(x,y,ink);
    }
  };
  // The RGB565 canvas lets the sky sample the finished artwork. Allocate it
  // once and share it between the pages and the reaction faces, so the two
  // never cost 16 KB of heap at the same time.
  static GFXcanvas16 canvas(64, 64);
  if (mood != desk::GESTURE_OFF) {
    // Faces live in the shared renderer, so they inherit the night red gate and
    // render identically in the Mac preview (no chip needed on the desk).
    const uint32_t now = uint32_t(millis());
    desk::Gesture face;
    face.mood = mood;
    face.now = now;
    face.since = now - gesture.startedAt;
    face.leanX = mpu.leanX;
    face.leanY = mpu.leanY;
    if (uint16_t* pixels = canvas.getBuffer()) {
      desk::Renderer<GFXcanvas16> renderer(canvas, controller, view, theme);
      renderer.drawGesture(face);
      blit(pixels);
    } else {
      desk::Renderer<MatrixPanel_I2S_DMA> renderer(*display, controller, view, theme);
      renderer.drawGesture(face);
    }
    applyBrightness();
    return;
  }
  fillLinkState(view);
  // Anchor for page-local phases (stock SH/NASDAQ rotation): restart the 6s
  // cadence whenever the displayed page changes, whatever caused the change.
  static desk::Page lastShownPage = desk::LOFT_PAGE;
  if (page != lastShownPage) {
    lastShownPage = page;
    visiblePageSince.store(uint32_t(controller.uptime()));
    if (page == desk::LOFT_PAGE) rotateClockInk();  // new hue on every return
  }
  view.pageShownAt = visiblePageSince.load();
  view.quotaWeek = quotaWeek;
  view.quotaAt = quotaAt; view.quotaStale = quotaStale;
  view.reset5hAt = data.reset5hAt;
  view.stock = data.stock;
  strlcpy(view.message, data.message, sizeof(view.message));
  view.messageUntil = messageUntil; view.messageStarted = messageStarted;
  if (uint16_t* pixels = canvas.getBuffer()) {
    desk::Renderer<GFXcanvas16> renderer(canvas, controller, view, theme);
    renderer.draw(page, pixels);
    blit(pixels);
  } else {
    desk::Renderer<MatrixPanel_I2S_DMA> renderer(*display, controller, view, theme);
    renderer.draw(page);
  }
  applyBrightness();  // redOnlyNight() is latched now: no one-frame lag
}

WebServer webServer(80);

// WebServer turns on TCP_NODELAY for the listening socket only, and lwIP does
// not carry that over to accepted connections. With Nagle on, every response
// pays a delayed-ACK round trip between the header write and the body write --
// ~150 ms on this link, on a console that polls every 1.2 s. Set it per
// accepted socket instead.
void settleSocket() { webServer.client().setNoDelay(true); }
// OTA without ArduinoOTA/mDNS: reuse the running WebServer so the idle cost
// stays near zero. Enabled only when a token is configured, so a bare LAN
// device never exposes firmware upload.
HTTPUpdateServer httpUpdater(true);

// ---- Resumable OTA --------------------------------------------------------
// /update (HTTPUpdateServer) wants the whole image in one multipart stream.
// This link cannot carry that: the AP relays between its 2.4 GHz radio (the
// ESP32) and the 5 GHz one (the Mac), so RTT jitters 100-200 ms and the
// socket tops out near 7 KB/s. A 1.2 MB stream dies at the lwIP
// retransmission timeout around 60-80% and curl reports "Broken pipe" having
// written nothing usable. /ota takes the same image as short raw-body POSTs
// and remembers how much is already in flash, so a dropped chunk costs one
// chunk instead of the whole push:
//   POST /ota/begin   prepare inactive slot, reset progress
//   GET  /ota/state    return the accepted resume prefix
//   POST /ota/chunk   receive a complete buffered slice before writing
//   POST /ota/end     validate the image, select slot, reboot
// The body may be raw (application/octet-stream) or multipart, so both
// tools/ota.py and a plain `curl -F` can drive it. Same Basic credentials as
// /update; a device without DESK_API_TOKEN exposes neither endpoint.
bool otaOpen = false;
esp_ota_handle_t otaHandle = 0;
const esp_partition_t* otaPartition = nullptr;
bool otaWriteBlocked = false;   // bad credentials, offset gap, or write error
size_t otaTotal = 0;
// The authoritative prefix includes bytes staged in RAM. A normal chunk end
// or interrupted-body abort flushes them before exposing a resumable offset.
// Do not substitute a flash-sector counter or the sender's guessed position.
size_t otaAccepted = 0;
constexpr size_t OTA_BUFFER_SIZE = 32768;
constexpr size_t OTA_FRAME_MAX = 61440; // 60KB (15 x 4KB flash sectors)
uint8_t* otaStorage = nullptr;
desk::OtaBuffer otaBuffer;
void otaFreeBuffer() { free(otaStorage);otaStorage=nullptr;otaBuffer.bind(nullptr,0); }
bool otaAllocateBuffer() {
  if (!otaStorage) { otaStorage=static_cast<uint8_t*>(malloc(OTA_BUFFER_SIZE));otaBuffer.bind(otaStorage,OTA_BUFFER_SIZE); }
  return otaStorage != nullptr;
}
uint64_t otaFlashUs = 0, otaChunkFlashStart = 0, otaChunkStarted = 0;
uint32_t otaChunkMs = 0, otaChunkWriteMs = 0, otaBeginMs = 0;
char otaError[64] = "";
uint32_t otaRestartAt = 0;
String otaAuthExpected;         // "Basic ..." for admin:<token>, built at boot

bool otaAuthorized() {
  if (!otaAuthExpected.length()) return false;
  return webServer.header("Authorization") == otaAuthExpected;
}

void otaReply(int code, bool ok) {
  char json[512];
  snprintf(json, sizeof(json),
           "{\"ok\":%s,\"open\":%s,\"progress\":%u,\"total\":%u,\"flash_ms\":%u,\"chunk_ms\":%u,\"chunk_flash_ms\":%u,\"begin_ms\":%u,\"stream\":true,\"preferred_chunk\":%u,\"target\":\"%s\"%s%s%s}",
           ok ? "true" : "false", otaOpen ? "true" : "false",
           unsigned(otaAccepted), unsigned(otaTotal), unsigned(otaFlashUs/1000),
           unsigned(otaChunkMs), unsigned(otaChunkWriteMs), unsigned(otaBeginMs),
           unsigned(OTA_FRAME_MAX),
           otaPartition ? otaPartition->label : "",
           otaError[0] ? ",\"err\":\"" : "", otaError[0] ? otaError : "",
           otaError[0] ? "\"" : "");
  webServer.send(code, "application/json", json);
}

// One rule for both body paths: only accept a chunk when the client's claimed
// offset matches the device accepted-byte counter. A duplicate byte range would corrupt
// the image silently, while a refused chunk is free -- the reply carries the
// real progress and the client resumes from there.
// The values ride in headers, not query arguments: the core parses a raw body
// before it parses the query string, so webServer.arg() is empty inside the
// upload callback. Headers are collected first for every method.
bool otaOffsetAgrees() {
  const String claimed = webServer.header("X-Ota-Offset");
  if (!claimed.length()) return otaAccepted == 0;
  const size_t want = size_t(strtoul(claimed.c_str(), nullptr, 10));
  if (want == otaAccepted) return true;
  snprintf(otaError, sizeof(otaError), "offset %u != %u",
           unsigned(want), unsigned(otaAccepted));
  return false;
}

bool otaFlashWrite(const uint8_t* data, size_t len) {
  const uint64_t started = esp_timer_get_time();
  const esp_err_t written = esp_ota_write(otaHandle, data, len);
  otaFlashUs += esp_timer_get_time()-started;
  webLastBeat.store(millis());
  if (written == ESP_OK) return true;
  otaWriteBlocked=true;otaOpen=false;
  snprintf(otaError,sizeof(otaError),"%s",esp_err_to_name(written));
  esp_ota_abort(otaHandle);
  otaLastActivity.store(0);
  return false;
}
void otaFlush() {
  if (!otaBuffer.flush(otaFlashWrite)) { otaWriteBlocked=true;otaFreeBuffer(); }
}
void otaWrite(uint8_t* data, size_t len) {
  if (otaWriteBlocked || !len) return;
  otaLastActivity.store(millis());
  if (otaAccepted + len > otaTotal) {
    otaWriteBlocked=true;snprintf(otaError,sizeof(otaError),"past total %u",unsigned(otaTotal));return;
  }
  if (!otaBuffer.append(data,len,otaFlashWrite)) { otaWriteBlocked=true;otaFreeBuffer();return; }
  otaAccepted += len;
}

void otaStartChunk() {
  otaChunkStarted = esp_timer_get_time();
  otaChunkFlashStart = otaFlashUs;
  otaBusy.store(true);
  otaLastActivity.store(millis());
  otaWriteBlocked = !otaOffsetAgrees();
  if (!otaWriteBlocked && !otaAllocateBuffer()) { otaWriteBlocked=true;snprintf(otaError,sizeof(otaError),"OTA buffer allocation failed"); }
  if (!otaWriteBlocked) otaError[0] = 0;
  // 1.4 KB blocks take ~200 ms at 7 KB/s, but a stalled AP can hold them for
  // seconds; the default 1 s read timeout would abort a perfectly good chunk.
  webServer.client().setTimeout(15000);
}

// Streams one request body into the OTA slot. Called for both raw bodies and
// multipart uploads; the raw path is what tools/ota.py uses because the core
// hands it 1436-byte blocks instead of scanning the body byte by byte.
void handleOtaBody() {
  if (!otaAuthorized() || !otaOpen) { otaWriteBlocked = true; return; }

  if (webServer.hasRaw()) {
    HTTPRaw& raw = webServer.raw();
    if (raw.status == RAW_START) {
      otaStartChunk();
    } else if (raw.status == RAW_WRITE) {
      otaWrite(raw.buf, raw.currentSize);
    } else if (raw.status == RAW_END) {
      if (otaOpen) otaFlush();
    } else if (raw.status == RAW_ABORTED) {
      if (otaOpen) otaFlush();
      otaFreeBuffer();
      otaLastActivity.store(0);
      // Preserve the staged partial prefix before exposing a resume offset.
      otaWriteBlocked = true;
      otaBusy.store(false);
      snprintf(otaError, sizeof(otaError), "chunk aborted");
    }
    return;
  }

  HTTPUpload& up = webServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaStartChunk();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    otaWrite(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaOpen) otaFlush();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (otaOpen) otaFlush();
    otaFreeBuffer();
    otaLastActivity.store(0);
    otaWriteBlocked = true;
    otaBusy.store(false);
    snprintf(otaError, sizeof(otaError), "chunk aborted");
  }
}

bool otaDeny() {
  if (otaAuthorized()) return false;
  otaError[0] = 0;
  webServer.send(401, "application/json", "{\"ok\":false,\"err\":\"auth\"}");
  return true;
}

void handleOtaBegin() {
  settleSocket();
  if (otaDeny()) return;
  const String size = webServer.header("X-Ota-Total");
  otaTotal = size.length() ? size_t(strtoul(size.c_str(), nullptr, 10)) : 0;
  otaError[0] = 0;
  if (otaOpen) esp_ota_abort(otaHandle);
  otaFreeBuffer();
  otaLastActivity.store(0);
  if (Update.isRunning()) Update.abort(); // legacy /update cannot share a target
  otaOpen = false;
  otaAccepted = 0;
  otaFlashUs = 0; otaChunkMs = otaChunkWriteMs = otaBeginMs = 0;
  if (otaTotal < 65536 || otaTotal > 0x200000) {
    snprintf(otaError, sizeof(otaError), "bad total %u", unsigned(otaTotal));
    otaReply(400, false);
    return;
  }
  otaPartition = esp_ota_get_next_update_partition(nullptr);
  if (!otaPartition || otaTotal > otaPartition->size) {
    snprintf(otaError, sizeof(otaError), "image exceeds inactive slot");
    otaReply(400, false);return;
  }
  // Erase before body reception. Incremental 64KB erases during the stream
  // can delay TCP ACKs while data is arriving; keep this out of reception.
  // The running partition is never a write target; IDF enforces this too.
  if (!otaAllocateBuffer()) {
    snprintf(otaError,sizeof(otaError),"OTA buffer allocation failed");otaReply(503,false);return;
  }
  otaBusy.store(true);
  const uint32_t beginAt = millis();
  const esp_err_t begun = esp_ota_begin(otaPartition, otaTotal, &otaHandle);
  otaBeginMs = millis()-beginAt;
  if (begun != ESP_OK) {
    snprintf(otaError, sizeof(otaError), "%s", esp_err_to_name(begun));
    otaFreeBuffer();otaReply(500, false);return;
  }
  otaLastActivity.store(millis());
  otaOpen = true;
  otaReply(200, true);
}

void handleOtaState() {
  settleSocket();
  if (otaDeny()) return;
  otaReply(200, true);            // progress is the resume point either way
}

void handleOtaEnd() {
  settleSocket();
  if (otaDeny()) return;
  if (!otaOpen) {
    snprintf(otaError, sizeof(otaError), "no open session");
    otaReply(409, false);
    return;
  }
  if (otaAccepted != otaTotal) {
    snprintf(otaError, sizeof(otaError), "short image %u of %u",
             unsigned(otaAccepted), unsigned(otaTotal));
    otaReply(409, false);
    return;
  }
  // IDF frees the handle even if verification fails. Do not change the boot
  // target until full image validation (including the embedded hash) passes.
  otaFlush();
  if (!otaOpen) { otaReply(500,false);return; }
  otaFreeBuffer();
  otaLastActivity.store(0);
  const esp_err_t verified = esp_ota_end(otaHandle);
  otaOpen = false;
  if (verified != ESP_OK) {
    snprintf(otaError, sizeof(otaError), "%s", esp_err_to_name(verified));
    otaReply(500, false);return;
  }
  const esp_err_t selected = esp_ota_set_boot_partition(otaPartition);
  if (selected != ESP_OK) {
    snprintf(otaError, sizeof(otaError), "%s", esp_err_to_name(selected));
    otaReply(500, false);return;
  }
  otaBusy.store(false);
  otaError[0] = 0;
  otaReply(200, true);
  otaRestartAt = millis() + 800;  // let the reply flush first
}

// Reply to a chunk we just streamed. The body itself was handled by
// handleOtaBody() before this runs.
void handleOtaChunkDone() {
  otaChunkMs = uint32_t((esp_timer_get_time()-otaChunkStarted)/1000);
  otaChunkWriteMs = uint32_t((otaFlashUs-otaChunkFlashStart)/1000);
  otaBusy.store(false);
  settleSocket();
  if (otaDeny()) return;
  otaReply(otaWriteBlocked ? 409 : 200, !otaWriteBlocked);
}

// One authenticated connection, bounded frames and one progress ACK per frame.
// The sender waits for this ACK before sending more bytes, so neither flash
// writes nor repeated TCP handshakes can compete with incoming OTA data.
void handleOtaStream() {
  settleSocket();
  if (otaDeny()) return;
  if (!otaOpen) { otaReply(409,false);return; }
  otaStartChunk();
  if (otaWriteBlocked) { otaReply(409,false);return; }
  WiFiClient client=webServer.client();
  client.print("HTTP/1.1 101 Switching Protocols\r\nUpgrade: desk-ota/1\r\nConnection: Upgrade\r\n\r\n");
  uint8_t block[1436];
  bool complete=false;
  for (;;) {
    uint8_t header[2];
    if (client.readBytes(header,2)!=2) break;
    const size_t length=(size_t(header[0])<<8)|header[1];
    if (length>OTA_FRAME_MAX || otaAccepted+length>otaTotal) break;
    otaChunkStarted=esp_timer_get_time();otaChunkFlashStart=otaFlashUs;
    otaLastActivity.store(millis());
    size_t remaining=length;
    while (remaining && !otaWriteBlocked) {
      const size_t want=min(remaining,sizeof(block));
      const size_t got=client.readBytes(block,want);
      if (got) { otaWrite(block,got);remaining-=got; }
      if (got!=want) break;
    }
    if (otaOpen) otaFlush();
    otaChunkMs=uint32_t((esp_timer_get_time()-otaChunkStarted)/1000);
    otaChunkWriteMs=uint32_t((otaFlashUs-otaChunkFlashStart)/1000);
    if (remaining) break; // the staged prefix was flushed for a later resume
    char ack[160];
    const int n=snprintf(ack,sizeof(ack),
      "{\"ok\":%s,\"progress\":%u,\"flash_ms\":%u,\"chunk_ms\":%u,\"chunk_flash_ms\":%u}\n",
      !otaWriteBlocked?"true":"false",unsigned(otaAccepted),unsigned(otaFlashUs/1000),
      unsigned(otaChunkMs),unsigned(otaChunkWriteMs));
    if (client.write(reinterpret_cast<const uint8_t*>(ack),size_t(n))!=size_t(n)) break;
    if (!length) { complete=true;break; }
    if (otaWriteBlocked) break;
  }
  if (!complete) {
    if (otaOpen) otaFlush();
    snprintf(otaError,sizeof(otaError),"stream interrupted; prefix saved");
  }
  otaFreeBuffer();otaLastActivity.store(0);otaBusy.store(false);
  client.stop();
}

// Modem sleep is what makes this board feel slow. With PS on, the radio only
// wakes on DTIM, so a 435-byte /api/status takes 1-2 s, a 12 KB screenshot
// crawls at 300 B/s and a 1.2 MB OTA stream dies at the retransmission
// timeout. Arduino caches setSleep() until STA_START and a radio recycle can
// drop it back to MIN_MODEM, so re-assert it after every association and
// publish the value the driver actually reports.
void applyRadioPolicy() {
  esp_wifi_set_ps(WIFI_PS_NONE);
  wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
  if (esp_wifi_get_ps(&ps) == ESP_OK) wifiPsMode.store(uint8_t(ps));
  Serial.printf("WiFi power save: ps=%u (0=none,1=min,2=max)\n",
                unsigned(wifiPsMode.load()));
}

void updateHeartbeatLed(bool wifi) {
  uint32_t now = millis();
  bool ledOn = false;
  if (wifi) {
    uint32_t cycle = now % 1000;
    ledOn = (cycle < 40) || (cycle >= 140 && cycle < 180);
  } else {
    ledOn = (now % 300) < 150;
  }
  digitalWrite(HEARTBEAT_LED_PIN, ledOn ? HIGH : LOW);
}

void handleScreenshot() {
  settleSocket();
  uint8_t* bmp = (uint8_t*)malloc(12342);
  if (!bmp) {
    webServer.send(503, "text/plain", "Out of memory");
    return;
  }
  static const uint8_t BMP_HEADER[54] = {
    0x42, 0x4d, 0x36, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  memcpy(bmp, BMP_HEADER, 54);

  GFXcanvas16 canvas(64, 64);
  if (!canvas.getBuffer()) {
    free(bmp);
    webServer.send(503, "text/plain", "Out of memory");
    return;
  }
  desk::ViewState view;
  fillLinkState(view);
  view.quota5h = quota5h; view.quotaWeek = quotaWeek;
  view.quotaAt = quotaAt; view.quotaStale = quotaStale;
  view.reset5hAt = data.reset5hAt;
  view.stock = data.stock;
  strlcpy(view.message, data.message, sizeof(view.message));
  view.messageUntil = messageUntil; view.messageStarted = messageStarted;
  desk::Renderer<GFXcanvas16> renderer(canvas, controller, view, theme);
  view.pageShownAt = visiblePageSince.load();
  if (gesture.mood != desk::GESTURE_OFF) {
    // Mirror what the panel is actually showing, so a screenshot taken while
    // the thing is being shaken captures the face, not the page underneath.
    const uint32_t nowMs = uint32_t(millis());
    desk::Gesture face;
    face.mood = gesture.mood; face.now = nowMs; face.since = nowMs - gesture.startedAt;
    face.leanX = mpu.leanX; face.leanY = mpu.leanY;
    renderer.drawGesture(face);
  } else {
    renderer.draw(activeDisplayPage(view), canvas.getBuffer());
  }

  const uint16_t* buffer = canvas.getBuffer();
  uint8_t* ptr = bmp + 54;
  for (int y = 63; y >= 0; --y) {
    for (int x = 0; x < 64; ++x) {
      uint16_t rgb = buffer[y * 64 + x];
      *ptr++ = (rgb & 0x1F) * 255 / 31;          // B
      *ptr++ = ((rgb >> 5) & 0x3F) * 255 / 63;   // G
      *ptr++ = ((rgb >> 11) & 0x1F) * 255 / 31;  // R
    }
  }
  webServer.setContentLength(12342);
  webServer.send(200, "image/bmp", "");
  WiFiClient client = webServer.client();
  // The default 1 s write timeout and the old 2 s give-up both fired on a
  // merely slow 2.4G link (RTT spikes to ~500 ms), truncating the BMP after
  // one 2 KB chunk while curl still waited for the declared Content-Length.
  client.setTimeout(5000);
  size_t sent = 0;
  uint32_t lastProgress = millis();
  while (sent < 12342 && client.connected()) {
    const size_t chunk = min((size_t)2048, (size_t)(12342 - sent));
    const size_t wrote = client.write(bmp + sent, chunk);
    if (!wrote) {
      // A client that closed mid-download used to spin this loop forever:
      // write() returned 0, sent never moved, and the single-threaded web
      // server stopped answering /api/status and the page itself. Allow a
      // longer quiet spell before deciding the socket is dead.
      if (millis() - lastProgress > 8000) break;
      delay(1);
      continue;
    }
    sent += wrote;
    lastProgress = millis();
    delay(1);  // let the WiFi stack drain before the buffer is freed
  }
  client.flush();
  free(bmp);
}

void handleSetBrightness() {
  int b = webServer.arg("val").toInt();
  if (b < 8 || b > 160) {
    webServer.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  if (!brightnessCommands) {
    webServer.send(503, "application/json", "{\"ok\":false}");
    return;
  }
  const BrightnessCommand command{uint8_t(b), uint64_t(esp_timer_get_time() / 1000)};
  xQueueOverwrite(brightnessCommands, &command);
  webServer.send(200, "application/json", "{\"ok\":true}");
}

static const char HTML_INDEX[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head><meta charset="utf-8">
<title>ESP32 硬件实时控制台</title><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box;}
body{background:#090d16;color:#e2e8f5;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;text-align:center;padding:20px 14px;margin:0;}
h1{font-size:18px;font-weight:600;letter-spacing:1px;margin:0 0 14px;color:#a4b8df;}
.stage-wrap{display:inline-block;padding:10px;background:#121826;border-radius:14px;box-shadow:0 12px 36px rgba(0,0,0,0.65);border:1px solid #1f2a40;}
.screen-box{position:relative;width:320px;height:320px;max-width:80vw;max-height:80vw;margin:0 auto;}
.screen-img{position:absolute;top:0;left:0;width:100%;height:100%;image-rendering:pixelated;border-radius:6px;transition:opacity 0.2s ease-in-out;}
.screen-img.live{opacity:1;z-index:2;}
.screen-img.hidden{opacity:0;z-index:1;}
.controls{margin-top:16px;display:flex;justify-content:center;gap:8px;flex-wrap:wrap;max-width:440px;margin-left:auto;margin-right:auto;}
.btn{background:#182236;color:#a5b6d8;border:1px solid #283754;padding:9px 13px;border-radius:8px;font-size:13px;font-weight:500;cursor:pointer;transition:all 0.15s ease;user-select:none;}
.btn:hover{background:#23314d;color:#fff;border-color:#3d537f;}
.btn.active{background:#183c48;color:#64dfdf;border-color:#64dfdf;box-shadow:0 0 12px rgba(100,223,223,0.3);}
.mode-bar{margin-top:14px;display:flex;justify-content:center;gap:10px;}
.mode-btn{background:#151d2f;color:#8da2c7;border:1px solid #23304a;padding:6px 14px;border-radius:20px;font-size:12px;cursor:pointer;transition:all 0.15s ease;}
.mode-btn.active{background:#1f3630;color:#4ddbb3;border-color:#4ddbb3;}
.info-bar{margin-top:16px;display:flex;gap:8px;justify-content:center;flex-wrap:wrap;font-size:12px;color:#8092b3;}
.tag{background:#141c2c;padding:4px 10px;border-radius:12px;border:1px solid #202c44;}
.tag b{color:#64dfdf;font-weight:600;}
</style></head><body>
<h1>❖ ESP32 硬件实时控制台</h1>
<div class="stage-wrap">
  <div class="screen-box">
    <img id="imgA" class="screen-img live" src="/screen.bmp" alt="Screen">
    <img id="imgB" class="screen-img hidden" src="" alt="Screen">
  </div>
</div>
<div class="controls">
  <button class="btn" id="btn0" onclick="setPage(0)">🏠 阁楼时钟</button>
  <button class="btn" id="btn1" onclick="setPage(1)">⚡ 能量额度</button>
  <button class="btn" id="btn2" onclick="setPage(2)">⚙ 系统状态</button>
  <button class="btn" id="btn3" onclick="setPage(3)">📈 股票行情</button>
</div>
<div class="mode-bar">
  <button class="mode-btn" id="carouselBtn" onclick="toggleCarousel()">🔄 智能情境轮播中</button>
  <button class="mode-btn" id="nightBtn" onclick="cycleNight()">🌙 夜间: 自动</button>
  <button class="mode-btn" onclick="nextInk()">🎨 时钟换色</button>
</div>
<div class="info-bar">
  <span class="tag">当前画面: <b id="tPage">--</b></span>
  <span class="tag">运行模式: <b id="tMode">--</b></span>
  <span class="tag">剩余内存: <b id="tHeap">-- KB</b></span>
  <span class="tag">系统开机: <b id="tUptime">-- s</b></span>
</div>
<script>
let curActive='imgA',fetching=false,timer=null,nightMode=0;
const pageNames=["01/阁楼时钟","02/能量额度","03/系统状态","04/股票行情"];
function updateFrame(){
  if(fetching||document.hidden)return;
  fetching=true;
  const targetId=(curActive==='imgA')?'imgB':'imgA';
  const targetImg=document.getElementById(targetId),curImg=document.getElementById(curActive);
  const loader=new Image();
  loader.onload=()=>{
    targetImg.src=loader.src;
    targetImg.className='screen-img live';
    curImg.className='screen-img hidden';
    curActive=targetId;
    fetching=false;
    refreshStatus();
    timer=setTimeout(updateFrame,1200);
  };
  loader.onerror=()=>{fetching=false;timer=setTimeout(updateFrame,2500);};
  loader.src='/screen.bmp?t='+Date.now();
}
function refreshStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('tPage').innerText=pageNames[d.page]||d.page;
    document.getElementById('tMode').innerText=d.mode;
    document.getElementById('tHeap').innerText=d.heap+' KB';
    document.getElementById('tUptime').innerText=d.uptime+' s';
    for(let i=0;i<4;i++){
      const b=document.getElementById('btn'+i);
      if(b)b.className=(d.page===i)?'btn active':'btn';
    }
    const cb=document.getElementById('carouselBtn');
    if(d.carousel){
      cb.innerText='🔄 智能情境轮播中 (点击锁定)';
      cb.className='mode-btn active';
    }else{
      cb.innerText='🔒 页面已锁定 (点击恢复轮播)';
      cb.className='mode-btn';
    }
    nightMode=d.night|0;
    const nb=document.getElementById('nightBtn');
    nb.innerText=['🌙 夜间: 自动','🌙 夜间: 强制红光','🌙 夜间: 强制彩色'][nightMode]||'🌙 夜间';
    nb.className=(nightMode===1)?'mode-btn active':'mode-btn';
  }).catch(()=>{});
}
function setPage(p){
  fetch('/page?set='+p).then(()=>{refreshStatus();if(!fetching){clearTimeout(timer);updateFrame();}});
}
function toggleCarousel(){
  const cb=document.getElementById('carouselBtn');
  const nowEn=cb.className.includes('active');
  fetch('/carousel?set='+(nowEn?'0':'1')).then(()=>{refreshStatus();if(!fetching){clearTimeout(timer);updateFrame();}});
}
function cycleNight(){
  fetch('/night?set='+((nightMode+1)%3)).then(()=>{refreshStatus();if(!fetching){clearTimeout(timer);updateFrame();}});
}
function nextInk(){
  fetch('/ink?set=next').then(()=>{if(!fetching){clearTimeout(timer);updateFrame();}});
}
document.addEventListener('visibilitychange',()=>{if(!document.hidden&&!fetching)updateFrame();});
updateFrame();
</script></body></html>)rawliteral";

void handleRoot() {
  settleSocket();
  webServer.send_P(200, "text/html", HTML_INDEX);
}

void handleSetPage() {
  settleSocket();
  if (panelMacLocked.load()) {
    webServer.send(409, "application/json", "{\"error\":\"Mac session is locked\"}");
    return;
  }
  if (webServer.hasArg("set")) {
    int p = webServer.arg("set").toInt();
    if (p >= 0 && p <= 3) {
      lastWebControlAt.store(millis());
      if (webServer.arg("hold") == "1") carousel.enabled = false;
      carousel.manualPage = p;
      carousel.manualUntil = millis() + 45000; // 45秒后自动安全恢复轮播，防永久卡死
      carousel.currentSlot = p;
      dirty = true;
    }
  }
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleSetCarousel() {
  settleSocket();
  if (panelMacLocked.load()) {
    webServer.send(409, "application/json", "{\"error\":\"Mac session is locked\"}");
    return;
  }
  if (webServer.hasArg("set")) {
    int c = webServer.arg("set").toInt();
    lastWebControlAt.store(millis());
    if (c != 0) {
      resumeRequested.store(true);
    } else carousel.enabled = false;
    dirty = true;
  }
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleSetNight() {
  // Manual sleep-window override: 0=auto (22:30–06:00 schedule), 1=force
  // red-only, 2=force full color. Same command shape as /page and /carousel.
  if (webServer.hasArg("set")) {
    int n = webServer.arg("set").toInt();
    if (n >= 0 && n <= 2) {
      desk::nightOverride() = n;
      dirty = true;
    }
  }
  webServer.send(200, "application/json", "{\"ok\":true}");
}

// Replay a reaction face on demand: /mood?set=0..6 (0 = back to live sensors).
void handleSetMood() {
  if (webServer.hasArg("set")) {
    const int n = webServer.arg("set").toInt();
    if (n >= 0 && n <= desk::GESTURE_SETTLE) {
      moodOverride.store(uint8_t(n));
      gesture.startedAt = uint32_t(millis());
      dirty = true;
    }
  }
  char json[64];
  snprintf(json, sizeof(json), "{\"ok\":true,\"mood\":%u}", unsigned(moodOverride.load()));
  webServer.send(200, "application/json", json);
}

// Which way is screen-right depends on how the chip sits on the module, which
// is unknowable until it is soldered in. /lean?set=-1 mirrors the two tilt
// directions without a reflash; /lean alone reports what the sensor sees, so
// the correct sign can be found by tilting the panel and reading lean.
void handleLean() {
  if (webServer.hasArg("set")) {
    const int sign = webServer.arg("set").toInt();
    if (sign == 1 || sign == -1) mpu.leanSign = int8_t(sign);
  }
  if (webServer.hasArg("swap")) {
    mpu.leanSwap = webServer.arg("swap").toInt() != 0;
  }
  char json[128];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"sign\":%d,\"swap\":%s,\"lean\":[%d,%d],\"tilt\":%u,\"taps\":%u,\"armed\":%s}",
           int(mpu.leanSign), mpu.leanSwap ? "true" : "false",
           int(mpu.leanX), int(mpu.leanY), unsigned(mpuTiltDeg()),
           unsigned(mpu.leanTaps), mpu.leanArmed ? "true" : "false");
  webServer.send(200, "application/json", json);
}

// Re-zero the mount pose without a power cycle: /cal. The reference vector
// lives in RAM (it is learned from whatever pose the panel sits in for ~1 s
// after boot), so moving the stand used to mean a reboot. This re-runs the
// same 50-sample average: leave the panel still for a second and it settles.
void handleCalibrate() {
  const bool wasPresent = mpu.present;
  mpu.calibrated = false;
  mpu.calibCount = 0;
  mpu.sumAx = mpu.sumAy = mpu.sumAz = 0;
  mpu.tiltCos = 1.0f;
  mpu.isUpsideDown = false;
  mpu.isDizzy = false;
  mpu.flipSamples = 0;
  dirty = true;
  char json[64];
  snprintf(json, sizeof(json), "{\"ok\":true,\"present\":%s,\"calibrating\":true}",
           wasPresent ? "true" : "false");
  webServer.send(200, "application/json", json);
}

// Hero-clock ink, changed live so a hue never needs a reflash:
//   /ink?set=auto     hand the clock back to the curated rotation
//   /ink?set=next     rotate one step now
//   /ink?set=rrggbb   pin this colour until told otherwise
void handleSetInk() {
  const String value = webServer.arg("set");
  if (value == "auto") {
    desk::clockInkPinned() = false;
    desk::loftClockInk() = 0;
  } else if (value == "next") {
    desk::clockInkPinned() = false;
    rotateClockInk();
  } else if (value.length() == 6) {
    uint32_t parsed = 0;
    bool valid = true;
    for (int i = 0; i < 6; ++i) {
      const char ch = value[i];
      int digit;
      if (ch >= '0' && ch <= '9') digit = ch - '0';
      else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
      else { valid = false; break; }
      parsed = (parsed << 4) | uint32_t(digit);
    }
    if (valid) {
      desk::loftClockInk() = parsed;
      desk::clockInkPinned() = true;
    }
  }
  dirty = true;
  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"ink\":\"#%06x\"}", unsigned(desk::loftClockInk()));
  webServer.send(200, "application/json", json);
}

void handleStatusApi() {
  settleSocket();
  desk::ViewState view; view.quota5h = quota5h;
  int cur = activeDisplayPage(view);
  char json[960];
  snprintf(json, sizeof(json),
    "{\"page\":%d,\"mode\":\"%s\",\"mac_locked\":%s,\"mac_online\":%s,\"mac_http\":%d,\"wifi\":%s,\"rssi\":%ld,\"ps\":%u,\"chan\":%u,\"ip\":\"%s\",\"carousel\":%s,\"manual\":%d,\"heap\":%u,\"uptime\":%u,\"quota\":%d,\"loft_scene\":%u,\"mpu\":%s,\"mpu_addr\":\"0x%02x\",\"mpu_cal\":%s,\"mpu_pose\":%u,\"mood\":%u,\"tilt\":%u,\"brightness\":%u,\"bright_base\":%u,\"night\":%d,\"clock\":%lld,\"clock_src\":\"%s\",\"ink\":\"#%06x\",\"ink_pin\":%s,\"wifi_reconnects\":%u,\"wifi_reason\":%u,\"wifi_drop\":%u,\"drops\":%u,\"reset\":%u,\"boot_part\":\"%s\",\"build\":\"%s %s\",\"refresh_hz\":%d}",
    cur,
    controller.mode == desk::WORK ? "WORK" : "FUN",
    panelMacLocked.load() ? "true" : "false",
    panelMacOnline.load() ? "true" : "false",
    panelMacHttp.load(),
    WiFi.status() == WL_CONNECTED ? "true" : "false",
    long(WiFi.RSSI()), unsigned(wifiPsMode.load()), unsigned(WiFi.channel()),
    WiFi.localIP().toString().c_str(),
    carousel.enabled && carousel.manualPage < 0 && !controller.manualRemainingMs() ? "true" : "false",
    carousel.manualPage,
    ESP.getFreeHeap() / 1024,
    uint32_t(millis() / 1000),
    quota5h,
    unsigned(desk::loftSceneIndex()),
    mpu.present ? "true" : "false",
    unsigned(mpu.i2cAddr),
    mpu.calibrated ? "true" : "false",
    unsigned(mpuPoseCode()),
    unsigned(gesture.mood),
    unsigned(mpuTiltDeg()),
    unsigned(panelBrightness.load()),
    unsigned(theme.brightness),
    desk::nightOverride(),
    (long long)controller.epoch(),
    clockSourceNow() == 1 ? "ntp" : (clockSourceNow() == 2 ? "mac" : "none"),
    unsigned(desk::loftClockInk()),
    desk::clockInkPinned() ? "true" : "false",
    unsigned(wifiReconnects.load()),
    unsigned(wifiLastReason.load()),
    unsigned(wifiLastDropAt.load()),
    unsigned(wifiDropCount.load()),
    unsigned(esp_reset_reason()),
    esp_ota_get_running_partition()->label, __DATE__, __TIME__,
    display ? display->calculated_refresh_rate : 0
  );
  webServer.send(200, "application/json", json);
}

void updateCarousel(uint32_t now) {
  if (!controller.macLocked && carousel.manualPage >= 0) {
    if (carousel.manualUntil > 0 && now >= carousel.manualUntil) {
      carousel.manualPage = -1;
      carousel.manualUntil = 0;
      carousel.lastSwitch = now;
      dirty = true;
    } else {
      return;
    }
  }
  if (!controller.macLocked && !carousel.enabled) return;

  // Slots are Page values: 0=LOFT(clock), 1=QUOTA, 2=STATUS, 3=STOCK.
  // WORK: quota 30s -> stock 12s -> loft breather 15s. FUN: loft 30s -> stock 12s.
  // STATUS never runs in the carousel.
  uint32_t elapsed = now - carousel.lastSwitch;
  if (controller.mode == desk::FUN) {
    if (carousel.currentSlot == 0) {
      if (elapsed >= desk::FUN_LOFT_MS) { carousel.currentSlot = 3; carousel.lastSwitch = now; dirty = true; }
    } else if (carousel.currentSlot == 3) {
      if (elapsed >= desk::STOCK_PAGE_MS) { carousel.currentSlot = 0; carousel.lastSwitch = now; dirty = true; }
    } else {
      carousel.currentSlot = 0; carousel.lastSwitch = now; dirty = true; // landed from WORK
    }
    return;
  }

  if (carousel.currentSlot == 1) {
    if (elapsed >= 30000) { carousel.currentSlot = 3; carousel.lastSwitch = now; dirty = true; }
  } else if (carousel.currentSlot == 3) {
    if (elapsed >= desk::STOCK_PAGE_MS) { carousel.currentSlot = 0; carousel.lastSwitch = now; dirty = true; }
  } else if (carousel.currentSlot == 0) {
    if (elapsed >= 15000) { carousel.currentSlot = 1; carousel.lastSwitch = now; dirty = true; }
  } else {
    carousel.currentSlot = 1; carousel.lastSwitch = now; dirty = true; // STATUS landed from FUN
  }
}

void snapshotTask(void*) {
  uint32_t lastPoll = millis() - 1500;
  uint32_t interval = 1500;
  bool wasWifi = false;
  for (;;) {
    const uint32_t activity=otaLastActivity.load();
    if (otaBusy.load() || (activity && millis()-activity<15000u)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    uint32_t now = millis();
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (wifi != wasWifi || now - lastPoll >= interval) {
      wasWifi = wifi;
      Snapshot next;
      next.wifi = wifi;
      strlcpy(next.ip, WiFi.localIP().toString().c_str(), sizeof(next.ip));
      next.requestStarted = millis();
      if (wifi && strlen(DESK_MAC_HOST) && strlen(DESK_API_TOKEN)) {
        HTTPClient http;
        http.setConnectTimeout(2300); http.setTimeout(2500);
        http.useHTTP10(true); // raw JSON stream, no chunked transfer framing
        if (http.begin(String(DESK_MAC_HOST) + "/state")) {
          http.addHeader("Authorization", String("Bearer ") + DESK_API_TOKEN);
          next.httpCode = http.GET();
          int size = http.getSize();
          if (next.httpCode == 200 && size > 0 && size <= 4096) {
            // Read exactly Content-Length with a total deadline, not just an
            // idle timeout that a trickling peer can extend indefinitely.
            char body[4097]; size_t count = 0;
            auto* stream = http.getStreamPtr();
            uint32_t started = millis();
            while (count < size && millis() - started < 2000) {
              int available = stream->available();
              if (available > 0) count += stream->readBytes(body + count, min(size_t(available), size_t(size) - count));
              else if (!http.connected()) break;
              else vTaskDelay(pdMS_TO_TICKS(5));
            }
            body[count] = 0;
            if (count == size) desk::decodeSnapshot(body, count, next);
          }
          http.end();
        }
      }
      next.receivedAt = esp_timer_get_time() / 1000;
      xQueueOverwrite(snapshots, &next);
      interval = next.valid ? 750 : min(interval * 2, uint32_t(3000));
      lastPoll = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiEverHadIp.store(true);
    webResetRequested.store(true);
  }
  // Keep the raw reason (wifi_err_reason_t) so a recurring auth/DHCP/beacon
  // loss shows up in /api/status instead of an unexplained "offline".
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiLastReason.store(uint8_t(info.wifi_sta_disconnected.reason));
    wifiLastDropAt.store(uint32_t(millis() / 1000));
    wifiDropCount.store(uint16_t(wifiDropCount.load() + 1));
    // A reason identifies the disconnect category, not its physical root cause.
    // In particular 36 (STA_LEAVING) alone does not establish an AP fault.
    Serial.printf("WiFi down: reason %u (ps %u, rssi %ld, uptime %lus)\n",
                  unsigned(info.wifi_sta_disconnected.reason),
                  unsigned(wifiPsMode.load()), long(WiFi.RSSI()),
                  (unsigned long)(millis() / 1000));
  }
}

void wifiTask(void*) {
  WiFi.persistent(false);   // never rewrite credentials into NVS
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // cached only; applyRadioPolicy() re-asserts it
  WiFi.setAutoReconnect(false); // one recovery owner; avoid competing reconnect loops
  WiFi.onEvent(onWifiEvent);
  if (strlen(DESK_WIFI_SSID)) WiFi.begin(DESK_WIFI_SSID, DESK_WIFI_PASS);
  startSntp();
  desk::WifiRecovery recovery;
  bool tuned = false, wasConnected = false, everConnected = false;
  uint32_t lastHttpRecovery = 0;
  for (;;) {
    const uint32_t now = millis();
    const bool wifi = WiFi.status() == WL_CONNECTED;
    if (wifi && !tuned) { applyRadioPolicy();tuned=true; }
    if (!wifi) tuned = false;
    if (wifi && !wasConnected) {
      if (everConnected) wifiReconnects.fetch_add(1);
      everConnected = true;
      startSntp();
    }
    wasConnected = wifi;
    const auto action = strlen(DESK_WIFI_SSID) ? recovery.update(wifi, now) : desk::WifiRecovery::NONE;
    if (action == desk::WifiRecovery::CONNECT) {
      Serial.println("WiFi reconnect requested (no association teardown)");
      esp_wifi_connect();
    } else if (action == desk::WifiRecovery::RESET_STATION ||
               action == desk::WifiRecovery::RESET_RADIO) {
      Serial.printf("WiFi recovery: %s reset\n", action == desk::WifiRecovery::RESET_RADIO ? "radio" : "station");
      webResetRequested.store(true);
      WiFi.disconnect(false, false);
      if (action == desk::WifiRecovery::RESET_RADIO) {
        WiFi.mode(WIFI_OFF);vTaskDelay(pdMS_TO_TICKS(200));WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);WiFi.setAutoReconnect(false);
      }
      WiFi.begin(DESK_WIFI_SSID, DESK_WIFI_PASS);
    } else if (action == desk::WifiRecovery::REBOOT) {
      Serial.println("WiFi recovery exhausted after 90s, restarting once");
      ESP.restart();
    }
    // A wedged HTTP task cannot prevent recovery. A slow OTA request gets a
    // longer budget; the supervisor never touches WebServer from this task.
    // /ota/stream holds netTask inside a long-lived upgrade handler, so
    // webLastBeat (only ticked by the netTask loop) freezes there even while
    // the OTA is progressing normally. otaLastActivity is refreshed on every
    // received frame, so treat the newer of the two as "HTTP is alive". That
    // keeps a healthy OTA from being mistaken for a wedged one; a genuinely
    // stuck OTA stops advancing otaLastActivity and is still caught after
    // the budget. Both counters return to normal once the OTA ends, so the
    // watchdog recovers its strict 30 s rule automatically.
    const uint32_t beat = webLastBeat.load();
    const uint32_t otaAct = otaLastActivity.load();
    const uint32_t alive = desk::httpAliveBeat(beat, otaAct);
    const uint32_t budget = (otaBusy.load() || otaAct) ? 120000u : 30000u;
    if (wifi && alive && now-alive > budget && (!lastHttpRecovery || now-lastHttpRecovery > 30000u)) {
      lastHttpRecovery = now;
      Serial.println("HTTP task stalled, resetting station sockets");
      webResetRequested.store(true);
      WiFi.disconnect(false, false); // next supervisor check starts recovery
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    updateHeartbeatLed(wifi);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void netTask(void*) {
  Update.onProgress([](size_t, size_t) {
    otaBusy.store(true); // also protect the legacy /update upload
    webLastBeat.store(millis());
  });
  if (strlen(DESK_API_TOKEN)) {
    // Basic header for /ota, computed once: the upload callback runs before
    // the request handler, so it cannot call authenticate() (that would send
    // a 401 mid-body).
    otaAuthExpected = String("Basic ") + base64::encode(String("admin:") + DESK_API_TOKEN);
  }
  for (;;) {
    const bool wifi = WiFi.status() == WL_CONNECTED;
    webLastBeat.store(millis());
    static bool webStarted = false;
    if (wifi && !webStarted) {
      webServer.on("/", handleRoot);
      webServer.on("/screen.bmp", handleScreenshot);
      webServer.on("/page", handleSetPage);
      webServer.on("/carousel", handleSetCarousel);
      webServer.on("/night", handleSetNight);
      webServer.on("/cal", handleCalibrate);
      webServer.on("/mood", handleSetMood);
      webServer.on("/lean", handleLean);
      webServer.on("/ink", handleSetInk);
      webServer.on("/brightness", handleSetBrightness);
      webServer.on("/api/status", handleStatusApi);
      if (strlen(DESK_API_TOKEN)) {
        httpUpdater.setup(&webServer, "/update", "admin", DESK_API_TOKEN);
        // Same credentials, chunked and resumable. Separate paths instead of
        // one path with a verb argument: the core parses a raw body before the
        // query string, so arguments are unavailable inside the upload
        // callback. /update stays for a plain one-shot push on a healthy link.
        // Custom headers are not collected unless asked for; collectHeaders
        // keeps Authorization (auth) and appends ours. Without this the total
        // arrives as an empty string and begin() rejects the push.
        static const char* otaHeaders[] = {"X-Ota-Total", "X-Ota-Offset"};
        webServer.collectHeaders(otaHeaders, 2);
        webServer.on("/ota/begin", HTTP_POST, handleOtaBegin);
        webServer.on("/ota/state", HTTP_GET, handleOtaState);
        webServer.on("/ota/chunk", HTTP_POST, handleOtaChunkDone, handleOtaBody);
        webServer.on("/ota/end", HTTP_POST, handleOtaEnd);
        webServer.on("/ota/stream", HTTP_GET, handleOtaStream);
        Serial.println("OTA ready: /update (whole file), /ota/* (resumable)");
      }
      webServer.begin();
      webStarted = true;
      Serial.printf("Web monitor running at http://%s/\n", WiFi.localIP().toString().c_str());
    }
    if (webStarted && webResetRequested.exchange(false)) {
      webServer.stop();
      if (wifi) webServer.begin();
      else webResetRequested.store(true);
    }
    if (webStarted && wifi) webServer.handleClient();
    otaBusy.store(false);
    const uint32_t activity=otaLastActivity.load();
    if (otaStorage && activity && millis()-activity>=15000u) otaFreeBuffer();
    webLastBeat.store(millis());
    if (otaRestartAt && int32_t(millis() - otaRestartAt) >= 0) {
      // The reply is already flushed; give the stack a beat, then boot the
      // slot Update just verified.
      Serial.println("OTA complete, restarting into the new image");
      vTaskDelay(pdMS_TO_TICKS(300));
      ESP.restart();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void fatal(const char* message) {
  Serial.println(message);
  for (;;) delay(1000);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Desk display starting (64x64 HUB75, USB diagnostics enabled)");
  initMpu();
  pinMode(HEARTBEAT_LED_PIN, OUTPUT);
  digitalWrite(HEARTBEAT_LED_PIN, LOW);
  if (DESK_BUTTON_ENABLED) pinMode(BTN_PIN, INPUT_PULLUP);
  controller.timezoneOffset = DESK_TIMEZONE_OFFSET;
  controller.workStartMinute = DESK_WORK_START_MINUTE;
  controller.workEndMinute = DESK_WORK_END_MINUTE;
  controller.workDays = DESK_WORK_DAYS;
  controller.tick(millis());
  HUB75_I2S_CFG cfg(64, 64, 1);
  // Row order for rear-cover mounting, with USB-C on the left in the silkscreen view.
  // OE and CLK spill to the USB-C end of the other row to avoid strapping pins.
  // Panel is natural RGB: R/G/B pins map straight through, no channel swap.
  cfg.gpio.r1 = 32; cfg.gpio.g1 = 23; cfg.gpio.b1 = 25;
  cfg.gpio.r2 = 26; cfg.gpio.g2 = 22; cfg.gpio.b2 = 27;
  cfg.gpio.a = 14; cfg.gpio.b = 19; cfg.gpio.c = 13;
  cfg.gpio.d = 18; cfg.gpio.e = 21;
  cfg.gpio.lat = 17; cfg.gpio.oe = 4; cfg.gpio.clk = 16;
  cfg.clkphase = false;
  cfg.i2sspeed = HUB75_I2S_CFG::HZ_20M;
  cfg.latch_blanking = 1;
  cfg.min_refresh_rate = 140;
  cfg.setPixelColorDepthBits(6);  // 25% fewer bitplane bytes than 8-bit
  cfg.double_buff = true;  // draw the next frame without clearing the visible one
  display = new (std::nothrow) MatrixPanel_I2S_DMA(cfg);
  if (!display || !display->begin()) fatal("HUB75 DMA allocation/init failed");
  Serial.printf("[HUB75] DMA initialized: %d Hz refresh rate (i2s=20MHz, depth=6bit, lat_blank=1)\n", display->calculated_refresh_rate);
  display->setRotation(1);  // 90 degrees clockwise on the mounted panel.
  display->setTextWrap(false);
  display->clearScreen();
  // begin() drives the panel at full power until the first frame lands. Latch
  // the compiled day level (60) immediately so a cold boot never flashes at
  // 255 for the ~100 ms before drawCurrentPage() runs. desk::DeskTheme already
  // starts at DESK_THEME_BRIGHTNESS, so this is also the answer to "开机多亮".
  display->setBrightness8(uint8_t(theme.brightness));
  panelBrightness.store(uint8_t(theme.brightness));
  Serial.printf("Brightness: boot %u, sleep window %u (compiled; runtime only)\n",
                unsigned(theme.brightness), unsigned(NIGHT_BRIGHTNESS));
  snapshots = xQueueCreate(1, sizeof(Snapshot));
  brightnessCommands = xQueueCreate(1, sizeof(BrightnessCommand));
  // Draw locally before starting any networking, including first boot offline.
  rotateClockInk();   // fresh ink each boot, never the same hue twice in a row
  drawCurrentPage(); display->flipDMABuffer();
  if (xTaskCreatePinnedToCore(wifiTask, "wifi-recovery", 4096, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println("No WiFi recovery task memory; local display remains available");
  if (xTaskCreatePinnedToCore(netTask, "web", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("No web task memory; local display remains available");
  }
  if (!snapshots || xTaskCreatePinnedToCore(snapshotTask, "mac-poll", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    if (snapshots) vQueueDelete(snapshots);
    snapshots = nullptr;
    Serial.println("No Mac poll task memory; local display remains available");
  }
  Serial.printf("Free heap: %u; largest DMA block: %u\n", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

void loop() {
  uint32_t now = millis();
  updateMpu(now);
  desk::Mode oldMode = controller.mode;
  uint8_t oldPage = controller.page;
  desk::Source oldSource = controller.source;
  bool oldOnline = controller.serviceOnline;
  bool oldLocked = controller.macLocked;
  controller.tick(now);
  static uint32_t lastClockCheck = 0;
  if (now - lastClockCheck >= 1000) {
    time_t wall = time(nullptr);
    if (wall >= 1700000000LL) controller.syncClock(wall);
    lastClockCheck = now;
  }
  Snapshot incoming;
  static BrightnessCommand lastDirect{};
  static uint64_t lastMacBrightnessRevision = 0;
  static uint64_t lastMacResumeRevision = 0;
  BrightnessCommand direct;
  if (brightnessCommands && xQueueReceive(brightnessCommands, &direct, 0) == pdTRUE) {
    lastDirect = direct;
    dirty |= theme.brightness != direct.value;
    theme.brightness = direct.value;
  }
  if (snapshots && xQueueReceive(snapshots, &incoming, 0) == pdTRUE) {
    panelMacHttp.store(incoming.httpCode);
    dirty |= data.wifi != incoming.wifi || data.httpCode != incoming.httpCode || strcmp(data.ip, incoming.ip) != 0;
    if (incoming.valid) {
      if (incoming.epoch >= 1700000000LL && incoming.epoch < 4102444800LL) {
        // Soft sync: only re-stamp the system clock when epoch actually jumped.
        if (controller.syncClock(incoming.epoch)) {
          struct timeval tv = {time_t(incoming.epoch), 0}; settimeofday(&tv, nullptr);
        }
      }
      if (incoming.macLocked >= 0) {
        controller.setMacLocked(incoming.macLocked != 0);
        panelMacLocked.store(controller.macLocked);
      }
      controller.contact();
      controller.remote(incoming.automatic, desk::AUTOMATIC, incoming.requestStarted);
      controller.remote(incoming.presence, desk::MAC_PRESENCE, incoming.requestStarted);
      controller.remote(incoming.manual, desk::MANUAL, incoming.requestStarted);
      if (incoming.resumeRevision && incoming.resumeRevision!=lastMacResumeRevision) {
        lastMacResumeRevision=incoming.resumeRevision;
        const uint32_t localAt=lastWebControlAt.load();
        if (incoming.resumeAgeMs<30000u && (!localAt || millis()-localAt>=incoming.resumeAgeMs) &&
            (!incoming.manual.id[0] || incoming.manual.ageMs>=incoming.resumeAgeMs)) resumeRequested.store(true);
      }
      dirty |= quota5h != incoming.quota5h || quotaWeek != incoming.quotaWeek ||
               quotaAt != incoming.quotaAt || quotaStale != incoming.quotaStale || strcmp(data.message, incoming.message) != 0;
      quota5h = incoming.quota5h; quotaWeek = incoming.quotaWeek;
      quotaAt = incoming.quotaAt; quotaStale = incoming.quotaStale;
      if (incoming.quota5h >= 0 && incoming.quota5h != carousel.lastQuota) {
        carousel.lastQuota = incoming.quota5h;
        carousel.lastQuotaChange = now;
      }
      dirty |= data.reset5hAt != incoming.reset5hAt;
      // Brightness from the Mac is a fallback, not an authority. deskctl and
      // the console's "direct link timed out" path both land here, and the
      // snapshot repeats the value forever until someone changes it. So it
      // only counts while it is FRESH (30 s): a stale repeat must never decide
      // what a boot looks like, otherwise switching the panel off and on
      // replays whatever level was last dragged — looks exactly like
      // "开机不是 60". Fresh means someone asked just now; honour it.
      if (incoming.brightness >= 8 && incoming.brightnessRevision != lastMacBrightnessRevision) {
        lastMacBrightnessRevision = incoming.brightnessRevision;
        if (incoming.brightnessAgeMs < 30000u && theme.brightness != incoming.brightness) {
          theme.brightness = incoming.brightness;
          dirty = true;
        }
      }
      if (strcmp(data.message, incoming.message) != 0 || controller.uptime() >= messageUntil) messageStarted = controller.uptime();
      messageUntil = controller.uptime() + incoming.messageMs;
      data = incoming;
    } else {
      data.wifi = incoming.wifi; data.httpCode = incoming.httpCode;
      strlcpy(data.ip, incoming.ip, sizeof(data.ip));
    }
  }
  if (DESK_BUTTON_ENABLED) {
    int action = button.update(digitalRead(BTN_PIN) == LOW, now);
    if (action >= 0) { controller.button(action); dirty = true; }
  }
  if (resumeRequested.exchange(false) && !controller.macLocked) {
    controller.resumeCarousel();
    carousel.enabled = true;
    carousel.manualPage = -1;
    carousel.manualUntil = 0;
    carousel.lastSwitch = now;
    dirty = true;
  }
  if (oldLocked != controller.macLocked) {
    carousel.enabled = true;
    carousel.manualPage = -1;
    carousel.manualUntil = 0;
  }
  if (oldLocked != controller.macLocked || oldMode != controller.mode) {
    carousel.currentSlot = controller.mode == desk::WORK ? desk::QUOTA_PAGE : desk::LOFT_PAGE;
    carousel.lastSwitch = now;
    dirty = true;
  }
  updateCarousel(now);
  panelMacOnline.store(controller.serviceOnline);
  if (oldMode != controller.mode || oldPage != controller.page || oldSource != controller.source || oldOnline != controller.serviceOnline) dirty = true;

  bool showingMessage = controller.uptime() < messageUntil && data.message[0];
  static bool wasShowingMessage = false;
  static int64_t lastMinute = -1, lastQuotaAge = -1;
  int64_t minute = controller.epoch() / 60;
  int64_t quotaAge = quotaAt ? (controller.epoch() - quotaAt) / 60 : -1;
  dirty |= showingMessage != wasShowingMessage || minute != lastMinute || quotaAge != lastQuotaAge;
  wasShowingMessage = showingMessage; lastMinute = minute; lastQuotaAge = quotaAge;
  static uint32_t lastReport = 0;
  if (dirty || now - lastReport >= 30000) {
    reportState();
    lastReport = now;
  }
  // A live reaction face needs a faster clock than a page: 20 fps is the
  // difference between a marshmallow that breathes and a slideshow.
  const uint8_t mood = activeGestureMood(now);
  uint32_t cadence = mood != desk::GESTURE_OFF ? 50 : 100;  // one scene; never all pages
  if (dirty || now - lastDraw >= cadence) {
    drawCurrentPage();
    display->flipDMABuffer();
    dirty = false; lastDraw = now;
  }
  delay(5);
}
