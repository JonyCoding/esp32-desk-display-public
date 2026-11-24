#pragma once
#include <algorithm>
#include <Adafruit_GFX.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "display_controller.h"
#include "display_theme.h"
#include "state_protocol.h"

// Peek entrance: 0 = rises from the bottom edge, 1 = slides in from the right,
// 2 = both at once, 3 = alternate between bottom and right every cycle.
#ifndef DESK_PEEK_STYLE
#define DESK_PEEK_STYLE 3
#endif

namespace desk {
struct Theme {
  int brightness = DESK_THEME_BRIGHTNESS;
  uint32_t nightColor = DESK_THEME_NIGHT_COLOR;
  uint32_t accentColor = DESK_THEME_ACCENT_COLOR;
  uint32_t clockColor = DESK_THEME_CLOCK_COLOR;
  uint32_t quotaColor = DESK_THEME_QUOTA_COLOR;
  int animationStepMs = DESK_THEME_ANIMATION_STEP_MS;
  int sceneDetail = DESK_THEME_SCENE_DETAIL;
};
struct ViewState {
  bool wifi = false;
  int httpCode = 0;
  char ip[16] = {};
  // Link quality for the SYSTEM page (-54 dBm). 0 = not measured yet, which
  // the page renders as a bare "OK" instead of a fake number.
  int8_t rssi = 0;
  // Free heap in KB — the one system number worth a row besides uptime.
  uint16_t heapKb = 0;
  // Where the wall clock came from: 0 = unsynced, 1 = SNTP (device's own
  // network time), 2 = Mac snapshot. The page prints NTP / MAC / --.
  uint8_t clockSrc = 0;
  // Local preview selection only. Firmware leaves this at -1 (visit cycle).
  int8_t loftScenePreview = -1;
  // MPU6050 slot. The SYSTEM page always draws these two rows, so soldering the
  // module in later fills them with live data and needs no re-layout.
  bool mpuPresent = false;      // answered on I2C at 0x68
  uint8_t tiltDeg = 0;          // degrees away from the calibrated mount pose
  uint8_t mpuPose = 0;          // 0 absent, 1 calibrating, 2 level, 3 tilted,
                                // 4 flipped, 5 shaken
  int quota5h = -1, quotaWeek = -1;
  int64_t quotaAt = 0, reset5hAt = 0;
  bool quotaStale = true;
  char message[161] = {};
  uint64_t messageUntil = 0, messageStarted = 0;
  bool otaInProgress = false;
  uint8_t otaProgressPct = 0;
  StockState stock;
  // Controller uptime (ms) at which the currently shown page appeared. The
  // stock page derives its 6s SH/NASDAQ phases from this, so entering the
  // page always starts with SH instead of a random boot-uptime phase.
  uint32_t pageShownAt = 0;
};
// Pages are absolute: controller.page indexes this enum directly, and the Mac
// service sends the same numbering (loft 0, quota 1, status 2, stock 3).
enum Page : uint8_t { LOFT_PAGE, QUOTA_PAGE, STATUS_PAGE, STOCK_PAGE };
constexpr uint32_t FUN_LOFT_MS = 30000;
constexpr uint32_t STOCK_PAGE_MS = 12000;
inline Page nextCarouselPage(Page page, Mode mode) {
  if (page == STOCK_PAGE) return LOFT_PAGE;
  if (mode == WORK && page == LOFT_PAGE) return QUOTA_PAGE;
  return STOCK_PAGE;
}
// The exact reverse of the chain above, so tilting back really goes back:
// WORK walks LOFT→QUOTA→STOCK→LOFT, FUN walks LOFT→STOCK→LOFT.
inline Page previousCarouselPage(Page page, Mode mode) {
  if (page == LOFT_PAGE) return STOCK_PAGE;
  if (page == QUOTA_PAGE) return LOFT_PAGE;
  return mode == WORK ? QUOTA_PAGE : LOFT_PAGE;
}
inline Page currentPage(const Controller& controller, const ViewState& data) {
  (void)data;
  switch (controller.page) {
    case 3: return STOCK_PAGE;
    case 2: return STATUS_PAGE;
    case 1: return QUOTA_PAGE;
    default: return LOFT_PAGE;
  }
}
// Sleep window (22:30–06:00 Shanghai wall clock): keep ONLY the red channel.
// Not a recolor — green/blue pixels go dark, so the artwork thins out exactly
// like the hardware does when USB power drops and only the red LEDs still
// light (the look the user wants for sleeping). Set once per frame at the top
// of Renderer::draw(); stays off while the clock is unsynced.
inline bool& redOnlyNight() { static bool on = false; return on; }
// Web-console override for the sleep window: 0=auto schedule, 1=force
// red-only now, 2=force full color now. Renderer::draw() resolves it every
// frame, so the 22:30–06:00 schedule resumes as soon as this returns to 0.
inline int& nightOverride() { static int mode = 0; return mode; }
// Runtime ink for the loft hero clock: 0 means "use the compiled theme
// colour". The sketch rotates this through a curated palette every time the
// clock page comes back, so the time changes hue without a reflash.
struct LoftCycle { uint32_t seenAt=0; uint8_t scene=0; };
inline LoftCycle& loftCycle() { static LoftCycle value; return value; }
inline uint8_t& loftSceneIndex() { return loftCycle().scene; }
inline uint32_t& loftClockInk() { static uint32_t ink = 0; return ink; }
// Pinned ink: set from the console (/ink?set=rrggbb) so the palette rotation
// stops overriding a hand-picked hue. Cleared by /ink?set=auto.
inline bool& clockInkPinned() { static bool on = false; return on; }
// Curated hero-clock inks, shared by firmware and the Mac preview. Every entry
// stays readable on the near-black loft page and plays nicely with the amber
// window lamp. 0x4a659c is the dusty blue the user liked.
static const uint32_t CLOCK_INKS[] = {
  0xffffff,  // pure white
  0x4a659c,  // dusty blue (user favourite)
  0x9ad0ff,  // ice blue
  0xffd28a,  // warm amber
  0xa8e6c8,  // mint
  0xffa8c8,  // rose
  0xc9b8ff,  // lavender
  0xff9a6b,  // coral
  0x8fe3ff,  // cyan
  0xffe066,  // gold
};
// Pick a palette entry from caller-supplied randomness, never repeating the
// previous hue back to back.
inline uint32_t pickClockInk(uint32_t rnd) {
  const int count = int(sizeof(CLOCK_INKS) / sizeof(CLOCK_INKS[0]));
  static int last = count;
  int pick = int(rnd % uint32_t(count));
  if (count > 1 && pick == last) pick = (pick + 1) % count;
  last = pick;
  return CLOCK_INKS[pick];
}
// Sleep-window ceiling: the only red channel tops out at 26/31 (~84 %) so a
// full-white clock does not flood a dark room even at backlight 20. Day
// rendering is untouched — this clamp lives inside the night branch only.
constexpr uint32_t NIGHT_RED_CEIL = 26;
inline uint16_t rgb565(int r, int g, int b) {
  // FINAL VERDICT (2026-11-20, root cause found and fixed): the panel is
  // natural RGB. The historic hue corruption was NOT a channel issue — the
  // HUB75 library picked its 8-bit CIE brightness LUT from a compile-time
  // macro while the sketch ran 6-bit DMA planes, so LUT outputs >63 were
  // truncated (mod-64) and mid-range colors collapsed. Fixed by defining
  // PIXEL_COLOR_DEPTH_BITS=6 in tools/firmware.sh. Pack plain RGB565 here,
  // byte-identical to the Mac preview. Do NOT add any channel swap.
  const uint16_t packed = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
  if (!redOnlyNight()) return packed;
  // Sleep window: keep ONLY red, but do not throw the other channels away.
  // Dropping them turned every dim green/blue shape (sky, moon glow, stars,
  // the sea on the globe) pitch black, which is why the night picture was
  // unreadable. Fold them into one red level instead.
  // "Level" is the pixel's strongest channel, not photometric luma: scotopic
  // luma weights blue at 29/256, so a deep-blue sky would still go black.
  // 75 % peak + 25 % luma keeps every hue visible while preserving shading
  // inside a hue family, then the ceiling caps the top end.
  const uint32_t r5 = (packed >> 11) & 0x1f;
  const uint32_t g5 = uint32_t(((packed >> 5) & 0x3f) * 31 / 63);
  const uint32_t b5 = packed & 0x1f;
  const uint32_t peak = std::max(r5, std::max(g5, b5));
  const uint32_t luma = (77u * r5 + 150u * g5 + 29u * b5) / 256u;
  uint32_t level = (peak * 3u + luma) / 4u;
  if (level > NIGHT_RED_CEIL) level = NIGHT_RED_CEIL;
  if (!level && (r5 | g5 | b5)) level = 1;  // 1px hairlines never disappear
  return uint16_t(level << 11);
}
inline uint16_t color(uint32_t hex) { return rgb565((hex >> 16) & 255, (hex >> 8) & 255, hex & 255); }
// Original, purpose-drawn 5x9 numeral masks; same data on MCU and preview.
static constexpr uint8_t DIGITS[10][9] = {
 {14,27,27,27,27,27,27,27,14},{6,14,6,6,6,6,6,6,15},
 {14,27,3,3,6,12,24,24,31},{30,3,3,3,14,3,3,3,30},
 {3,7,15,27,19,31,3,3,3},{31,24,24,24,30,3,3,27,14},
 {14,24,24,24,30,27,27,27,14},{31,3,3,6,6,12,12,12,12},
 {14,27,27,27,14,27,27,27,14},{14,27,27,27,15,3,3,3,14}
};
// 4x7 clean digits font for medium desk clock
static constexpr uint8_t DIGITS_4x7[10][7] = {
  {0b1110, 0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b1110}, // 0
  {0b0100, 0b1100, 0b0100, 0b0100, 0b0100, 0b0100, 0b1110}, // 1
  {0b1110, 0b0001, 0b0001, 0b1110, 0b1000, 0b1000, 0b1111}, // 2
  {0b1110, 0b0001, 0b0001, 0b0110, 0b0001, 0b0001, 0b1110}, // 3
  {0b1001, 0b1001, 0b1001, 0b1111, 0b0001, 0b0001, 0b0001}, // 4
  {0b1111, 0b1000, 0b1000, 0b1110, 0b0001, 0b0001, 0b1110}, // 5
  {0b0110, 0b1000, 0b1000, 0b1110, 0b1001, 0b1001, 0b0110}, // 6
  {0b1111, 0b0001, 0b0010, 0b0010, 0b0100, 0b0100, 0b0100}, // 7
  {0b0110, 0b1001, 0b1001, 0b0110, 0b1001, 0b1001, 0b0110}, // 8
  {0b0110, 0b1001, 0b1001, 0b0111, 0b0001, 0b0001, 0b0110}, // 9
};
// Compact 3x5 labels leave the expensive pixels to the main number and scene.
inline uint16_t microGlyph(char c) {
  switch (c) {
    case '0':return 0b111101101101111;case '1':return 0b010110010010111;
    case '2':return 0b110001111100111;case '3':return 0b110001110001110;
    case '4':return 0b101101111001001;case '5':return 0b111100110001110;
    case '6':return 0b011100111101111;case '7':return 0b111001010010010;
    case '8':return 0b111101111101111;case '9':return 0b111101111001110;
    case 'A':return 0b010101111101101;case 'B':return 0b110101110101110;
    case 'C':return 0b011100100100011;case 'D':return 0b110101101101110;
    case 'E':return 0b111100110100111;case 'F':return 0b111100110100100;
    case 'G':return 0b011100101101011;case 'H':return 0b101101111101101;
    case 'I':return 0b111010010010111;case 'J':return 0b001001001101010;
    case 'K':return 0b101101110101101;case 'L':return 0b100100100100111;
    case 'M':return 0b101111111101101;case 'N':return 0b101111111111101;
    case 'O':return 0b010101101101010;case 'P':return 0b110101110100100;
    case 'Q':return 0b010101101111011;case 'R':return 0b110101110101101;
    case 'S':return 0b011100010001110;case 'T':return 0b111010010010010;
    case 'U':return 0b101101101101111;case 'V':return 0b101101101101010;
    case 'W':return 0b101101101111010;case 'X':return 0b101101010101101;
    case 'Y':return 0b101101010010010;case 'Z':return 0b111001010100111;
    case ':':return 0b000010000010000;case '.':return 0b000000000000010;
    case '/':return 0b001001010100100;case '-':return 0b000000111000000;
    case '%':return 0b101001010100101;case '?':return 0b110001010000010;
    case ' ':return 0;case '!':return 0b010010010000010;
    case '+':return 0b000010111010000;case '=':return 0b000111000111000;
    default:return 0b110001010000010;
  }
}

// 12x12 Chinese characters: 0=上, 1=证, 2=指, 3=数, 4=纳, 5=斯, 6=达, 7=克
static constexpr uint16_t CHINESE_12x12[8][12] = {
  {0x040, 0x040, 0x040, 0x07c, 0x040, 0x040, 0x040, 0x040, 0x7fe, 0x000, 0x000, 0x000}, // 上
  {0x2fc, 0x010, 0x650, 0x250, 0x25c, 0x250, 0x250, 0x350, 0x350, 0x0fe, 0x000, 0x000}, // 证
  {0x278, 0x7c4, 0x244, 0x27c, 0x3fc, 0x644, 0x27c, 0x244, 0x27c, 0x644, 0x000, 0x000}, // 指
  {0x590, 0x7fe, 0x324, 0x5e4, 0x068, 0x798, 0x298, 0x398, 0x3a4, 0x442, 0x000, 0x000}, // 数
  {0x210, 0x47c, 0x594, 0x694, 0x2bc, 0x5ac, 0x6ec, 0x084, 0x784, 0x08c, 0x000, 0x000}, // 纳
  {0x7fc, 0x4a0, 0x7a0, 0x4be, 0x7ac, 0x4ac, 0x7ec, 0x02c, 0x4ec, 0x44c, 0x000, 0x000}, // 斯
  {0x620, 0x2fc, 0x020, 0x030, 0x628, 0x248, 0x2c4, 0x380, 0x700, 0x0fe, 0x000, 0x000}, // 达
  {0x7fc, 0x040, 0x3f8, 0x208, 0x208, 0x3f8, 0x0a0, 0x120, 0x324, 0x63c, 0x000, 0x000}, // 克
};

// ---------------------------------------------------------------------------
// Reaction captions. The lines ("要滑了", "快停下", "我要掉啦" ...) and their
// 12x12 masks started life in the /gravity.html prototype; they live here now
// so the panel and the Mac preview speak with the same voice. Rows are 12-bit
// masks with bit 11 = leftmost pixel, and only the characters the five faces
// actually say are compiled in (24 bytes each).
enum CnGlyph : uint8_t {
  CN_YAO, CN_HUA, CN_LE, CN_FU, CN_HAO, CN_WO, CN_KUAI, CN_TING, CN_XIA,
  CN_BIE, CN_HUANG, CN_LA, CN_FANG, CN_LAI, CN_DIAO, CN_PENG, CN_YOU, CN_QIAO,
  CN_YUN, CN_TIAN, CN_ZAI, CN_ZHUAN, CN_JIE, CN_ZHU, CN_XIAN, CN_COUNT
};
static constexpr uint16_t CN_MASKS[CN_COUNT][12] = {
  {0x000, 0x7fe, 0x090, 0x3fc, 0x294, 0x3fc, 0x040, 0x0f8, 0x1f0, 0x078, 0x78c, 0x000}, // 要
  {0x000, 0x27c, 0x35c, 0x054, 0x7fe, 0x102, 0x1fe, 0x27c, 0x27c, 0x444, 0x44c, 0x000}, // 滑
  {0x000, 0x7fe, 0x00c, 0x030, 0x060, 0x060, 0x020, 0x020, 0x020, 0x060, 0x0e0, 0x000}, // 了
  {0x210, 0x210, 0x27e, 0x790, 0x210, 0x37e, 0x610, 0x238, 0x22c, 0x646, 0x682, 0x000}, // 扶
  {0x300, 0x33e, 0x204, 0x7c8, 0x288, 0x2ff, 0x688, 0x388, 0x188, 0x618, 0x418, 0x000}, // 好
  {0x030, 0x1fc, 0x734, 0x130, 0x7fe, 0x134, 0x1d4, 0x798, 0x118, 0x13a, 0x34e, 0x000}, // 我
  {0x000, 0x310, 0x310, 0x37e, 0x794, 0x794, 0x714, 0x37f, 0x330, 0x328, 0x366, 0x382}, // 快
  {0x310, 0x2fe, 0x200, 0x67c, 0x644, 0x67c, 0x2ff, 0x2c4, 0x27c, 0x210, 0x230, 0x000}, // 停
  {0x000, 0x7fe, 0x040, 0x040, 0x050, 0x058, 0x04c, 0x044, 0x040, 0x040, 0x040, 0x000}, // 下
  {0x000, 0x3e2, 0x26a, 0x26a, 0x3ea, 0x18a, 0x7ea, 0x12a, 0x12a, 0x666, 0x446, 0x000}, // 别
  {0x000, 0x3fc, 0x3fc, 0x204, 0x3fc, 0x168, 0x168, 0x7fe, 0x090, 0x192, 0x61e, 0x000}, // 晃
  {0x048, 0x048, 0x75e, 0x5e0, 0x546, 0x574, 0x5d4, 0x554, 0x75c, 0x544, 0x4fe, 0x000}, // 啦
  {0x110, 0x110, 0x7fe, 0x224, 0x264, 0x3f4, 0x2d4, 0x298, 0x698, 0x5be, 0x5a2, 0x000}, // 放
  {0x000, 0x060, 0x020, 0x7fe, 0x02c, 0x128, 0x020, 0x7fe, 0x0f0, 0x12c, 0x626, 0x060}, // 来
  {0x210, 0x21e, 0x210, 0x7fe, 0x246, 0x37e, 0x77e, 0x210, 0x2ff, 0x610, 0x610, 0x000}, // 掉
  {0x024, 0x028, 0x7fe, 0x218, 0x25a, 0x7da, 0x6fe, 0x2fe, 0x398, 0x218, 0x27f, 0x000}, // 碰
  {0x000, 0x3fc, 0x108, 0x188, 0x098, 0x0d0, 0x070, 0x060, 0x0f8, 0x38e, 0x606, 0x000}, // 又
  {0x108, 0x108, 0x7ee, 0x7c8, 0x7fe, 0x034, 0x7d4, 0x7dc, 0x6c8, 0x5de, 0x432, 0x400}, // 敲
  {0x000, 0x3fc, 0x3fc, 0x3fc, 0x7fe, 0x442, 0x3fc, 0x1fc, 0x020, 0x7fe, 0x020, 0x000}, // 晕
  {0x000, 0x3fc, 0x060, 0x060, 0x060, 0x7fe, 0x060, 0x0d0, 0x098, 0x30e, 0x602, 0x000}, // 天
  {0x060, 0x040, 0x3fe, 0x080, 0x190, 0x310, 0x77e, 0x510, 0x110, 0x110, 0x1fe, 0x000}, // 在
  {0x310, 0x210, 0x7fe, 0x210, 0x57e, 0x7b0, 0x130, 0x13e, 0x784, 0x538, 0x118, 0x104}, // 转
  {0x000, 0x218, 0x27e, 0x220, 0x788, 0x310, 0x6ff, 0x22c, 0x238, 0x23c, 0x6c6, 0x000}, // 接
  {0x110, 0x110, 0x2fe, 0x210, 0x610, 0x610, 0x2fe, 0x210, 0x210, 0x210, 0x2fe, 0x000}, // 住
  {0x030, 0x730, 0x568, 0x746, 0x6fa, 0x704, 0x57c, 0x558, 0x748, 0x4fe, 0x400, 0x000}, // 险
};
// Two lines per mood. The face alternates every 2.2 s while the gesture lasts,
// so a panel held upside down keeps complaining instead of freezing on one
// sentence. CN_DONE ends a shorter line.
static constexpr uint8_t CN_DONE = 0xff;
static constexpr uint8_t CN_LINES[8][2][4] = {
  {{CN_DONE, CN_DONE, CN_DONE, CN_DONE}, {CN_DONE, CN_DONE, CN_DONE, CN_DONE}},
  {{CN_YAO, CN_HUA, CN_LE, CN_DONE},     {CN_FU, CN_HAO, CN_WO, CN_DONE}},      // 倾斜
  {{CN_KUAI, CN_TING, CN_XIA, CN_DONE},  {CN_BIE, CN_HUANG, CN_LA, CN_DONE}},   // 摇晃
  {{CN_FANG, CN_WO, CN_XIA, CN_LAI},     {CN_WO, CN_YAO, CN_DIAO, CN_LA}},      // 倒转
  {{CN_BIE, CN_PENG, CN_WO, CN_DONE},    {CN_YOU, CN_QIAO, CN_WO, CN_DONE}},    // 轻敲
  {{CN_WO, CN_YUN, CN_LE, CN_DONE},      {CN_TIAN, CN_ZAI, CN_ZHUAN, CN_DONE}}, // 眩晕
  {{CN_JIE, CN_ZHU, CN_LA, CN_DONE},     {CN_HAO, CN_XIAN, CN_DONE, CN_DONE}},  // 扶正
  {{CN_FANG, CN_WO, CN_XIA, CN_LAI},     {CN_WO, CN_YAO, CN_DIAO, CN_LA}},      // 拿起来
};

// The reactions, including picked up in hands and set back down.
enum GestureMood : uint8_t {
  GESTURE_OFF, GESTURE_LEAN, GESTURE_SHAKE, GESTURE_FLIP, GESTURE_TAP, GESTURE_DIZZY, GESTURE_SETTLE, GESTURE_HELD
};
// One frame of reaction state. `now` is a free-running millisecond clock (wobble,
// orbit, blink); `since` counts from the moment this mood took over, so each
// face can play its own entrance — the tap jumps and settles, the flip slips
// and is caught. `leanX/leanY` are the gravity vector resolved onto the
// mounted plane, -100..100 (see the sketch's gravity projection).
struct Gesture {
  uint8_t mood = GESTURE_OFF;
  uint32_t now = 0;
  uint32_t since = 0;
  int8_t leanX = 0;
  int8_t leanY = 0;
};

template <class Surface> class Renderer {
 public:
  Renderer(Surface& surface, const Controller& state, const ViewState& data, const Theme& settings)
      : d(surface), c(state), v(data), t(settings) {}
  // Sleep window gate for redOnlyNight(): 22:30–06:00, red channel only.
  // tm.valid is REQUIRED: an unsynced clock reads 00:00, and 0 < 360 would
  // light the pre-dawn branch — that made the whole boot screen run red until
  // the first clock sync (2026-11-21). Manual override wins: 1 forces the red
  // channel, 2 forces full color, 0 follows schedule. Every painter calls this
  // first, so no screen can slip out in full colour after 22:30.
  void latchNight() {
    const auto tm = local();
    const int mins = tm.hour * 60 + tm.minute;
    const bool scheduled = tm.valid && (mins >= 22 * 60 + 30 || mins < 6 * 60);
    redOnlyNight() = nightOverride() == 1 || (nightOverride() == 0 && scheduled);
  }
  void draw(Page page, uint16_t* pixels=nullptr) {
    latchNight();
    d.setTextWrap(false); d.fillScreen(0);
    if (v.otaInProgress) {
      drawOtaScreen();
      return;
    }
#ifdef DESK_COLOR_TEST
    // Brightness cross-check: the three "cursed" mid-range colors rendered
    // catastrophically wrong at brightness 98, while saturated primaries were
    // certified correct at brightness 25. Re-render the SAME four rows at the
    // SAME low brightness to isolate which variable drives the corruption.
    // Rows: gray-blue / mint / orange / green control; left full value,
    // right low-2-bits cleared (they rendered identically, kept for symmetry).
    struct Duo { uint32_t full; const char* name; };
    const Duo rows[] = {
        {0x8799ad, "gray-blue"}, {0x6ee7b7, "mint"}, {0xffbe76, "orange"},
        {0x00ff00, "green-ctl"},
    };
    for (int r = 0; r < 4; r++) {
      const uint32_t hex = rows[r].full;
      const int y0 = r * 16;
      d.fillRect(0,  y0, 32, 16, color(hex));
      d.fillRect(32, y0, 32, 16,
                 color(((hex >> 16) & 0xfc) << 16 | ((hex >> 8) & 0xfc) << 8 | (hex & 0xfc)));
    }
    return;
#endif
    switch (page) {
      case LOFT_PAGE: drawLoft(); break;
      case QUOTA_PAGE: drawQuota(); break;
      case STATUS_PAGE: drawStatus(); break;   // no globe: MAC row shows link
      case STOCK_PAGE: drawStock(); break;
    }
    if (page != STATUS_PAGE) drawLinkLight();
    drawMessage();
    // The SYSTEM page is pure text on black: stars and meteors would land in
    // the 3 px gaps between rows and read as dirt on the panel.
    const int scene = v.loftScenePreview >= 0 ? v.loftScenePreview : loftSceneIndex();
    if (pixels && page != STATUS_PAGE && (page != LOFT_PAGE || scene%2==0)) drawSky(page,pixels);
  }
 private:
  Surface& d; const Controller& c; const ViewState& v; const Theme& t;
  uint32_t frame() const { return c.uptime() / std::max(t.animationStepMs, 40); }
  // Top-right link light, three levels:
  //   no Wi-Fi at all   -> dim red broken-link glyph (device is offline)
  //   Wi-Fi but no Mac  -> five teal dots in a cross, frozen (network, no data)
  //   Wi-Fi + Mac data  -> same cross, pulsing outward AND cycling colour
  //                        through a muted palette (fully live):
  //                        horizontal dots step out 1px and back, then the
  //                        vertical dots step out 1px and back, alternating;
  //                        hue drifts slowly so colour and position never
  //                        tick in lockstep.
  // The glyph is on 24/7, so it is dimmed to a safe level (no full-brightness
  // pixel). It stays fixed at center x=61,y=2: one up and one right of the
  // former 60,3 anchor. No wander — the dimmed level does not need it.
  // Scale a packed RGB565 colour down while keeping its hue.
  static uint16_t dim565(uint16_t rgb, int keep) {
    return uint16_t(((((rgb >> 11) & 31) * keep / 256) << 11) |
                    ((((rgb >> 5) & 63) * keep / 256) << 5) |
                    (((rgb & 31) * keep / 256)));
  }

  // 64x64 全屏 OTA 升级页面：萌宠吃豆兽大胃王 + 右侧专属大数字百分比
  void drawOtaScreen() {
    const uint32_t up = c.uptime();
    const int pct = std::clamp<int>(v.otaProgressPct, 0, 100);

    const uint16_t cyan = color(0x5fe0c4u);
    const uint16_t amber = color(0xffbe38u);
    const uint16_t milk = color(0xf5fffau);
    const uint16_t mint = color(0x6ee7b7u);
    const uint16_t mintD = color(0x50be92u);
    const uint16_t dark = color(0x10221cu);
    const uint16_t pink = color(0xffa8b8u);

    // 1. 顶部状态栏 (y=2..9)：左侧干净的 OTA FLASHING，右侧心跳指示灯
    micro(3, 3, "OTA FLASHING", cyan);
    d.drawPixel(60, 4, ((up / 140) % 2) ? amber : color(0x182c42u));
    d.drawFastHLine(3, 10, 58, color(0x182c42u));

    // 2. 传送带导轨 (下移至 y=28..29，支撑滑向嘴巴中心 y=25)
    d.drawFastHLine(28, 28, 34, color(0x2d4b69u));
    d.drawFastHLine(28, 29, 34, color(0x192d41u));
    // 滚轮支架 (y=30..32)
    for (int x = 33; x <= 58; x += 8) {
      d.drawFastVLine(x, 30, 3, color(0x3c648cu));
    }

    // 3. 传送带上的三颗发光彩色芯片 (y=22..27，中心 y=24.5~25，100% 严格对齐嘴尖吞入中心！)
    const int shiftX = int((up / 80) % 10);
    // 芯片 1：金色核心代码块 (滑入嘴前，中心正对嘴巴中心 y=25)
    const int c1x = 39 - shiftX;
    if (c1x >= 27) {
      d.drawRect(c1x, 22, 6, 6, color(0xffe57fu));
      d.fillRect(c1x + 1, 23, 4, 4, amber);
      d.drawPixel(c1x + 2, 24, milk);
    }
    // 芯片 2：青蓝 ROM 模块
    const int c2x = 50 - shiftX;
    if (c2x >= 28 && c2x <= 56) {
      d.drawRect(c2x, 23, 6, 5, color(0x78d2ffu));
      d.fillRect(c2x + 1, 24, 4, 3, color(0x3ca0f0u));
      d.drawPixel(c2x + 2, 25, milk);
    }
    // 芯片 3：薄荷绿逻辑芯片
    const int c3x = 61 - shiftX;
    if (c3x >= 28 && c3x <= 58) {
      d.drawRect(c3x, 23, 5, 5, color(0x8cffbeu));
      d.fillRect(c3x + 1, 24, 3, 3, color(0x46c88cu));
      d.drawPixel(c3x + 2, 24, milk);
    }

    // 4. 左侧萌宠吃豆兽 (x=4..26, y=14..45)
    // 萌宠轻微浮动踏步 (0..1px)
    const int bob = int((up / 200) % 2);

    // 头顶 Wi-Fi 接收电极雷达 (x=14, y=11..16)
    d.drawFastVLine(14, 13 + bob, 4, amber);
    d.drawPixel(14, 12 + bob, milk);
    // 雷达向外发射波纹
    if ((up / 300) % 2 == 0) {
      d.drawPixel(13, 10 + bob, cyan);
      d.drawPixel(15, 10 + bob, cyan);
      d.drawPixel(16, 11 + bob, cyan);
    }

    // 圆滚滚大身体
    d.fillRoundRect(5, 17 + bob, 21, 26, 5, mint);
    d.drawFastHLine(8, 17 + bob, 15, color(0xc0ffeau)); // 顶部高光
    // 耳朵
    d.fillRect(6, 15 + bob, 3, 3, mint);

    // 嘴巴：张嘴吃豆与吞下咀嚼交替（嘴尖凹陷中心严格对准 y=25+bob）
    const bool mouthOpen = ((up / 160) % 2) == 0;
    if (mouthOpen) {
      // 张大嘴准备吃芯片：三角形黑色凹陷深入
      d.fillTriangle(26, 20 + bob, 18, 25 + bob, 26, 30 + bob, dark);
      // 上下两颗可爱的小白牙
      d.drawPixel(25, 21 + bob, milk);
      d.drawPixel(25, 29 + bob, milk);
    } else {
      // 咀嚼闭嘴（嘴角微微上扬）
      d.drawFastHLine(22, 25 + bob, 5, dark);
      d.drawPixel(22, 24 + bob, dark);
    }

    // 眨动大眼睛
    const bool blink = ((up / 100) % 30) == 0;
    if (pct >= 100) {
      // 100% 满电完成：开心眯眼笑
      d.drawLine(12, 20 + bob, 14, 19 + bob, dark);
      d.drawLine(14, 19 + bob, 16, 20 + bob, dark);
    } else if (blink) {
      d.drawFastHLine(12, 20 + bob, 5, dark);
    } else {
      d.fillRect(12, 19 + bob, 4, 4, dark);
      d.drawPixel(12, 19 + bob, milk); // 纯白高光
      d.drawPixel(15, 20 + bob, color(0x80ffeeu));
    }
    // 腮红
    d.fillRect(6, 24 + bob, 3, 2, pink);

    // 肚皮透明能量电池仓（随进度充能）
    d.drawRoundRect(8, 28 + bob, 11, 12, 2, color(0x32785fu));
    d.fillRect(9, 29 + bob, 9, 10, color(0x0a1914u));
    // 电池能量充能格 (最大高度 10px)
    const int batH = pct * 10 / 100;
    if (batH > 0) {
      const uint16_t batCol = (pct >= 85) ? color(0xffe57fu) : amber;
      d.fillRect(9, 39 + bob - batH, 9, batH, batCol);
    }
    // 电池中央发光闪电符号
    d.drawLine(13, 31 + bob, 12, 34 + bob, milk);
    d.drawFastHLine(12, 34 + bob, 4, milk);
    d.drawLine(15, 34 + bob, 13, 38 + bob, milk);

    // 小短腿
    d.fillRect(8, 43 + bob, 4, 3, mintD);
    d.fillRect(17, 43 + (1 - bob), 4, 3, mintD);

    // 5. 【红框位置】：右侧放大版 5x7 点阵大号百分比数字 (去掉 PROGRESS，垂直居中 y=36..42)
    {
      static const uint8_t DIGITS_5x7[11][7] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, // 0
        {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, // 1
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, // 2
        {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}, // 3
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, // 4
        {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}, // 5
        {0x0e, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x0e}, // 6
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, // 8
        {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}, // 9
        {0x19, 0x1a, 0x04, 0x0b, 0x13, 0x00, 0x00}  // 10: 标准 5x5 % 符号 (双圆圈+45°倾斜对角线)
      };
      auto drawChar5x7 = [&](int x, int y, int idx, uint16_t c) {
        if (idx < 0 || idx > 10) return;
        for (int r = 0; r < 7; ++r) {
          uint8_t row = DIGITS_5x7[idx][r];
          for (int col = 0; col < 5; ++col) {
            if (row & (1U << (4 - col))) d.drawPixel(x + col, y + r, c);
          }
        }
      };

      char pStr[5];
      snprintf(pStr, sizeof(pStr), "%d", pct);
      const int n = int(std::strlen(pStr));
      // 5x7 数字宽 5px + 间距 1px，整体在 x=31..59 居中，高度在 y=36 居中
      const int totalW = n * 6 + 6; // 数字总宽 + % 符号 6px
      const int x0 = 31 + (28 - totalW) / 2;
      for (int i = 0; i < n; ++i) {
        drawChar5x7(x0 + i * 6, 36, pStr[i] - '0', amber);
      }
      // 标准点阵 % 符号 (左上2x2 + 右下2x2 + 倾斜对角线)
      drawChar5x7(x0 + n * 6, 37, 10, color(0xffe57fu));
    }

    // 6. 底部双层高能进度条 (y=49..54)
    d.drawRect(5, 49, 54, 6, color(0x22485eu));
    d.fillRect(6, 50, 52, 4, color(0x0c1824u));
    const int fillW = pct * 50 / 100;
    if (fillW > 0) {
      d.fillRect(7, 51, fillW, 2, cyan);
      const int headX = 7 + fillW - 1;
      d.drawFastVLine(headX, 50, 4, milk);
      if (headX > 7) d.drawFastVLine(headX - 1, 51, 2, color(0xffe57fu));
    }

    // 7. 底部防断电呼吸警示 (y=57)
    const int breath = int(sinf(float(up) / 220.0f) * 45.0f) + 210;
    const uint16_t warnColor = dim565(amber, breath);
    micro(6, 57, "KEEP POWER ON", warnColor);
  }

  void drawLinkLight() {
    const uint32_t up=c.uptime();
    if (v.otaInProgress) {
      // 升级中专属 7x7 赛博反应堆动画：外环 8 像素能量旋转轮 + 中心金色下落下载箭头
      const uint16_t white = dim565(color(0xffffffu), 255);
      const uint16_t cyanLit = dim565(color(0x5fe0c4u), 230);
      const uint16_t cyanTrail = dim565(color(0x2a7590u), 170);
      const uint16_t ringDark = dim565(color(0x112840u), 120);
      const uint16_t goldLit = dim565(color(0xffbe38u), 245);
      const uint16_t goldHead = dim565(color(0xffe57fu), 255);

      // 1. 外环 8 像素八边形轮廓
      static const int8_t ringX[8] = {60, 62, 63, 62, 60, 58, 57, 58};
      static const int8_t ringY[8] = { 0,  1,  3,  5,  6,  5,  3,  1};
      const size_t spin = (up / 85u) % 8u;
      for (size_t i = 0; i < 8; ++i) {
        uint16_t c = ringDark;
        if (i == spin) c = white;
        else if (i == ((spin + 7) % 8)) c = cyanLit;
        else if (i == ((spin + 6) % 8)) c = cyanTrail;
        d.drawPixel(ringX[i], ringY[i], c);
      }

      // 2. 内部核心：3x5 向下注入金色光箭
      const uint32_t pulse = (up / 160u) % 3u;
      // 垂直箭柄 (60, 1..3)
      d.drawPixel(60, 1, pulse == 0 ? white : goldLit);
      d.drawPixel(60, 2, pulse == 1 ? goldHead : goldLit);
      d.drawPixel(60, 3, pulse == 2 ? goldHead : goldLit);
      // 箭头左右两翼 (59, 3) 和 (61, 3)，以及箭头尖端 (60, 4)
      d.drawPixel(59, 3, goldLit);
      d.drawPixel(61, 3, goldLit);
      d.drawPixel(60, 4, goldHead);
      return;
    }
    if (!v.wifi) {
      const uint16_t red=dim565(color(0xff5968u),176);
      d.drawFastHLine(59,1,4,red);
      d.drawFastVLine(62,2,3,red);
      d.drawFastHLine(58,5,5,red);
      d.drawFastVLine(58,2,3,red);
      return;
    }
    if (c.macLocked) {
      // Mac 锁屏状态：月光琥珀色单点柔和慢呼吸
      const int breath = int(sinf(float(up) / 450.0f) * 40.0f) + 160;
      const uint16_t lockDot = dim565(color(0xffbf75u), breath);
      d.drawPixel(61, 2, lockDot);
      return;
    }
    const bool live=c.serviceOnline;
    // Muted palette: a round-the-clock indicator should read as "alive",
    // not glare. The dormant teal is dimmed less so it stays legible.
    static constexpr uint32_t hues[]={
      0x5fe0c4u, 0x61d8f5u, 0xc393f7u, 0x70e4b3u, 0xffd489u, 0xff9cabu
    };
    constexpr uint32_t hueHold=700u;         // ms per palette step (vs 300ms beat)
    const size_t hi=(up/hueHold)%std::size(hues);
    const uint16_t dim=dim565(color(0x2f6e69u),200);
    const uint16_t lit=dim565(mix(hues[hi],hues[(hi+1)%std::size(hues)],
                                  int((up%hueHold)*255u/hueHold)),150);
    const uint16_t dot=live?lit:dim;

    int lx=60, rx=62, uy=1, dy=3;            // resting arms around center (61,2)
    if (live) {
      // Four 300ms beats, alternating horizontal then vertical. Each axis is
      // out for one beat and home for one beat; the idle beats keep the cross
      // readable and make the motion read as a pulse, not a jump.
      const uint32_t beat = (up/300u) % 4u;
      if (beat==0) { --lx; ++rx; }            // horizontal dots step outward
      else if (beat==2) { --uy; ++dy; }       // vertical dots step outward
    }
    d.drawPixel(61,2,dot);                   // center is fixed
    d.drawPixel(lx,2,dot);
    d.drawPixel(rx,2,dot);
    d.drawPixel(61,uy,dot);
    d.drawPixel(61,dy,dot);
  }
  static uint32_t skyHash(uint32_t value) {
    value = (value^(value>>16))*0x7feb352du;
    value = (value^(value>>15))*0x846ca68bu;
    return value^(value>>16);
  }
  // A meteor may pass BEHIND artwork: we only require the exact cells it
  // occupies to be empty, so the streak is simply hidden where a bright
  // shape crosses it (and reappears past it) — reading as depth. Stars keep
  // the stricter clearances so they never crowd text.
  bool skyPixel(Page page,const uint16_t* pixels,int x,int y,bool meteorPath=false) const {
    if (x<1 || x>62 || y<1 || y>62 || (x>=55 && y<=8)) return false;
    const uint16_t* center=pixels+y*64+x;
    if (meteorPath) return center[0]==0;  // only the streak's own cells
    // Stars: reserve deliberate black space inside and around larger glyphs/UI.
    if (page==LOFT_PAGE && x>=5 && x<=57 && y>=9 && y<=28) return false;
    if (page==QUOTA_PAGE && ((x<=25 && y>=4 && y<=23) ||
        (y>=25 && y<=31) || (x>=28 && y>=36 && y<=54) ||
        (x<=26 && y>=38 && y<=56))) return false;
    if (page==STOCK_PAGE && x<=45 && y>=14 && y<=25) return false;
    return !(center[0] | center[-1] | center[1] | center[-64] | center[64]);
  }
  // Four staggered meteor lanes (2x density) spread across each interval so
  // the sky feels alive without crowding; streak is ~1.5x longer than before.
  // `stepMs` is unchanged, so the extra length reads as a longer trail, not
  // a faster streak.
  void drawMeteorLane(Page page,uint16_t* pixels,uint32_t now,uint32_t seed,unsigned lane) {
    constexpr uint32_t interval=11000u;   // 4 slots per interval, one per lane
    constexpr int steps=13;               // ~1.5x travel length (was 9)
    static constexpr uint32_t flame[][9]={
      {0xfff7da,0xffc47b,0xf18442,0xba512e,0x71322c,0x341d29,0x2a1622,0x1f1019,0x160b12},
      {0xe0fbff,0x8be5ff,0x619bea,0x4a58a4,0x442a61,0x2c1a3d,0x231538,0x1a0f2a,0x130a1e}
    };
    const uint32_t slotSeed=seed+lane*0x9e3779b9u;
    const uint32_t cycle=now/interval;
    const uint32_t phase=now%interval;
    // Four ~2.6s-spaced windows; max start + full 2.6s trail stays < 11s
    // interval so a streak never gets clipped at the cycle boundary.
    const uint32_t start=200u+lane*2600u+skyHash(slotSeed+cycle)%200u;
    constexpr uint32_t stepMs=125u;       // unchanged travel speed
    if (phase<start) return;
    const int head=int((phase-start)/stepMs);
    if (head>=steps+int(std::size(flame[0]))-1) return;
    for (uint32_t attempt=0;attempt<56;++attempt) {
      const uint32_t route=skyHash(slotSeed+cycle*31u+attempt*17u);
      const bool edgeRoute=attempt>=48;
      const int x=edgeRoute?62:6+int(route%50u);
      const int y=edgeRoute?9+int(attempt-48):3+int((route>>8)%55u);
      const int direction=edgeRoute?-1:(route&0x10000u)?1:-1;
      const int pathLength=attempt<26?steps:9;
      // Behind-artwork: only require the first cells to be clear (so a streak
      // never materialises from inside a shape). Beyond that, paint each cell
      // ONLY if it is still black — bright artwork crossing the path simply
      // occludes the streak, which then reappears past it = behind-the-scene
      // depth, vs. the old "find an empty corridor" behaviour.
      if (!skyPixel(page,pixels,x,y,true) ||
          !skyPixel(page,pixels,x+direction,y,true)) continue;
      const auto& colors=flame[(route>>17)&1u];
      for (int tail=0;tail<=head && tail<int(std::size(colors));++tail) {
        const int step=head-tail;
        if (step>=pathLength) continue;
        const int px=x+step*direction,py=y+step/2;
        if (px>0 && px<63 && py>0 && py<63 && pixels[py*64+px]==0)
          pixels[py*64+px]=color(colors[tail]);
        if (tail==2 && px>1 && px<62 && py>1 && py<61 &&
            skyPixel(page,pixels,px+direction,py+1,true))
          pixels[(py+1)*64+px+direction]=mix(0,colors[tail],110);
      }
      return;
    }
  }
  void drawMeteor(Page page,uint16_t* pixels,uint32_t now,uint32_t seed) {
    drawMeteorLane(page,pixels,now,seed,0);
    drawMeteorLane(page,pixels,now,seed,1);
    drawMeteorLane(page,pixels,now,seed,2);
    drawMeteorLane(page,pixels,now,seed,3);
  }
  void drawSky(Page page,uint16_t* pixels) {
    const uint32_t now=c.uptime(),seed=(uint32_t(page)+1u)*0x9e3779b9u;
    drawMeteor(page,pixels,uint32_t(c.uptime()-v.pageShownAt),seed);
    static constexpr uint32_t shades[]={0xffcc91u,0xd6b2ffu,0x9ce4c3u,0xe0eaffu,0xffadc3u,0x93c5ffu};
    constexpr uint32_t halfGlowMs=480;
    // Two layers: 56 brighter twinklers + 40 faint far stars = deeper field.
    for (uint32_t i=0;i<96;++i) {
      const bool far=i>=56;
      const uint32_t j=far?i-56:i;
      const uint32_t period=(far?3100u:2300u)+(j%7u)*170u;
      const uint32_t tick=now+j*(far?977u:581u);
      const uint32_t phase=tick%period;
      if (phase>=2*halfGlowMs) continue;
      const uint32_t pulse=phase<halfGlowMs?phase:2*halfGlowMs-phase;
      const uint32_t star=skyHash(seed+(i+1u)*0x85ebca6bu+tick/period);
      // A star stays in place for one pulse. If artwork moves into it, the star is hidden.
      const int x=int(star&63u),y=int((star>>6)&63u);
      // The loft page's lower band is a room, not sky: a twinkle landing on a
      // black wall reads as a stuck pixel. Stars keep to the clock band.
      if(page==LOFT_PAGE&&y>=31) continue;
      if (!skyPixel(page,pixels,x,y)) continue;
      const uint32_t peak=far?(44u+((star>>20)&31u)):(120u+((star>>20)&63u));
      pixels[y*64+x]=mix(0,shades[(star>>12)%std::size(shades)],
                         (far?6:12)+pulse*peak/halfGlowMs);
    }
  }
  uint16_t mix(uint32_t a, uint32_t b, int fraction) const {
    int r=((a>>16)&255)*(255-fraction)+((b>>16)&255)*fraction;
    int g=((a>>8)&255)*(255-fraction)+((b>>8)&255)*fraction;
    int bl=(a&255)*(255-fraction)+(b&255)*fraction;
    return rgb565(r/255,g/255,bl/255);
  }
  void micro(int x, int y, const char* text, uint16_t shade, int scale=1) {
    while (*text) {
      char ch=*text++; if(ch>='a'&&ch<='z') ch-=32;
      if (ch=='W') {
        // W gets a 5-wide zigzag (three peaks up top, two feet below) — the
        // 3x5 cell is too cramped to tell W apart from U. Strokes stay 1px.
        static constexpr uint8_t w5[5]={0b10101,0b10101,0b01110,0b01010,0b01010};
        for(int row=0;row<5;++row) for(int col=0;col<5;++col)
          if(w5[row]&(1U<<(4-col))) d.fillRect(x+col*scale,y+row*scale,scale,scale,shade);
        x+=6*scale;
        continue;
      }
      uint16_t mask=microGlyph(ch);
      for(int row=0;row<5;++row) for(int col=0;col<3;++col)
        if(mask & (1U << (14-row*3-col))) d.fillRect(x+col*scale,y+row*scale,scale,scale,shade);
      x+=4*scale;
    }
  }
  void number(int x,int y,int value,int scale,uint16_t shade) {
    char text[5]; snprintf(text,sizeof(text),"%d",std::clamp(value,0,100));
    for(const char* p=text;*p;++p) {
      digit(x,y,*p-'0',scale,shade);x+=5*scale+2;
    }
  }
  void digit(int x,int y,int value,int scale,uint16_t shade) {
    for(int row=0;row<9;++row) for(int col=0;col<5;++col)
      if(DIGITS[value][row] & (1U << (4-col))) d.fillRect(x+col*scale,y+row*scale,scale,scale,shade);
  }
  void digit4x7(int x, int y, int value, uint16_t shade) {
    if (value < 0 || value > 9) return;
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = DIGITS_4x7[value][row];
      for (int col = 0; col < 4; ++col) {
        if (bits & (1U << (3 - col))) d.drawPixel(x + col, y + row, shade);
      }
    }
  }
  void clock4x7(int x, int y, int hour, int minute,
                uint16_t hourShade, uint16_t minuteShade, uint16_t colonShade) {
    digit4x7(x, y, hour / 10, hourShade);
    digit4x7(x + 5, y, hour % 10, hourShade);
    d.drawPixel(x + 10, y + 2, colonShade);
    d.drawPixel(x + 10, y + 4, colonShade);
    digit4x7(x + 12, y, minute / 10, minuteShade);
    digit4x7(x + 17, y, minute % 10, minuteShade);
  }
  struct Local { int hour=0,minute=0,second=0,day=0,month=0,weekday=0; bool valid=false; };
  Local localAt(int64_t epoch) const {
    Local value;
    if(c.hasClock) {time_t e=epoch+c.timezoneOffset;struct tm tm;gmtime_r(&e,&tm);
      value={tm.tm_hour,tm.tm_min,tm.tm_sec,tm.tm_mday,tm.tm_mon+1,tm.tm_wday,true};}
    return value;
  }
  Local local() const { return localAt(c.epoch()); }
  // A 7x4 pixel fish. dir=true faces right; tail flickers with the frame.
  void fish(int fx,int fy,bool dir,uint32_t body,uint32_t tail) {
    uint16_t b=color(body), t=color(tail);
    int eye=dir?fx+4:fx, tx=dir?fx-2:fx+5;
    d.fillRect(fx,fy,5,3,b);
    d.fillTriangle(tx,fy+1,tx+2,fy,tx+2,fy+3,t);
    d.drawPixel(eye,fy+1,color(0x10222c));
    d.drawFastHLine(fx+1,fy,3,color(0xfff0c0));
  }
  // Tiny desktop aquarium in the reserved lower-right 22x20 region. Keeps
  // clear of the reset countdown (x<32) and the status dot (x>=60,y>=62).
  void aquarium(int x,int y) {
    const int W=22,H=20;
    for(int row=0;row<H-2;++row)
      d.drawFastHLine(x+1,y+2+row,W-2,mix(0x0d3a52,0x1c6f7d,(H-2-row)*12));
    d.drawFastHLine(x+1,y+2,W-2,color(0x9fd6e0));
    int shimmer=x+2+(frame()/2)%(W-6);
    d.drawFastHLine(shimmer,y+3,3,color(0xbfeaf0));
    d.drawFastHLine(x+1,y+H-2,W-2,color(0xc2a06a));
    for(int i=0;i<7;++i)d.drawPixel(x+2+i*3,y+H-3,color(0xe0c088));
    for(int p=0;p<3;++p)for(int seg=0;seg<7;++seg) {
      int px=x+4+p*6, sway=int((frame()/3+seg)%3)-1;
      d.drawPixel(px+sway,y+H-3-seg,color(seg>4?0x63c48a:0x3f9f6d));
    }
    for(int b=0;b<3;++b) {
      int bx=x+3+(b%2)*2, by=y+H-4-((frame()/2+b*6)%(H-8));
      d.drawPixel(bx,by,color(0x9fe4ef));
    }
    fish(x+3+(frame()/2)%11,y+7,true,0xffd27a,0xff9d5c);
    fish(x+13-(frame()/3)%11,y+12,false,0xff8fb2,0xe4648d);
    d.drawFastVLine(x,y+1,H-1,color(0x6fa8bf));
    d.drawFastVLine(x+W-1,y+1,H-1,color(0x6fa8bf));
    d.drawFastHLine(x,y+H-1,W,color(0x6fa8bf));
    d.drawFastHLine(x,y+1,W,color(0x9fd6e0));
  }
  // The hairline under CODEX / 5H is now the allowance bar itself: filled
  // from the left, a travelling glint inside the fill and a bright tip cursor.
  // Empty track carries 10% ticks so the scale still reads at a glance.
  void quotaBar(int pct,bool known,bool stale,uint32_t accent,int Y=25,int H=3) {
    const int X=3,W=58;
    d.drawFastHLine(X,Y,W,color(0x343a60));
    d.drawFastHLine(X,Y+1,W,color(0x272d4d));
    if (H >= 3) d.drawFastHLine(X,Y+2,W,color(0x1a2038));
    for(int i=1;i<10;++i)d.drawPixel(X+W*i/10,Y,color(0x475183));
    if (!known || pct==0) {
      // Animate the empty track without drawing any filled quota.
      const int scan=int((c.uptime()%3400u)*(W+3)/3400u);
      const uint32_t tint=known?0xffad73u:0xa9caffu;
      for (int i=0;i<4;++i) {
        const int pos=scan-i;
        if (pos>=0 && pos<W) d.drawPixel(X+pos,Y+1,mix(0x272d4du,tint,215-50*i));
      }
      return;
    }
    const int n=std::max(1,pct*W/100);
    d.drawFastHLine(X,Y,n,mix(0x17223du,accent,163));
    d.drawFastHLine(X,Y+1,n,color(accent));
    if (H >= 3) d.drawFastHLine(X,Y+2,n,mix(0x111b39u,accent,120));
    d.drawFastVLine(X+n-1,Y-1,H+2,color(stale?0xd5d0e2u:0xeaffffu));
    const uint32_t sweepColor=stale?0xddc4ffu:0xffffffu;
    if (n<=2) {
      const int beat=int(c.uptime()%1400u);
      const int level=beat<700?beat:1400-beat;
      d.drawFastHLine(X,Y+1,n,mix(accent,sweepColor,55+200*level/700));
      return;
    }
    constexpr uint32_t sweepMs=3150;
    constexpr int glow[] = {235,255,255,238,205,164,125,79,43};
    const int trail=std::min(n,int(std::size(glow)));
    const int head=int((c.uptime()%sweepMs)*(n+trail-1)/sweepMs);
    for (int i=0;i<trail;++i) {
      const int pos=head-i;
      if (pos<0 || pos>=n) continue;
      d.drawPixel(X+pos,Y,mix(stale?0x8f78b6u:0x9bcfffu,sweepColor,glow[i]));
      if (i<=5) d.drawPixel(X+pos,Y+1,mix(accent,sweepColor,glow[i]-55));
    }
  }
  // Slow-rising motes over the aurora: motion without adding another panel.
  void stardust() {
    for(int i=0;i<3;++i) {
      int x=39+(i*11)%23, y=60-((frame()/4+i*9)%23);
      int glow=int(150+70*std::sin(frame()*0.07f+i*1.7f));
      d.drawPixel(x,y,mix(0x291c45u,0xbfa9ffu,std::clamp(glow,0,255)));
    }
  }
  // Frameless quota ambience: four drifting ribbons blended straight into the
  // page gradient. No panel and no border, so the corner stays part of the page.
  // Hue follows the 5-hour allowance and the drift speeds up as it runs out.
  void aurora() {
    const int X0=36, W=28, Y0=38, H=24;
    bool known=v.quota5h>=0;
    int pct=std::clamp(v.quota5h,0,100);
    bool low=known&&pct<20;
    bool stale=known&&(v.quotaStale||(v.quotaAt&&c.hasClock&&c.epoch()-v.quotaAt>300));
    uint32_t bands[4]={
      low?0xff8f6bu:(stale?0x8d90adu:t.quotaColor),
      low?0xffb45bu:(stale?0x9a93b8u:0x9d8cffu),
      low?0xff6b8fu:(stale?0x8a8fa8u:t.accentColor),
      low?0xffd27au:(stale?0x777f9eu:0x6ee7c8u)
    };
    int drift=frame()/(low?1:3);
    auto bgAt=[](int gy)->uint32_t{
      int f=std::clamp(gy*3,0,255);
      return (((0x11*(255-f)+0x29*f)/255)<<16)
           | (((0x1b*(255-f)+0x1c*f)/255)<<8)
           |  ((0x39*(255-f)+0x45*f)/255);
    };
    for(int b=0;b<4;++b) {
      if(!known&&b>1) continue;
      int base=4+b*5;
      float ph=b*1.7f+drift*0.18f;
      float pulse=0.85f+0.15f*std::sin(frame()*0.05f+b*1.3f);
      for(int x=0;x<W;++x) {
        int yy=int(base+2.6f*std::sin((x+drift)*0.22f+ph)
                        +1.2f*std::sin((x+drift*2)*0.5f+ph));
        if(yy<1||yy>=H-1) continue;
        int edge=std::min(6,std::min(x,W-1-x)+2);
        int alpha=int(255*(low?0.95f:0.8f)*pulse*edge/6);
        d.drawPixel(X0+x,Y0+yy,mix(bgAt(Y0+yy),bands[b],alpha));
        d.drawPixel(X0+x,Y0+yy-1,mix(bgAt(Y0+yy-1),bands[b],alpha*34/100));
        d.drawPixel(X0+x,Y0+yy+1,mix(bgAt(Y0+yy+1),bands[b],alpha*45/100));
      }
    }
  }
  static float easeOutBack(float t) {
    float u=t-1.0f;
    return 1.0f+2.70158f*u*u*u+1.70158f*u*u;
  }
  void peekPixel(int x,int y,uint16_t s) {
    // The beacon moved to the header, so paws may reach the very bottom edge.
    if(x>=0&&x<64&&y>=0&&y<64) d.drawPixel(x,y,s);
  }
  // ---- quota page coder scene -----------------------------------------------
  // Pixel programmer in the free bottom-right corner (x29..63, y31..63).
  // 35×33 scene: Matrix-rain monitor + desk + chair + animated coder.
  // States (cycle=400 frames): typing(200) → stretch(40) → nap(80) → walk(60) → bug(20)
  void coderScene() {
    const uint32_t f = frame();
    const int ox = 29, oy = 31;

    // ---- colours ----
    const uint16_t cDesk  = color(0xa89060);
    const uint16_t cDesk2 = color(0x685440);
    const uint16_t cMon   = color(0x28303e);
    const uint16_t cChr   = color(0x3e3430);
    const uint16_t cSkin  = color(0xfcc098);
    const uint16_t cEar   = color(0xe89a72);
    const uint16_t cHair  = color(0x2c1a10);
    const uint16_t cShirt = color(0x5884ca);
    const uint16_t cShrtD = color(0x3a5c9e);
    const uint16_t cPants = color(0x343a4e);
    const uint16_t cShoe  = color(0x201612);
    const uint16_t cSpec  = color(0xc6daf0);
    const uint16_t cEye   = color(0x1a100a);
    const uint16_t cMouth = color(0xbc6250);
    const uint16_t cZzz   = color(0xa0b9d8);
    const uint16_t cExclm = color(0xffd026);
    const uint16_t cSweat = color(0x8ac0ff);
    const uint16_t cThot  = color(0xb9b9da);
    const uint16_t cKeyH  = color(0x707082);
    const uint16_t cKey   = color(0x4b4e5c);
    // Matrix rain gradient: head(bright) → tail(dim)
    const uint16_t rain[6] = {
      color(0xd2ffd2), color(0x00eb50), color(0x00b437),
      color(0x007824), color(0x004414), color(0x001e08)
    };

    // ---- fixed scene ----

    // desk surface
    d.drawFastHLine(ox,    oy+20, 32, color(0x826e52));
    d.fillRect(ox,         oy+21, 32, 2, cDesk);
    d.drawFastHLine(ox,    oy+22, 32, cDesk2);
    d.fillRect(ox+2,       oy+23,  2, 9, cDesk2);
    d.fillRect(ox+26,      oy+23,  2, 9, cDesk2);

    // keyboard
    d.fillRect(ox+13, oy+20, 16, 1, color(0x373a48));
    d.fillRect(ox+13, oy+21, 16, 2, cKey);
    for (int i = 0; i < 6; i++) d.drawPixel(ox+14+i*2, oy+20, cKeyH);

    // monitor shell
    d.fillRect(ox,    oy+6,  13, 16, cMon);
    // pure-black screen
    d.fillRect(ox+1,  oy+7,  11, 11, color(0x000000));
    // monitor stand
    d.fillRect(ox+5,  oy+21,  3,  1, color(0x3c4150));
    d.fillRect(ox+4,  oy+22,  5,  1, color(0x323540));

    // Matrix rain — 11 independent columns, each with its own period & phase.
    // The head travels past the seven rain rows and then dwells off-screen, so a
    // column is dark between passes: fewer stripes on screen, slower drops,
    // and (with mismatched primes) an irregular, non-repeating pattern.
    // Periods scaled again by ~2/3 (another 1.5x faster) while keeping them
    // distinct and non-multiples of each other; phases stay spread so the
    // streaks remain sparse and irregular.
    static const uint16_t rPer[11] = { 69, 71, 73, 75, 85, 87, 91, 93, 99,101,105};
    static const uint8_t  rOff[11] = {  0, 57,113, 23,149, 81,177,129, 41,165, 93};
    const int SPAN = 26; // Leave short gaps between drops; prompt remains on screen.
    for (int col = 0; col < 11; col++) {
      // multiply before divide to stay integer; headY ranges 0..(SPAN-1)
      int headY = ((f + rOff[col]) % rPer[col]) * SPAN / rPer[col];
      for (int row = 0; row < 7; row++) {
        int dist = headY - row;
        if (dist >= 0 && dist < 6) {
          const bool cyanHead = dist == 0 && (col == 2 || col == 8);
          d.drawPixel(ox+1+col, oy+7+row, cyanHead ? color(0x92efff) : rain[dist]);
        }
      }
    }

    // Permanent > prompt and a softly pulsing cursor, below the rain.
    const uint16_t prompt = color(0x4cc090);
    d.drawPixel(ox+2,oy+15,prompt);
    d.drawPixel(ox+3,oy+16,prompt);
    d.drawPixel(ox+2,oy+17,prompt);
    const uint32_t cursorPhase=c.uptime()%1600u;
    const int glow=cursorPhase<800u ? cursorPhase : 1600u-cursorPhase;
    d.drawFastHLine(ox+5,oy+17,3,mix(0x246651u,0xaff8d8u,glow*210/800));

    // chair
    d.fillRect(ox+31, oy+13,  2, 11, cChr);
    d.fillRect(ox+23, oy+23,  9,  2, cChr);
    d.fillRect(ox+23, oy+25,  2,  6, cChr);
    d.fillRect(ox+30, oy+25,  2,  6, cChr);
    d.drawPixel(ox+31, oy+13, color(0x584b41));

    // ---- character sub-routines (lambdas) ----

    // head: hx,hy relative to ox,oy; mouth 0=norm 1=open 2=smile 3=closed(sleep)
    auto drawHead = [&](int hx, int hy, bool blink, int dir, int mouth) {
      d.fillRect(ox+hx-1, oy+hy,   3, 5, cSkin);
      d.fillRect(ox+hx-2, oy+hy+1, 5, 3, cSkin);
      d.fillRect(ox+hx-2, oy+hy,   5, 2, cHair);
      d.drawPixel(ox+hx-2, oy+hy+2, cHair);
      d.drawPixel(ox+hx+2, oy+hy+2, cHair);
      d.drawPixel(ox+hx-1+dir, oy+hy+2, cSpec);
      d.drawPixel(ox+hx+1+dir, oy+hy+2, cSpec);
      if (!blink) {
        d.drawPixel(ox+hx-1+dir, oy+hy+3, cEye);
        d.drawPixel(ox+hx+1+dir, oy+hy+3, cEye);
      } else {
        d.drawPixel(ox+hx-1, oy+hy+3, cHair);
        d.drawPixel(ox+hx+1, oy+hy+3, cHair);
      }
      if      (mouth == 1) { d.drawPixel(ox+hx, oy+hy+4, cEye); }
      else if (mouth == 2) { d.drawPixel(ox+hx-1, oy+hy+4, cMouth);
                             d.drawPixel(ox+hx+1, oy+hy+4, cMouth); }
      else if (mouth == 3) { /* closed — eyes already shut above */ }
      else                 { d.drawPixel(ox+hx,   oy+hy+4, cMouth); }
    };

    auto drawBody = [&](int bx, int by) {
      d.fillRect(ox+bx-2, oy+by,   5, 5, cShirt);
      d.fillRect(ox+bx-2, oy+by,   5, 1, cShrtD);
      d.drawPixel(ox+bx,  oy+by,   cSpec);
    };

    auto drawSitLegs = [&](int lx, int ly) {
      d.fillRect(ox+lx-2, oy+ly,   5, 2, cPants);
      d.fillRect(ox+lx-3, oy+ly+2, 2, 4, cPants);
      d.fillRect(ox+lx+1, oy+ly+2, 2, 4, cPants);
      d.fillRect(ox+lx-4, oy+ly+5, 3, 1, cShoe);
      d.fillRect(ox+lx+1, oy+ly+5, 3, 1, cShoe);
    };

    auto drawArmsTyping = [&](int bx, int by, int ph2) {
      // Shoulders stay attached; one hand presses while the other lifts.
      for (int side : {-1, 1}) {
        const int handY = by + 4 - ((ph2 + (side > 0)) % 2);
        const int handX = bx + side*4;
        d.drawLine(ox+bx+side*2, oy+by+1, ox+handX, oy+handY-1, cShrtD);
        d.drawFastHLine(ox+handX-1, oy+handY, 2, cSkin);
      }
    };

    // tiny Z glyph (3×3)
    auto drawZ = [&](int zx, int zy) {
      d.drawFastHLine(ox+zx,   oy+zy,   3, cZzz);
      d.drawPixel    (ox+zx+1, oy+zy+1,    cZzz);
      d.drawFastHLine(ox+zx,   oy+zy+2, 3, cZzz);
    };

    // Side-on sleeping head, cheek resting on the folded-arm pillow.
    // hx,hy = left,top of a 6×3 face; drawn last so features stay visible.
    auto drawSleepHead = [&](int hx, int hy) {
      // face oval: 6×3 with a rounded crown
      d.fillRect(ox+hx,   oy+hy,   6, 3, cSkin);
      d.drawPixel(ox+hx+1, oy+hy-1,    cSkin);
      d.drawPixel(ox+hx+2, oy+hy-1,    cSkin);
      d.drawPixel(ox+hx+3, oy+hy-1,    cSkin);
      d.drawPixel(ox+hx+4, oy+hy-1,    cSkin);
      // hair sweeping over the back/top-left
      d.fillRect(ox+hx,   oy+hy,   2, 3, cHair);
      d.drawPixel(ox+hx+1, oy+hy-1,    cHair);
      d.drawPixel(ox+hx+2, oy+hy-1,    cHair);
      // ear
      d.drawPixel(ox+hx+2, oy+hy+1,    cEar);
      // closed eye: a short horizontal line
      d.drawFastHLine(ox+hx+3, oy+hy+1, 2, cHair);
      // nose tip on the right edge + small sleeping mouth
      d.drawPixel(ox+hx+5, oy+hy+1,    cSkin);
      d.drawPixel(ox+hx+5, oy+hy+2,    cMouth);
    };

    // ---- state machine ----
    // cycle=400 f: typing 0-199 | stretch 200-239 | nap 240-319 | walk 320-379 | bug 380-399
    const int CYCLE = 400;
    const int ph = f % CYCLE;

    if (ph < 200) {
      // ===== TYPING =====
      int t = ph;
      bool blink   = (f % 120) < 6;
      int  tyPhase = int(f / 8) % 2;
      bool lookDn  = t > 140 && t < 175;
      bool think   = t > 170 && t < 199;

      drawHead(22, 10 + (lookDn ? 1 : 0), blink, lookDn ? 1 : 0, 0);
      drawBody(22, 16);
      drawArmsTyping(22, 16, tyPhase);
      drawSitLegs(22, 22);

      if (think) {
        int prog = (t - 170) / 8;
        d.drawPixel(ox+25, oy+9, cThot);
        if (prog >= 1) { d.drawPixel(ox+26,oy+8,cThot); d.drawPixel(ox+27,oy+7,cThot); }
        if (prog >= 2) { d.fillRect(ox+27,oy+6,2,2,cThot); }
        if (prog >= 3) { d.drawPixel(ox+27,oy+5,cThot); d.drawPixel(ox+28,oy+5,cThot); }
      }
    }
    else if (ph < 240) {
      // ===== STRETCH =====
      int t  = ph - 200;                              // 0-39
      int pn = t < 20 ? t : t < 30 ? 20 : 40 - t;   // 0→20→20→0, denominator=20

      drawHead(22, 10, false, 0, (t > 5 && t < 35) ? 1 : 0);
      drawBody(22, 16);
      for (int side : {-1, 1}) {
        const int elbowX = 22 + side*5;
        const int elbowY = 18 - pn*3/20;
        const int handX = 22 + side*(5-pn/10);
        const int handY = 20 - pn*12/20;
        d.drawLine(ox+22+side*2, oy+17, ox+elbowX, oy+elbowY, cShrtD);
        d.drawLine(ox+elbowX, oy+elbowY, ox+handX, oy+handY, cShirt);
        d.drawPixel(ox+handX, oy+handY, cSkin);
      }
      drawSitLegs(22, 22);
    }
    else if (ph < 320) {
      // ===== NAP (doze off → zzz → startle awake) =====
      int t = ph - 240;   // 0-79

      if (t < 15) {
        // Fold the arms first, then lower the face onto them.
        const int lean = t*5/14;
        drawSitLegs(22, 22);
        drawBody(22-lean/2, 16+lean/2);
        d.fillRect(ox+15, oy+20, 10, 2, cShrtD);
        d.fillRect(ox+16, oy+20, 7, 1, cSkin);
        if (t>=9) drawSleepHead(17+(14-t)/2, 16+(t-9)*2/5);
        else drawHead(22-lean, 10+t*6/9, t>6, -1, t>6?3:0);
      }
      else if (t < 60) {
        // sleeping — side-on face resting on the folded-arm pillow.
        // Body/arms first, head last so the face is not painted over.
        drawBody(22, 16);
        drawSitLegs(22, 22);
        d.fillRect(ox+15, oy+20, 10, 2, cShrtD); // folded arms on desk (pillow)
        d.fillRect(ox+16, oy+20, 7, 1, cSkin);   // forearms peeking out
        drawSleepHead(17, 18);                   // head on the pillow, cheek down

        // zzz bubbles — one group per 25-frame mini-cycle
        int zp = (t - 15) % 25;
        if (zp < 20) {
          int zx = 25 + zp * 3 / 20;
          int zy = 15 - zp * 6 / 20;
          drawZ(zx, zy);
        }
        if (t > 25 && zp > 8 && zp < 25) {
          int sp = zp - 8;
          d.drawPixel(ox+27+sp*2/17, oy+13-sp*7/17,   cZzz);
          d.drawPixel(ox+28+sp*2/17, oy+14-sp*7/17,   cZzz);
          d.drawPixel(ox+27+sp*2/17, oy+15-sp*7/17,   cZzz);
        }
      }
      else {
        // startle awake: head springs up
        const int pr = t - 60;
        const int rise = std::min(8, pr*8/12);
        const int bx = 20+rise/4;
        drawSitLegs(22, 22);
        drawBody(bx, 18-rise/4);
        if (pr<4) {
          d.fillRect(ox+15, oy+20, 10, 2, cShrtD);
          d.fillRect(ox+16, oy+20, 7, 1, cSkin);
          drawSleepHead(17+pr/2, 18-pr/2);
        } else {
          const int headY=18-rise;
          drawArmsTyping(bx, 18-rise/4, 0);
          drawHead(bx, headY, false, 0, pr<15?1:0);
          if (pr>=7 && pr<14) {
            d.drawPixel(ox+bx+4, oy+headY+pr-7, cSweat);
          }
        }
      }
    }
    else if (ph < 380) {
      // ===== WALK =====
      int t = ph - 320;   // 0-59

      const int walkFrame = std::clamp(t-10,0,39);
      const int rise = t<10 ? t*4/9 : t<50 ? 4 : (59-t)*4/9;
      int wx = 0;
      if (t>=10 && t<50) {
        if (walkFrame<10) wx = walkFrame/2;
        else if (walkFrame<20) wx = (20-walkFrame)/2;
        else if (walkFrame<30) wx = -(walkFrame-20)/2;
        else wx = -(40-walkFrame)/2;
      }
      const int bx=22+wx, by=16-rise;
      const bool walking=t>=10 && t<50;
      const int facing=walking ? (walkFrame>=10&&walkFrame<30?-1:1) : 0;
      const int stride=walking ? ((walkFrame/5)%2 ? 1 : -1) : 0;

      // Feet stay on the same floor line through sitting and standing.
      for (int side : {-1, 1}) {
        const int footX=bx+side*2+side*stride;
        const int footY=27-(walking && side==stride ? 1 : 0);
        d.drawLine(ox+bx+side, oy+by+5, ox+footX, oy+footY-1, cPants);
        d.drawFastHLine(ox+footX-1, oy+footY, 3, cShoe);
        const int handY=by+5+side*stride;
        d.drawLine(ox+bx+side*2, oy+by+1, ox+bx+side*3, oy+handY, cShrtD);
        d.drawPixel(ox+bx+side*3, oy+handY, cSkin);
      }
      drawBody(bx, by);
      drawHead(bx, by-6, false, facing, 0);
    }
    else {
      // ===== BUG =====
      int t = ph - 380;   // 0-19

      if (t < 15) {
        int ey = std::clamp(7 - (t < 8 ? t/3 : 2-(t-8)/6), 0, 7);
        d.drawPixel(ox+10, oy+ey,   cExclm);
        d.drawPixel(ox+10, oy+ey+2, cExclm);
        // The rotated DMA rectangle routine requires positive width and height.
        // A few explicit terminal pixels keep the alert inside the monitor.
        const uint16_t alert = color(t<10 ? 0xe47272 : 0x963d47);
        for (int row=0; row<3; ++row) {
          if (t < 2+row*2) continue;
          for (int col=0; col<7-row; ++col)
            d.drawPixel(ox+3+col, oy+9+row*3, alert);
        }
      }

      bool fp = t > 10 && t < 18;
      drawHead(22, 10+(fp?1:0), false, fp?-1:0, fp?1:0);

      if (t > 12) {
        int sy = int(f) % 12;
        if (sy < 9) d.drawPixel(ox+27, oy+10+sy, cSweat);
      }

      drawBody(22, 16);
      if (fp) {
        d.drawLine(ox+24, oy+17, ox+26, oy+16, cShrtD);
        d.drawLine(ox+26, oy+16, ox+24, oy+13, cShirt);
        d.drawFastHLine(ox+23, oy+13, 2, cSkin);
        d.drawLine(ox+20, oy+17, ox+18, oy+20, cShrtD);
        d.drawPixel(ox+18, oy+20, cSkin);
      } else {
        drawArmsTyping(22, 16, 0);
      }
      drawSitLegs(22, 22);
    }
  }
  // The 3x5 text font cannot draw a legible W, so the weekly label carries its
  // own 5x5 glyph and shares the baseline with the digits that follow it.
  void wMark(int x,int y,uint16_t shade,int scale=1) {
    static const uint8_t rows[5]={0b10001,0b10001,0b10101,0b10101,0b01010};
    for(int r=0;r<5;++r)for(int c=0;c<5;++c)
      if(rows[r]&(0x10>>c))d.fillRect(x+c*scale,y+r*scale,scale,scale,shade);
  }
  // A full 5H allowance gets a bright twinkling star instead of a cramped
  // three-digit "100" that would run into the weekly figure beside it.
  void fullStar(int x,int y,uint32_t accent) {
    static const uint8_t STAR[7]={
      0b0001000,0b0001000,0b0011100,0b0111110,
      0b0011100,0b0101010,0b1000001
    };
    const int s=2;
    uint16_t gold=color(accent),glow=color(0xfff2c4);
    for(int r=0;r<7;++r)for(int c=0;c<7;++c)
      if(STAR[r]&(1U<<(6-c))) d.fillRect(x+c*s,y+r*s,s,s,gold);
    if((frame()/5)%2) d.fillRect(x+3*s-1,y+2*s,2,2,glow);
  }
  void drawQuota() {
    bool known=v.quota5h>=0;
    bool stale=known&&(v.quotaStale||(v.quotaAt&&c.hasClock&&c.epoch()-v.quotaAt>300));
    int pct=std::clamp(v.quota5h,0,100);
    uint32_t accent=stale?0xa8a7bf:(known&&pct<20?0xffb45b:t.quotaColor);

    // The hero figure shifts with the allowance: a soft macaron red when it
    // runs low (easy on the eye, never a harsh pure red), warming through
    // amber and settling into calm white when there is plenty left.
    uint16_t figure;
    if (stale) figure=color(0xb7b6c6u);
    else if (pct>=70) figure=color(0xf6f4ffu);
    else if (pct>=45) figure=mix(0xf2c286u,0xf6f4ffu,(pct-45)*255/25);
    else if (pct>=15) figure=mix(0xf29aa2u,0xf2c286u,(pct-15)*255/30);
    else figure=mix(0xea8496u,0xf29aa2u,pct*255/15);

    // 5H Hero Big Number (Left, Y=5, bottom Y=22 perfectly aligned with W30)
    if(known&&pct>=100) {
      fullStar(5,5,accent);
    } else if(known) {
      number(3,6,pct,2,color(0x11152c));
      number(2,5,pct,2,figure);
    } else micro(4,8,"--",color(0x9693b4),3);

    // Refill time beside the main allowance figure.
    uint16_t dial=color(stale?0xc9a68c:0xffc878);
    d.drawCircle(30,6,2,dial);
    d.drawPixel(30,6,dial); d.drawPixel(30,5,dial);
    char text[8]="--:--";
    if(known&&v.reset5hAt&&c.hasClock) {
      Local refill=localAt(v.reset5hAt);
      if(refill.valid) snprintf(text,sizeof(text),"%02d:%02d",refill.hour,refill.minute);
    }
    micro(35,4,text,color(stale?0xe2bd9d:0xffd188));

    const uint16_t weeklyLabel=color(0xb2aed4u);
    if (v.quotaWeek>=0) {
      const int weekly=std::clamp(v.quotaWeek,0,100);
      const int digits=weekly==100?3:weekly>=10?2:1;
      constexpr int weekX=36;
      wMark(weekX-7,16,weeklyLabel);
      number(weekX,14,weekly,1,color(0xd8d1ffu));
      static constexpr uint8_t percent[]={0b11001,0b11010,0b00010,0b00100,0b01000,0b01011,0b10011};
      for (int y=0;y<7;++y) for (int x=0;x<5;++x)
        if (percent[y]&(0x10>>x)) d.drawPixel(weekX+1+digits*6+x,15+y,color(0xd8d1ffu));
    } else {
      wMark(29,16,weeklyLabel);
      micro(36,16,"--",weeklyLabel);
    }

    // Full-width 5H glowing progress bar divider (Y=25, 3px thick, 2px gap above)
    quotaBar(pct,known,stale,accent,27,3);

    // Bottom Left: Date & Clear Medium Clock
    Local now=local();
    char date[12]="--/--";
    if(now.valid)snprintf(date,sizeof(date),"%02d/%02d",now.month,now.day);
    micro(3,33,date,color(0xd9c6b0));

    if(now.valid) {
      // Soft mint hours + warm amber minutes, calm blue-grey colon — the old
      // near-white (0xf6f8ff) was too hot sitting between the two rails.
      clock4x7(3,44,now.hour,now.minute,
               color(0x8fd9c4u),color(0xe8bc8eu),color(0x9fb2c8u));
      // Two 30-second bands: upper = 0-29s, lower = 30-59s. Each band is a
      // 26x2 dot matrix from the user's pixel pattern; dots are read left to
      // right, upper row before lower within a column. 30 dots = 30 seconds,
      // so the lit head steps exactly one dot per real second (now.second) —
      // no fractional stepping, no stall. Blank pattern cells stay dark.
      // State colours: dim blue = upcoming, green = passed, red = live head.
      // Each tone is mixed onto the deep base so none reads as a flat pure
      // colour, and the three levels separate cleanly on these tiny dots.
      constexpr int railX=1;
      constexpr uint32_t base=0x121f34u;
      // {row,col} of the 30 pattern dots, in time order (row0 = pattern line 1).
      static constexpr uint8_t DOT[30][2]={
        {0,0},{1,0},{0,1},{1,1},{0,2},{1,3},{0,4},{1,5},{0,6},{0,7},
        {1,8},{0,9},{1,10},{0,11},{1,12},{1,13},{0,14},{1,15},{0,16},{1,17},
        {0,18},{0,19},{1,20},{0,21},{1,22},{0,23},{0,24},{1,24},{0,25},{1,25}
      };
      // NB: mix() already returns a packed RGB565 uint16_t — do NOT wrap it
      // in color() again, or the packed value is decoded as 0xRRGGBB and red
      // turns green / green turns cyan. Use the mixed values directly.
      const uint16_t upcoming=mix(base,0x2f66b4u,92);   // dim blue
      const uint16_t passed  =mix(base,0x35d07fu,170);  // green
      const uint16_t live    =mix(base,0xff5264u,236);  // red head
      const int sec=now.second;   // 0..59 — one full minute
      for (int band=0;band<2;++band) {
        const int bandSec=sec-band*30;   // 0..29 within this band
        for (int i=0;i<30;++i) {
          const int row=DOT[i][0], col=DOT[i][1];
          // Physical y: upper band row0=41 (outer), row1=42 (inner);
          // lower band row0=52 (inner), row1=53 (outer).
          const int y=band==0?(row==0?41:42):(row==0?52:53);
          uint16_t shade;
          if      (bandSec<0 || i>bandSec) shade=upcoming;  // not reached
          else if (i<bandSec)              shade=passed;    // elapsed
          else                             shade=live;      // i==bandSec
          d.drawPixel(railX+col,y,shade);
        }
      }
    } else {
      micro(5,45,"--:--",color(0xa0a7c8));
    }

    // Wordmark: first four glyphs static, trailing X is a jelly animation.
    const uint16_t letters[]={0b000011100100011,0b000010101101010,
                              0b001001011101011,0b000010101110011};
    static constexpr uint32_t palette[]={0x61d8f5u,0xc393f7u,0xffb879u,0x70e4b3u};
    constexpr uint32_t colorStep=720;
    const uint16_t wordmark=color(0xf5f7ffu);
    for (size_t letter=0;letter<std::size(letters);++letter)
      for (int y=0;y<5;++y) for (int x=0;x<3;++x)
        if (letters[letter]&(1U<<(14-y*3-x)))
          d.drawPixel(3+int(letter)*4+x,57+y,wordmark);

    // X idle + random bursts. Time is divided into ~5s opportunity slots;
    // each slot gets a pseudo-random start delay, duration and one of five
    // moves (or a full rest), so the X just sits still most of the time and
    // suddenly performs a hop / dart / shake / double-hop / centred grow,
    // then settles back home. The grow is one calm symmetric zoom (never a
    // continuous breathing loop). Deterministic from uptime so the Mac
    // preview matches the device frame-for-frame.
    {
      const uint16_t glyph=microGlyph('X');
      const uint32_t t=c.uptime();
      constexpr uint32_t slotMs=5200u;
      const uint32_t slot=t/slotMs, st=t%slotMs;
      const uint32_t rnd=skyHash(slot*0x9e3779b9u+0x51ed3b7u);
      const bool rest=(rnd%5)==0;                    // 1 in 5 slots: no move
      const uint32_t start=300u+((rnd>>3)%1800u);  // 0.3..2.1s into the slot
      const uint32_t dur=900u+((rnd>>13)%700u);    // 0.9..1.6s performance
      const int kind=int((rnd>>23)%5u);
      int ox=0, oy=0, w=3, h=5;
      if (!rest && st>=start && st<start+dur) {
        const float p=float(st-start)/float(dur);   // 0..1
        const float env=sinf(p*3.14159265f);        // eases out and back home
        switch (kind) {
          case 0:  // single hop straight up, up to 3px
            oy=-int(roundf(env*3.0f)); break;
          case 1:  // dart right up to 5px and back
            ox=int(roundf(env*5.0f)); break;
          case 2:  // quick left-right shiver with a tiny lift
            ox=int(roundf(sinf(p*18.8495559f)*env));
            oy=-int(roundf(env)); break;
          case 3:  // double hop: two bounces inside one burst
            oy=-int(roundf(fabsf(sinf(p*6.2831853f))*3.0f)); break;
          default: // grow: one calm in-place zoom 3x5 -> 5x7 and back.
            w=3+int(roundf(env*2.0f));              // symmetric, no drifting
            h=5+int(roundf(env*2.0f)); break;
        }
      }
      // Home is x0=19,y0=57 (bottom 61). Grow zooms around the glyph centre
      // with the bottom edge pinned, so it reads as a clean pulse, not a wobble.
      const int x0=(w==3?19+ox:20-w/2);  // grow: 18..20, right edge <=22
      const int y0=(h==5?57+oy:62-h);    // grow: 55..57, top > rail 52-53
      for (int dy=0;dy<h;++dy) {
        const int sy=int((dy+0.5f)*5.0f/float(h));   // centre-aligned, symmetric
        for (int dx=0;dx<w;++dx) {
          const int sx=int((dx+0.5f)*3.0f/float(w));
          if (!(glyph&(1U<<(14-sy*3-sx)))) continue;
          const uint32_t cp=(t+uint32_t((dx+dy)*155u))%(colorStep*std::size(palette));
          const size_t ci=cp/colorStep;
          const uint16_t ink=mix(palette[ci],palette[(ci+1)%std::size(palette)],int(cp%colorStep)*255/colorStep);
          d.drawPixel(x0+dx,y0+dy,ink);
        }
      }
    }

    // Bottom Right: pixel programmer (typing / stretch / nap / walk / bug).
    coderScene();
  }
  // SYSTEM page: a plain text readout, split into the only two things worth
  // checking on a panel — the network path and the box itself.
  //   WIFI  signal        MAC   link state      IP   address    TIME  source
  //   MODE  mode/source   UP    runtime         HEAP free KB
  // No quota rows (that page is right next door), no icon glyphs, no filled
  // bands: every lit pixel here is a letter. Colour carries status only —
  // green=ok, amber=warn (token stale / clock from Mac only), red=fail, grey=label.
  void drawStatus() {
    const uint32_t ok=0x86e6b7u, warn=0xffbe76u, fail=0xff5968u;
    const uint32_t labelC=0x6f86b8u, dimC=0x5a6f9au, infoC=0xa9c0ffu;
    const bool macOk=c.serviceOnline, macWarn=!macOk&&v.httpCode==401;
    // No title, no rules, no bands: nine plain rows on a 7 px grid (2 px gaps),
    // because 64 px is not enough room to waste on air. The last two rows are
    // the MPU6050 slot: they already exist and read "--" until it is wired.
    const int yWifi=1, yMac=8, yIp=15, yTime=22, yMode=29, yUp=36, yMem=43,
                  yMpu=50, yPose=57;
    // Values all end on the same column, so the right edge reads as one rail.
    auto value=[&](int y,const char* text,uint32_t hex) {
      micro(62-4*int(std::strlen(text)),y,text,color(hex));
    };
    auto label=[&](int y,const char* text) { micro(3,y,text,color(labelC)); };
    auto row=[&](int y,const char* name,const char* text,uint32_t hex) {
      label(y,name); value(y,text,hex);
    };

    // ---- network ----
    if (v.wifi) {
      char sig[10];
      if (v.rssi) snprintf(sig,sizeof sig,"OK %d",int(v.rssi));
      else        snprintf(sig,sizeof sig,"OK");
      row(yWifi,"WIFI",sig,ok);
    } else row(yWifi,"WIFI","OFF",fail);

    row(yMac,"MAC",macOk?"LINK":(macWarn?"TOKEN":"OFF"),
        macOk?ok:(macWarn?warn:fail));

    // A /24 IPv4 address (13 glyphs, 51 px) does not fit beside a label, and
    // truncating it hid the leading "1" (…92.168.31.88). Long addresses drop
    // the label and start at the margin instead — dots make them obvious.
    const char* ip=v.ip[0]?v.ip:"--";
    if (std::strlen(ip)>12) micro(3,yIp,ip,color(infoC));
    else row(yIp,"IP",ip,infoC);

    row(yTime,"TIME",v.clockSrc==1?"NTP":(v.clockSrc==2?"MAC":"--"),
        v.clockSrc==1?ok:(v.clockSrc==2?warn:fail));

    // ---- system ----
    label(yMode,"MODE");
    micro(23,yMode,c.mode==WORK?"WORK":"HOME",color(c.mode==WORK?0x95baffu:0xc9a2efu));
    { // Right side = who currently owns the page (manual lease shows minutes left).
      char src[6];
      const uint32_t rem=c.manualRemainingMs()/60000u;
      if (c.source==MANUAL&&rem>0) snprintf(src,sizeof src,"%uM",unsigned(rem>99?99:rem));
      else snprintf(src,sizeof src,"%s",c.source==MAC_PRESENCE?"MAC":c.source==MANUAL?"MAN":"AUTO");
      value(yMode,src,dimC);
    }

    { char up[10]; const uint32_t mins=uint32_t(c.uptime()/60000u);
      if (mins>=1440u)    snprintf(up,sizeof up,"%uD%02uH",mins/1440u,(mins/60u)%24u);
      else if (mins>=60u) snprintf(up,sizeof up,"%uH%02uM",mins/60u,mins%60u);
      else                snprintf(up,sizeof up,"%uM",mins);
      row(yUp,"UP",up,infoC);
    }

    { char heap[8];
      if (v.heapKb) snprintf(heap,sizeof heap,"%uK",unsigned(v.heapKb));
      else          snprintf(heap,sizeof heap,"--");
      row(yMem,"MEM",heap,dimC);   // free RAM in KB (HEAP was jargon)
    }

    // ---- MPU6050 slot: wiring check + the one number worth watching ----
    if (!v.mpuPresent) {
      row(yMpu,"MPU","--",dimC);
      row(yPose,"POSE","--",dimC);
    } else {
      row(yMpu,"MPU","0X68",ok);
      char pose[8]; uint32_t ink=ok;
      switch (v.mpuPose) {
        case 1: snprintf(pose,sizeof pose,"CAL"); ink=warn; break;
        case 4: snprintf(pose,sizeof pose,"FLIP"); ink=fail; break;
        case 5: snprintf(pose,sizeof pose,"SHAKE"); ink=warn; break;
        default: snprintf(pose,sizeof pose,"%uD",unsigned(v.tiltDeg));
                 ink = v.tiltDeg>=35 ? fail : (v.tiltDeg>=8 ? warn : ok); break;
      }
      row(yPose,"POSE",pose,ink);
    }
  }
  // Program-drawn space views; share the room canvas, no persistent sprite RAM.
struct SpaceScene {
 static constexpr int orbitEarthX=10,orbitEarthY=60,orbitEarthRadius=32;
 Surface& d;Renderer& r;
 uint32_t tick() const { return uint32_t(r.c.uptime()/100u); }
 uint16_t ink(uint32_t c){return desk::color(c);}
 void p(int x,int y,uint32_t c){d.drawPixel(x,y,ink(c));}
 void stars(int seed){
  for(int i=0;i<36;++i){uint32_t h=r.skyHash(uint32_t(i)*977u+seed);int x=h%64,y=(h>>8)%64;
   if((x<34&&y<22)||(x>=55&&y<9))continue;
   uint32_t c=i%5?0x647287:0xb5bdc9;
   int glow=130+int(45*std::sin(tick()*0.06f+i*1.9f));
   d.drawPixel(x,y,r.mix(0,c,glow));
   if(i%17==0){p(x-1,y,0x38465a);p(x+1,y,0x38465a);p(x,y-1,0x38465a);p(x,y+1,0x38465a);}
  }
 }
 void earth(int cx,int cy,int rad,int latitudeBias=0){
  for(int y=std::max(0,cy-rad-1);y<std::min(64,cy+rad+2);++y)
   for(int x=std::max(0,cx-rad-1);x<std::min(64,cx+rad+2);++x){
    int dx=x-cx,dy=y-cy,q=dx*dx+dy*dy;
    if(q>(rad+1)*(rad+1))continue;
    if(q>rad*rad){p(x,y,0x709ead);continue;}
    // Longitude moves under a fixed sphere, independently of cloud drift.
    int v=dy*24/std::max(1,rad)+latitudeBias;
    float depth=std::sqrt(float(std::max(0,rad*rad-dx*dx-dy*dy)));
    int longitude=int(std::atan2(float(dx),depth)*20.37f);
    int spin=int(tick()/3u)%64;
    int u=(longitude+spin+96)%64-32;
    bool land=((u+8)*(u+8)*2+(v+7)*(v+7)<110)||
      ((u+3)*(u+3)*3+(v-3)*(v-3)<120)||
      ((u-9)*(u-9)+(v+3)*(v+3)*2<80)||
      ((u+2)*(u+2)*2+(v+13)*(v+13)<70)||
      ((u-14)*(u-14)*3+(v-6)*(v-6)<55);
    bool cloud=(v+u/3>-15&&v+u/3<-10&&u<9)||
      (v-u/4>6&&v-u/4<10&&u>-8)||
      (v+u/5>-3&&v+u/5<1&&u>-22&&u<4);
    // Polar ice caps sit over whatever the surface says; clouds still win.
    const bool ice=v>16||v<-19;
    uint32_t base=cloud?0xc1d8da:(ice?0xdbe7e6:(land?0x5c9180:0x326d9a));
    // Slow large-scale sheen keeps the oceans from reading as flat fill.
    int lit=std::clamp(200-dx*75/std::max(1,rad)-dy*35/std::max(1,rad)
      +int(7*std::sin(float(u)*0.33f)+5*std::sin(float(v)*0.45f)),70,235);
    d.drawPixel(x,y,r.mix(0x101d30,base,lit));
   }
 }
 void hud(){
  // Only the readout is drawn here. There is no backing rectangle: artwork
  // that drifts into this corner simply passes behind the digits, which are
  // painted last. A black patch used to swallow whatever crossed it (the
  // earth in the moon scene, the station's orbit) and read as a hole in the
  // picture, not as a HUD.
  auto t=r.local();char date[6];snprintf(date,sizeof(date),"%02d/%02d",t.month,t.day);
  r.micro(3,16,t.valid?date:"--/--",ink(0xadb5b9));
  const auto white=ink(loftClockInk()?loftClockInk():r.t.clockColor);
  if(!t.valid) {
    for(int x:{3,9,20,26}) d.drawFastHLine(x,7,5,white);
    return;
  }
  r.digit(3,3,t.hour/10,1,white);r.digit(9,3,t.hour%10,1,white);
  d.fillRect(16,5,1,2,white);d.fillRect(16,9,1,2,white);
  r.digit(20,3,t.minute/10,1,white);r.digit(26,3,t.minute%10,1,white);
 }
 // A tiny drawing adapter, not a framebuffer. The station is a flat sprite, so
 // it can never be *partly* inside the globe while still in front of it: on the
 // near half of the orbit every pixel is drawn, and on the far half the globe's
 // silhouette cuts it. That is only pop-free because the orbit places the
 // front/behind crossing on the rim itself (overlap ~1px there) — the gradual
 // disappearance comes from the station's own motion carrying it deeper behind
 // the limb arc, never from a soft fade.
 struct StationSurface : Adafruit_GFX {
  Surface& target; int occ2;
  StationSurface(Surface& surface,int occlusionRadius2)
    :Adafruit_GFX(64,64),target(surface),occ2(occlusionRadius2) {}
  void drawPixel(int16_t x,int16_t y,uint16_t c) override {
    if(x<0||x>=64||y<0||y>=64)return;
    if(!occ2){ target.drawPixel(x,y,c); return; }
    const int dx=x-orbitEarthX,dy=y-orbitEarthY,q=dx*dx+dy*dy;
    if(q<=occ2)return;
    target.drawPixel(x,y,c);
  }
 };
 void station(int cx,int cy,int occ2){
  constexpr float scale=1.0f; // Preserve the approved full-size station artwork.
  // The station is deliberately drawn much larger than astronomical scale.
  StationSurface canvas(d,occ2);
  auto sx=[&](int x){ return cx+int(std::lround((x-38)*scale)); };
  auto sy=[&](int y){ return cy+int(std::lround((y-33)*scale)); };
  auto size=[&](int n){ return std::max(1,int(std::lround(n*scale))); };
  auto shade=[&](uint32_t rgb){ return ink(rgb); };
  auto line=[&](int x,int y,int xx,int yy,uint32_t rgb){canvas.drawLine(sx(x),sy(y),sx(xx),sy(yy),shade(rgb));};
  line(27,43,48,22,0x697b8b);line(28,43,49,22,0xa0aeb8);
  auto wing=[&](int x,int y){
   canvas.fillRect(sx(x),sy(y),size(9),size(12),shade(0x987044));
   canvas.drawRect(sx(x),sy(y),size(9),size(12),shade(0xcfaa69));
   line(x+4,y,x+4,y+11,0x3c485c);
   for(int yy=y+3;yy<y+12;yy+=3)line(x,yy,x+8,yy,0x4d5260);
  };
  wing(19,31);wing(46,26);line(27,37,46,32,0xb8c7ce);
  canvas.fillRoundRect(sx(33),sy(30),size(10),size(7),size(2),shade(0xc6d1d1));
  canvas.fillRect(sx(36),sy(27),size(4),size(13),shade(0x859ba9));
  canvas.fillCircle(sx(38),sy(33),size(3),shade(0xdde3db));
  canvas.fillRect(sx(37),sy(31),size(3),size(3),shade(0x416b86));
  line(41,28,45,23,0xdde3db);
  canvas.fillCircle(sx(46),sy(22),size(2),shade(0x8d9aa8));
  line(47,22,49,20,0xc5cfd3);
  canvas.drawPixel(sx(35),sy(38),shade(tick()%23u<4u?0xffd79a:0x9b624c));
 }
 void orbit(){
  stars(81);
  // Keep the original Earth radius. Moving the centre slightly right/up
  // shows about 2/5 of the globe instead of the original 1/4. The approved
  // station artwork, dimensions and colours are unchanged.
  const uint32_t phase=tick()%140u;
  const float degrees=phase<110u?100.0f-float(phase)*200.0f/110.0f:
    -100.0f-float(phase-110u)*160.0f/30.0f;
  const float angle=degrees*0.0174532925f;
  // A wider major axis (55px) makes the visible arc follow the planet's
  // curvature: it skims over the upper limb, and the far-side crossing lands
  // exactly on the rim, where the sprite's nearest corner is ~1px clear of
  // the silhouette. The front/behind flip therefore swaps draw order with no
  // overlap to pop, and from there the silhouette clip eats the station
  // gradually — the cut always follows the limb arc, so the station visibly
  // slides behind the planet instead of blinking or getting buried core-first.
  const float along=55*std::cos(angle),across=12*std::sin(angle);
  const float depth=51.85f*std::sin(angle);
  // The orbit plane rides 30 degrees above the horizontal, not 45: a 45-degree
  // diagonal threw the station into the top-right corner, where its wings
  // reached the globe's disc and the depth test started shaving them off.
  constexpr float kCosTilt=0.8660254f,kSinTilt=0.5f;
  const int cx=orbitEarthX+int(std::lround(along*kCosTilt-across*kSinTilt));
  const int cy=orbitEarthY-int(std::lround(along*kSinTilt+across*kCosTilt));
  // Binary occlusion: full silhouette (rim included) once behind, nothing in
  // front. The gradual look comes from the orbit geometry above, not from a
  // fade — any soft cone eats core-first and reads as the sprite sinking into
  // the middle of the planet.
  const int occ2=depth<0?(orbitEarthRadius+1)*(orbitEarthRadius+1):0;
  // The visible arc is slow; the far-side arc is quicker while fully hidden.
  // Preserve the original rotating land and cloud texture.
  earth(orbitEarthX,orbitEarthY,orbitEarthRadius);
  // A distant little moon uses the previously empty area to the right of
  // the time. Its silhouette stays clear of the existing link indicator.
  for(int y=11;y<=19;++y)for(int x=49;x<=57;++x){
   const int dx=x-53,dy=y-15;
   if(dx*dx+dy*dy>16)continue;
   d.drawPixel(x,y,r.mix(0x202c40u,0xbac3bfu,std::clamp(180-dx*18-dy*6,45,240)));
  }
  d.fillCircle(52,14,1,ink(0x68787e));
  d.drawPixel(54,17,ink(0x667780));
  station(cx,cy,occ2);
  hud();
 }
 void moonbase(){
  stars(59);
  // The earth crosses the lunar sky once per cycle and keeps going: it enters
  // off-frame on the left and leaves off-frame on the right, so the loop
  // restarts where nothing is visible. The old cosine shuttle turned it round
  // mid-sky, which looked like a yo-yo rather than a planet.
  const float trip=float(tick()%220u)/219.0f;
  earth(-10+int(std::lround(84*trip)),34-int(std::lround(20*trip)),7);
  // Only actual lunar ground is filled; the sky remains LED-off black.
  for(int x=0;x<64;++x){int top=45+(x<20?std::abs(x-9)/7:(x<43?std::abs(x-32)/9:-(x-43)/7));
   for(int y=top;y<64;++y){int k=(y-top)*4;d.drawPixel(x,y,r.mix(0x424957,0x747a82,std::clamp(k,0,130)));}
   p(x,top,0x94999c);
  }
  // Crater rims and wheel tracks.
  d.drawEllipse(9,61,7,1,ink(0x333d4a));d.drawEllipse(52,57,6,2,ink(0x3c4653));
  d.drawFastHLine(49,59,7,ink(0x8b9091));
  for(int x=15;x<31;x+=3){p(x,57,0x8b9091);p(x-1,59,0x8b9091);}
  // Pressurised dome, ribs and a small warm airlock.
  for(int y=35;y<=47;++y)for(int x=29;x<=51;++x){int dx=x-40,dy=y-47;
   if(dx*dx+dy*dy<=121){d.drawPixel(x,y,r.mix(0x334e67,0x8db7c5,std::clamp(210-(x-29)*5,50,210)));}
  }
  d.drawFastHLine(29,47,23,ink(0xb7c6c8));
  d.drawLine(40,36,40,47,ink(0xc2d3cf));
  d.drawLine(35,38,32,47,ink(0x8dadaf));d.drawLine(45,38,48,47,ink(0x8dadaf));
  d.fillRect(29,48,23,4,ink(0x747f88));
  d.fillRoundRect(37,44,7,8,1,ink(0xe0c38c));
  d.fillRect(39,46,3,5,ink(0x493f3b));p(42,48,tick()%63u<7u?0xf5db9a:0xad9878);
  // Solar array beside the dome.
  d.fillRect(5,45,14,4,ink(0x436987));d.drawRect(5,45,14,4,ink(0xa2adb2));
  for(int x=8;x<19;x+=4)d.drawFastVLine(x,45,4,ink(0x7998ae));
  d.drawFastVLine(11,49,4,ink(0xb5bcc0));
  // A ten-second patrol fits inside a 15-second scene: 4s out, 1s
  // stopped, 4s back, 1s stopped; one pixel every 0.4s while driving.
  const uint32_t ph=tick()%100u;
  const int travel=ph<40u?int(ph/4u):ph<50u?10:ph<90u?10-int((ph-50u)/4u):0;
  const int rx=13+travel;
  d.fillRect(rx,52,12,4,ink(0xd5d1bb));d.fillRect(rx+4,49,6,3,ink(0xb6c5c7));
  d.fillRect(rx+6,50,3,2,ink(0x4c7188));d.drawLine(rx+2,51,rx+2,46,ink(0xc0c9c9));
  p(rx+2,45,tick()%31u<5u?0xf1be78:0x9a7555);
  for(int x=rx+1;x<=rx+11;x+=5){d.fillCircle(x,57,2,ink(0x202c38));p(x+(travel%2),57,0x9a9f9e);}
  hud();
 }
 // The rocket leans a few degrees toward the ringed planet instead of standing
 // bolt upright. Rotating at the pixel level keeps every primitive below (nose,
 // fins, hull shading, porthole, flame) exactly as drawn; the one-pixel gap
 // fill stops nearest-neighbour rounding from punching holes in the hull.
 struct TiltedSurface : Adafruit_GFX {
  Surface& target; float c,s; int ox,oy; int16_t last[64];
  TiltedSurface(Surface& surface,int px,int py,float radians)
    : Adafruit_GFX(64,64),target(surface),c(std::cos(radians)),
      s(std::sin(radians)),ox(px),oy(py) {
    for(int i=0;i<64;++i) last[i]=-1000;
  }
  void drawPixel(int16_t x,int16_t y,uint16_t col) override {
    const float dx=float(x-ox),dy=float(y-oy);
    const int tx=ox+int(std::lround(dx*c-dy*s));
    const int ty=oy+int(std::lround(dx*s+dy*c));
    if(tx<0||tx>=64||ty<0||ty>=64)return;
    int16_t& prev=last[ty];
    if(prev!=-1000&&tx>prev+1&&tx<=prev+3)
      for(int i=prev+1;i<tx;++i) target.drawPixel(i,ty,col);
    prev=int16_t(tx);
    target.drawPixel(tx,ty,col);
  }
 };
 void rocket(){
  // Roll around the longitudinal axis. The nose and exhaust stay on the body
  // axis; fins, hull highlight and porthole rotate around the cylinder. The
  // whole sprite is also canted 20 degrees toward the planet, pivoting at the
  // engine bell so the ship looks like it is steering at Saturn rather than
  // standing to attention.
  const int x=10,y=29;
  const float roll=float(tick()%240u)*6.2831853f/240.0f;
  TiltedSurface canvas(d,x,y+23,0.3491f);   // +20 degrees, nose to the right
  auto fins=[&](bool front){
    for(int i=0;i<3;++i) {
      const float angle=roll+float(i)*2.0943951f;
      const float depth=std::cos(angle),side=std::sin(angle);
      if((depth>=0)!=front) continue;
      const int root=x+int(std::lround(4*side));
      const int tip=x+int(std::lround(10*side));
      canvas.fillTriangle(root,y+14,tip,y+25,root,y+21,
        r.mix(0x644f53u,0xcd8b73u,int(145+depth*70)));
    }
  };
  fins(false);
  // Keep the silhouette fixed; only the light moves around the hull.
  canvas.fillTriangle(x,y,x-5,y+8,x+5,y+8,ink(0xc47d69));
  canvas.fillRoundRect(x-5,y+7,11,15,2,ink(0xb5c4c7));
  const int shine=x+int(std::lround(3*std::sin(roll)));
  for(int dx=-4;dx<=4;++dx) {
    const int level=std::clamp(240-std::abs(x+dx-shine)*24,70,240);
    canvas.drawFastVLine(x+dx,y+8,13,r.mix(0x617a8bu,0xe6e9d9u,level));
  }
  canvas.drawLine(x,y+1,shine,y+7,ink(0xe6b49a));
  canvas.drawFastHLine(x-4,y+17,9,ink(0xa36c5c));
  // Planet-shine: the ringed world sits up and to the right, so the hull picks
  // up one warm pixel along that edge. A shared light is what makes the ship
  // belong to the scene instead of sitting on top of it.
  for(int yy=y+9;yy<y+21;++yy) canvas.drawPixel(x+5,yy,ink(0xc79263));
  // One porthole: visible on the front half, foreshortened at the sides,
  // hidden on the back half. A second porthole never magically appears.
  const float front=std::cos(roll);
  if(front>0.08f) {
    const int px=x+int(std::lround(4*std::sin(roll)));
    const int width=std::max(1,int(std::lround(2*front)));
    canvas.fillEllipse(px,y+11,width,3,ink(0x7f939d));
    if(width>1) canvas.fillEllipse(px,y+11,width-1,2,ink(0x2c536f));
    else canvas.drawFastVLine(px,y+9,5,ink(0x2c536f));
    canvas.drawPixel(px,y+10,ink(0x9bc7cd));
  }
  fins(true);
  canvas.fillRect(x-3,y+22,7,2,ink(0x536775));
  const int flame=7+int(tick()%4u);
  canvas.fillTriangle(x-2,y+24,x+2,y+24,x,y+24+flame,ink(0xda9264));
  canvas.fillTriangle(x-1,y+24,x+1,y+24,x,y+29,ink(0xffdf9a));
 }
 void rings(){
  stars(107);
  const int cx=52,cy=31,rad=11;
  // A large cropped gas giant is the distant backdrop; the approved rocket
  // remains the near subject. Each band has a continuous shaded terminator.
  auto ring=[&](bool front){
    for(int i=0;i<56;++i){
      const float a=float(i)*6.2831853f/56.0f;
      if((std::sin(a)>0)!=front)continue;
      const float aa=float(i+1)*6.2831853f/56.0f;
      for(int band=0;band<3;++band){
        const float radius=18.0f-float(band)*2.0f;
        auto point=[&](float t,int& x,int& y){
          const float u=radius*std::cos(t),v=6*std::sin(t);
          x=cx+int(std::lround(u*0.94f-v*0.342f));
          y=cy+int(std::lround(u*0.342f+v*0.94f));
        };
        int x,y,xx,yy;point(a,x,y);point(aa,xx,yy);
        d.drawLine(x,y,xx,yy,ink(front?(band==1?0x84745f:0xc3a97b):0x514858));
      }
    }
  };
  ring(false);
  static constexpr uint32_t bands[]={0x978480,0xc8ad8c,0xa58d81,0xd2b992,0xb3937e};
  const int cloudShift=int(tick()/12u)%4;
  for(int y=cy-rad;y<=cy+rad;++y)for(int x=cx-rad;x<=cx+rad;++x){
    const int dx=x-cx,dy=y-cy;
    if(dx*dx+dy*dy>rad*rad)continue;
    const uint32_t base=bands[((y-cy+rad+cloudShift)/3)%5];
    d.drawPixel(x,y,r.mix(0x1a2235,base,std::clamp(220-dx*8-dy*2,36,250)));
  }
  ring(true);
  // Sparse near-field rocks flow through the lower-right, clear of the hull.
  // The stream lane runs 34..97, so both the exit and the wrap happen fully
  // off-screen; the first three columns draw only a dim incoming speck, so a
  // rock's re-entry never reads as a pop in the middle of the field.
  for(int i=0;i<12;++i){
    const uint32_t seed=r.skyHash(uint32_t(i)*977u+81u);
    const int lane=34+int((tick()/5u+uint32_t(i)*7u)%64u);
    const int y=43+int((seed>>8)%17u)+(lane-34)/11;
    if(lane<37){ p(lane,y,0x3f4852); continue; }
    const uint16_t rock=ink(i%3?0x6e6b69:0x9b8e78);
    d.drawFastHLine(lane,y,2+int(seed%2u),rock);
    d.drawPixel(lane+1,y-1,ink(0xa29882));
    if(i%2==0)d.drawPixel(lane+2,y+1,ink(0x3f4852));
  }
  // A distant probe crosses the gap between the ring and the foreground moon.
  const int probeX=34+int(std::lround(5*std::sin(tick()*0.045f)));
  const int probeY=40+int(std::lround(2*std::cos(tick()*0.035f)));
  d.fillRect(probeX,probeY,2,3,ink(0xb3c7cc));
  d.drawFastHLine(probeX-3,probeY+1,8,ink(0x6f7d87));
  d.fillRect(probeX-4,probeY,3,3,ink(0x96794f));
  d.fillRect(probeX+3,probeY,3,3,ink(0x96794f));
  if(tick()%26u<5u)p(probeX+1,probeY-1,0xe6cf8f);
  // Foreground teal moon: craters, a grazing rim and a slowly shifting light.
  const int mx=56+int(std::lround(std::sin(tick()*0.035f))),my=57;
  for(int y=my-9;y<=my+9;++y)for(int x=mx-9;x<=mx+9;++x){
    const int dx=x-mx,dy=y-my,q=dx*dx+dy*dy;
    if(q>81)continue;
    d.drawPixel(x,y,r.mix(0x172d3bu,0x669c98u,std::clamp(190-dx*13-dy*5,28,240)));
  }
  d.fillCircle(mx-3,my-2,2,ink(0x3c666d));
  d.drawPixel(mx-4,my-3,ink(0x8fb5aa));
  d.fillCircle(mx+2,my+3,1,ink(0x2b505d));
  rocket();
  hud();
 }
 void cabin(){
  // Pure forward flight, frameless: the star stream owns the whole panel
  // and the HUD chronometer sits on the vanishing point, so the view reads
  // as drifting through a system with the readout projected on the glass.
  // Streaming starfield: every star owns a fixed ray out of the vanishing
  // point and sweeps along it from near to far on a ~4s cycle, drawing a
  // short tail toward the vanishing point whose length grows with apparent
  // speed. Ray reach follows a power law — most stars stay in the dense
  // core, enough reach the rim that the outer ring is never empty — so the
  // field reads dense in the middle and present all the way to the edges.
  for(uint32_t i=0;i<150;++i){
    const uint32_t h=r.skyHash(i*977u+0x7134u);
    const float ang=float(h%628u)*0.01f;
    const float reach=45.0f*std::pow(float((h>>10)%256u)/255.0f,1.15f);
    const float s=float((tick()+i*7u)%40u)/40.0f;
    const float rad=reach*(0.12f+0.88f*s);
    const int x=32+int(std::lround(rad*std::cos(ang)));
    const int y=28+int(std::lround(rad*std::sin(ang)));
    if(x<0||x>63||y<0||y>63)continue;
    const int bright=70+int(70*(1+std::sin(tick()*0.045f+i))*0.5f);
    d.drawPixel(x,y,r.mix(0,0xb9d1d7u,bright));
    const int tail=1+int(rad/12);
    for(int k=1;k<=tail;++k){
      const int sx=x-int(std::lround((x-32)*k/10.0f)),sy=y-int(std::lround((y-28)*k/10.0f));
      if(sx>=0&&sx<64&&sy>=0&&sy<64)d.drawPixel(sx,sy,
        r.mix(0,0x7f99a5u,bright*(tail+1-k)/(tail+2)));
    }
  }
  // Rim stars: a sparse dedicated band whose rays all reach the outer edge,
  // so the corners are dotted too without thickening the dense core.
  for(uint32_t i=0;i<26;++i){
    const uint32_t h=r.skyHash(i*631u+0x29b7u);
    const float ang=float(h%628u)*0.01f;
    const float reach=40.0f+8.0f*float((h>>10)%256u)/255.0f;
    const float s=float((tick()+i*11u)%40u)/40.0f;
    const float rad=reach*(0.55f+0.45f*s);
    const int x=32+int(std::lround(rad*std::cos(ang)));
    const int y=28+int(std::lround(rad*std::sin(ang)));
    if(x<0||x>63||y<0||y>63)continue;
    const int bright=60+int(60*(1+std::sin(tick()*0.045f+i*1.7f))*0.5f);
    d.drawPixel(x,y,r.mix(0,0xb9d1d7u,bright));
    const int tail=1+int(rad/14);
    for(int k=1;k<=tail;++k){
      const int sx=x-int(std::lround((x-32)*k/12.0f)),sy=y-int(std::lround((y-28)*k/12.0f));
      if(sx>=0&&sx<64&&sy>=0&&sy<64)d.drawPixel(sx,sy,
        r.mix(0,0x7f99a5u,bright*(tail+1-k)/(tail+2)));
    }
  }
  // One meteor per ~9s: a warm speck with a short hot trail climbs across
  // the glass and is gone.
  {
    const uint32_t mt=tick()%90u;
    if(mt<12u){
      const float mp=float(mt)/12.0f;
      const int mx=14+int(std::lround(38.0f*mp));
      const int my=46-int(std::lround(34.0f*mp));
      for(int k=1;k<=5;++k)
        d.drawPixel(mx-k,my+int(std::lround(0.9f*k)),
          r.mix(0,0xd18d63u,180-28*k));
      p(mx,my,0xfff3cf);
    }
  }
  // Everything out here streams backward past the capsule — that is what
  // steady forward flight looks like from the cockpit. Three bodies ride
  // radial tracks away from the vanishing point. The distance easing is
  // deliberately sqrt-shaped (s^0.6): the boring far-away phase whips past
  // quickly while the big close pass lingers on screen, so the three
  // bodies' visible windows overlap and there is no empty stretch with
  // nothing but specks. Each dims to black only at the very end of its
  // pass, so the wrap back to a distant speck never pops.
  auto bodyPos=[&](uint32_t seed,uint32_t cycle,int baseR,
                   int&bx,int&by,int&rad,float&fade){
    const uint32_t h=r.skyHash(seed);
    const float ang=float(h%628u)*0.01f;
    const float s=float((tick()+uint32_t(h>>10u))%cycle)/float(cycle);
    const float dist=4.0f+52.0f*std::pow(s,0.6f);
    bx=32+int(std::lround(std::cos(ang)*dist));
    by=28+int(std::lround(std::sin(ang)*dist));
    rad=std::max(1,int(std::lround(baseR*(0.35f+s*1.8f))));
    // Fade in from a speck near the vanishing point, and fade back to black
    // at the end of the pass so the wrap never pops.
    fade=std::min(std::min(s/0.06f,(1.0f-s)/0.12f),1.0f);
  };
  auto ringArc=[&](int bx,int by,int rx,int ry,bool front,uint16_t c){
    for(int i=0;i<36;++i){
      const float a=float(i)*6.2831853f/36.0f;
      const float a2=float(i+1)*6.2831853f/36.0f;
      if((std::sin(a)>0)!=front)continue;
      d.drawLine(bx+int(std::lround(rx*std::cos(a))),by+int(std::lround(ry*std::sin(a))),
                 bx+int(std::lround(rx*std::cos(a2))),by+int(std::lround(ry*std::sin(a2))),c);
    }
  };
  // The ringed giant: banded disc, ring behind first, ring in front last.
  {int bx,by,rad;float fade;bodyPos(0x51e07u,300u,5,bx,by,rad,fade);
   const int rx=rad+4,ry=std::max(2,rad*2/5);
   ringArc(bx,by,rx,ry,false,r.mix(0,0x514858u,int(220*fade)));
   static constexpr uint32_t gb[]={0xc8ad8c,0xa58d81,0xd2b992};
   const int shift=int(tick()/14u)%3;
   for(int y=by-rad;y<=by+rad;++y)for(int x=bx-rad;x<=bx+rad;++x){
     const int dx=x-bx,dy=y-by;if(dx*dx+dy*dy>rad*rad)continue;
     d.drawPixel(x,y,r.mix(0x1a2235,gb[(dy+rad+shift)/2%3],
       int(std::clamp(210-dx*10-dy*3,40,235)*fade)));
   }
   ringArc(bx,by,rx,ry,true,r.mix(0,0xc3a97bu,int(235*fade)));}
  // The cratered moon, teal-grey like the ring scene's foreground world.
  {int bx,by,rad;float fade;bodyPos(0x62c10u,230u,4,bx,by,rad,fade);
   for(int y=by-rad;y<=by+rad;++y)for(int x=bx-rad;x<=bx+rad;++x){
     const int dx=x-bx,dy=y-by;if(dx*dx+dy*dy>rad*rad)continue;
     d.drawPixel(x,y,r.mix(0x172d3bu,0x669c98u,
       int(std::clamp(190-dx*12-dy*5,28,225)*fade)));
   }
   d.fillCircle(bx-rad/3,by-rad/3,std::max(1,rad/3),r.mix(0,0x3c666du,int(200*fade)));
   if(rad>3){p(bx-rad/3-1,by-rad/3-1,0x8fb5aa);d.fillCircle(bx+rad/3,by+rad/3,1,r.mix(0,0x2b505du,int(200*fade)));}}
  // The tumbling rock: a potato silhouette that slowly spins.
  {int bx,by,rad;float fade;bodyPos(0x73a29u,170u,3,bx,by,rad,fade);
   const float tumble=float(tick())*0.11f;
   for(int y=by-rad-1;y<=by+rad+1;++y)for(int x=bx-rad-1;x<=bx+rad+1;++x){
     const int dx=x-bx,dy=y-by;
     const float rr=float(rad)*(0.82f+0.22f*std::sin(std::atan2(float(dy),float(dx))*2.0f+tumble));
     if(float(dx*dx+dy*dy)>rr*rr)continue;
     d.drawPixel(x,y,r.mix(0x232b36u,0x8b8578u,
       int(std::clamp(180-dx*14-dy*8,40,220)*fade)));
   }}
  const auto tm=r.local();
  auto holo=[&](int phase){return r.mix(0x9fbbff,0xc4f3ed,
    140+int(90*std::sin(tick()*0.018f+phase*0.5f)));};
  const uint16_t holoDim=r.mix(0x0b1220u,0x9fbbffu,150);
  {
    // Bottom-centre HUD chronometer: the digits ride the lower edge of the
    // stream, clear of the vanishing point and the passing bodies. Colon
    // blinks on whole seconds; a lone dot to the right sweeps once per
    // minute as the quiet second hand. Top-right stays clear for the link
    // indicator.
    if(tm.valid){
      const uint16_t hc=holo(0);
      r.digit4x7(21,56,tm.hour/10,hc);r.digit4x7(26,56,tm.hour%10,hc);
      if(tm.second%2==0){d.drawPixel(31,58,holo(2));d.drawPixel(31,61,holo(2));}
      else{d.drawPixel(31,58,holoDim);d.drawPixel(31,61,holoDim);}
      r.digit4x7(33,56,tm.minute/10,holo(3));r.digit4x7(38,56,tm.minute%10,holo(4));
      const float sa=float(r.c.uptime()%60000u)*6.2831853f/60000.0f-1.5707963f;
      p(46+int(std::lround(2*std::cos(sa))),59+int(std::lround(2*std::sin(sa))),0xc4f3ed);
    }else{
      for(int x:{21,26,33,38})r.digit4x7(x,56,0,holoDim);
    }
  }
 }
 void draw(int scene){d.fillScreen(0);r.latchNight();if(scene==0)orbit();else if(scene==1)moonbase();else if(scene==2)rings();else cabin();}

};
  // Room variants retain the big scale-2 HH:MM hero with a seconds
  // sweep, and the cozy scene compressed into the lower band. Pure black
  // background; day/night mood comes from the window and lamp only.
  void drawLoft() {
    const uint8_t scene=v.loftScenePreview>=0?uint8_t(v.loftScenePreview):loftScene(loftDay());
    if(scene%2u==1u) {
      SpaceScene{d,*this}.draw((scene-1)/2);
      return;
    }
    Local tm=local();bool night=!tm.valid||tm.hour<7||tm.hour>=18;
    // Date strip beside the link light.
    const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
    char date[13];
    if(tm.valid)snprintf(date,sizeof(date),"%s %02d/%02d",days[tm.weekday],tm.month,tm.day);
    else snprintf(date,sizeof(date),"TIME UNSYNC");
    micro(3,3,date,color(night?0x8fa3d8:0x8fd0c5));

    // Hero clock: 5x9 digits at scale 2, shadowed like the quota number.
    if(tm.valid) {
      const uint32_t inkHex = loftClockInk() ? loftClockInk() : t.clockColor;
      uint16_t ink=color(inkHex),hole=color(0x11152c);
      digit(7,11,tm.hour/10,2,hole);digit(19,11,tm.hour%10,2,hole);
      digit(35,11,tm.minute/10,2,hole);digit(47,11,tm.minute%10,2,hole);
      digit(6,10,tm.hour/10,2,ink);digit(18,10,tm.hour%10,2,ink);
      digit(34,10,tm.minute/10,2,ink);digit(46,10,tm.minute%10,2,ink);
      // The gap between digit cells is x=28..33; center both blink phases.
      if(tm.second%2==0) { d.fillRect(30,14,2,2,ink); d.fillRect(30,21,2,2,ink); }
      else d.fillRect(30,18,2,1,mix(0x111a2eu,inkHex,120));
      // Seconds sweep along the bottom of the hero: one pixel per second.
      d.drawFastHLine(3,29,58,color(0x272d4d));
      if(tm.second>0) d.drawFastHLine(3,29,tm.second,mix(0x20263fu,t.accentColor,150));
    } else {
      for(int x:{6,18,34,46})d.fillRect(x,18,10,2,color(0x425466));
    }

    // ---- the room is deterministic --------------------------------------
    // Three clocks keep the scene from reading like a screensaver:
    //   slot  -> wall art, desk prop  (30 min)
    //   day   -> weather + moon phase             (overnight)
    //   frame -> clouds / rain / plane / fish / flame
    // Nothing calls random(): the browser preview and the panel must agree.
    const uint32_t day=loftDay();
    const uint32_t slot=c.uptime()/1800000u;
    const uint32_t seed=skyHash(slot*0x85ebca6bu+day*0x27d4eb2fu+0x1b873593u);
    const uint8_t weather=loftWeather();
    switch(scene) {
      case 2: loftCafeRoom(night,weather,slot); break;
      case 4: loftStudyRoom(night,weather,slot,day); break;
      case 6: loftNookRoom(night,weather,slot); break;
      default: loftAtticRoom(night,weather,slot,seed,tm.valid?tm.hour:0); break;
    }
  }
  // Which room the lower band shows. It is picked when the page is entered —
  // the same rhythm as the hero clock's hue — and then held: the room must
  // never change while you are looking at it. A remembered last pick keeps two
  // visits in a row off the same room, which read as "the scene never changes".
  uint8_t loftScene(uint32_t /*day*/) const {
    // Advance only on a new visit, never on redraw or a new date/half-hour.
    // Four rooms alternate with four space views. These are variants of
    // LOFT_PAGE, not new absolute pages or extra canvases.
    auto& seenAt=loftCycle().seenAt;
    if(v.pageShownAt&&v.pageShownAt!=seenAt) {
      seenAt=v.pageShownAt;
      loftSceneIndex()=uint8_t((loftSceneIndex()+1u)%8u);
    }
    return loftSceneIndex();
  }
  // 1 · attic: the original corner — floor lamp, reading desk, cat pacing.
  void loftAtticRoom(bool night,uint8_t weather,uint32_t slot,uint32_t seed,int hour) {
    drawWindow(42,33,19,18,WIN_CROSS,color(0x8b6355),night,weather,slot);
    // The taller fishbowl needs headroom. A light string fits above it;
    // a framed picture down to y41 would collide with the rim at y40.
    // The desk is pinned to the fishbowl, so the wall above it is pinned to
    // the light string too: no picture frame swapping in and out over a bowl.
    if(t.sceneDetail>=2) drawWallArt(4,night);
    d.fillRect(39,52,25,2,color(0xba795d));         // sill
    // Floor lamp (left): accent shade, warm glow at night.
    d.fillTriangle(7,33,1,42,13,42,color(t.accentColor));
    d.fillRect(5,42,5,2,color(0xe7ae6c));
    d.fillRect(6,44,2,14,color(0x694439));
    d.fillRect(3,58,8,2,color(0x694439));
    if(night) {
      const uint16_t glow=mix(0,0xffca7e,54+int(8*std::sin(frame()*0.045f)));
      d.drawFastHLine(3,44,9,glow);
      d.drawPixel(5,45,glow);d.drawPixel(9,45,glow);
    }
    drawDesk(night,weather,hour,seed);
    d.drawFastHLine(0,61,64,color(night?0x2c3247:0x39434f));
    catProwl(54);
  }
  // 2 · café: arched dormer window on the left, counter on the right, the cat
  // sitting up on the bar and the plant swaying next to the cup.
  void loftCafeRoom(bool night,uint8_t weather,uint32_t slot) {
    drawWindow(3,33,26,20,WIN_ARCH,color(0x46564e),night,weather,slot);
    d.fillRect(1,55,30,2,color(night?0x6a4a3e:0x8a6250));   // sill
    // Pendant over the counter: cord, shade, and a cone of light at night.
    d.fillRect(39,31,1,3,color(0x3a3a42));
    d.fillTriangle(34,39,44,39,39,33,color(night?0x5a4a34:0xc9a26a));
    d.fillRect(33,39,13,2,color(0xe7ae6c));
    d.drawPixel(39,41,color(0xfff0c0));
    if(night) {
      const uint16_t glow=mix(0,0xffca7e,65+int(8*std::sin(frame()*0.04f)));
      d.fillRect(38,41,3,1,glow);
      d.drawFastHLine(35,45,9,glow);
    }
    // Counter: top, shadow, cabinet front with plank lines.
    const uint16_t top=color(night?0x7a4e3e:0xa87258);
    d.fillRect(33,46,31,3,top);
    d.fillRect(33,49,31,1,color(night?0x3a2620:0x5a3a2e));
    // An open counter, with a small drawer and two legs. Keep the space
    // beneath it black rather than filling the corner with a blue slab.
    d.fillRect(43,50,18,4,color(night?0x49332d:0x705044));
    d.drawFastHLine(45,50,14,color(night?0x765345:0x9a7058));
    d.drawFastHLine(50,52,4,color(0xbda17c));
    d.fillRect(43,54,2,7,color(0x694439));
    d.fillRect(60,54,2,7,color(0x694439));
    // Coffee cup with two threads of steam, then a small potted plant.
    d.fillRect(36,43,3,3,color(0xe8eef2));
    d.drawPixel(39,44,color(0xe8eef2));
    d.fillRect(35,46,5,1,color(0xc8ccd4));
    if(t.sceneDetail>=2) roomSteam(37,42,0);
    d.fillRect(46,44,5,3,color(0xb06a4a));           // pot
    d.fillRect(46,44,5,1,color(0xc97f5c));
    // Three leaves, not three stems: 1 px lines read as spring onions.
    const int sway=int(frame()/9u)%3-1;
    d.drawPixel(47+sway,41,color(0x63c48a));d.drawPixel(47+sway,42,color(0x3f9f6d));
    d.drawPixel(48+sway,40,color(0x3f9f6d));d.drawPixel(48+sway,41,color(0x63c48a));
    d.drawPixel(49,42,color(0x63c48a));d.drawPixel(49,43,color(0x3f9f6d));
    d.drawPixel(46,43,color(0x3f9f6d));
    // Stool in front of the counter, then the cat up on the far end.
    d.fillEllipse(36,52,4,1,color(night?0x6a4a3e:0x9a6a52));
    d.fillRect(36,53,1,8,color(0x694439));
    d.fillRect(34,58,5,1,color(0x5a3a2e));
    d.fillRect(33,61,7,1,color(0x694439));
    d.drawFastHLine(0,61,33,color(night?0x2c3247:0x39434f));
    catSit(51,46);
  }
  // 3 · study: tall three-pane window on the left, book wall, armchair with a
  // blanket over the arm and the cat curled on the cushion.
  void loftStudyRoom(bool night,uint8_t weather,uint32_t slot,uint32_t day) {
    drawWindow(2,33,16,22,WIN_SASH,color(0x6b4a3c),night,weather,slot);
    // Bookcase. Spines come off the calendar day, so the shelf looks restocked
    // tomorrow and never reshuffles while you watch it.
    const uint16_t wood=color(night?0x4a352c:0x6a4a3a);
    // Two open shelves: grouped books, a leaning spine and a horizontal
    // stack make a bookcase, rather than a solid grid of coloured squares.
    d.fillRect(23,32,18,1,wood);
    d.fillRect(23,40,18,1,wood);
    d.fillRect(23,48,18,2,wood);
    d.fillRect(23,33,1,15,wood);
    d.fillRect(40,33,1,15,wood);
    static constexpr uint32_t spines[]={0x9a5a4e,0x4e7a96,0xb08a4a,0x5e9a74,0x7a6a9a,0xa8705a};
    for(int s=0;s<2;++s) {
      const int deck=40+s*8;
      for(int i=0;i<4;++i) {
        const uint32_t b=skyHash(day*13u+uint32_t(s*37+i*7));
        const int bh=4+int(b%3);
        const int bx=25+i*3;
        d.fillRect(bx,deck-bh,2,bh,color(spines[(b>>5)%6]));
        if(i==1) d.drawPixel(bx,deck-2,color(0xd4b98b));
      }
      d.drawLine(37,deck-5,39,deck-1,color(spines[(day+s)%6]));
    }
    // Armchair: back, cushion, near arm, two short legs. At this size the
    // height difference between back and arm is what reads as "chair".
    const uint16_t cloth=color(night?0x33465a:0x466072);
    const uint16_t seat=color(night?0x5a7080:0x8aa8b8);
    d.fillRoundRect(46,40,15,15,3,cloth);
    d.drawFastHLine(49,41,9,color(night?0x506477:0x6b8594));
    d.fillRoundRect(44,53,18,4,1,seat);
    d.fillRect(43,49,3,8,color(night?0x2c3d4e:0x3e5464));
    d.fillRect(60,49,3,8,color(night?0x2c3d4e:0x3e5464));
    d.fillRect(46,57,2,3,color(0x5a3a2e));
    d.fillRect(58,57,2,3,color(0x5a3a2e));
    // A thrown blanket, the one warm accent in a blue room.
    d.fillRect(43,49,3,5,color(0xc9785f));
    d.drawPixel(44,54,color(0xe2a17d));
    // A little reading table, with a book left open beside the chair.
    d.fillRect(26,54,13,2,color(night?0x80513d:0xb07855));
    d.fillRect(28,56,1,5,wood);d.fillRect(36,56,1,5,wood);
    d.fillRect(28,51,8,3,color(0xd3b98c));
    d.drawFastVLine(32,51,3,color(0x856549));
    // Rug and floor.
    d.drawFastHLine(42,60,22,color(night?0x55404a:0x8a6a72));
    d.drawFastHLine(0,61,64,color(night?0x2c3247:0x39434f));
    catCurl(47,51);
  }
  // 4 · nook: round porthole on the right, low table and paper lamp on matting
  // — the late-night corner of the same flat.
  void loftNookRoom(bool night,uint8_t weather,uint32_t slot) {
    drawWindow(44,33,18,18,WIN_ROUND,color(0x7a5a44),night,weather,slot);
    // A hanging scroll: without it the wall above the table reads as empty.
    d.fillRect(15,32,11,1,color(night?0x4a3a30:0x7a5a44));   // rod
    d.fillRect(16,33,9,11,color(night?0x2a241c:0x3a3228));   // mount
    d.fillRect(17,34,7,9,color(night?0x8a7a62:0xc9b898));    // paper
    d.drawCircle(20,39,2,color(0x4a3a30));                   // ensō
    d.drawPixel(22,42,color(0x8a4a3a));                      // seal
    // Matting, then the low table with short legs.
    d.drawFastHLine(3,60,39,color(night?0x61443a:0x9a7058));
    d.drawFastHLine(4,59,2,color(night?0x45342b:0x6a5040));
    d.drawFastHLine(39,59,2,color(night?0x45342b:0x6a5040));
    const uint16_t wood=color(night?0x8a5a44:0xb87a58);
    d.fillRect(7,48,24,3,wood);
    d.fillRect(7,48,24,1,color(night?0x9a6a50:0xd09468));
    d.fillRect(11,51,2,5,color(0x5a3a2e));
    d.fillRect(28,51,2,5,color(0x5a3a2e));
    // Tea set, a book stack, one cushion.
    d.fillRect(23,45,4,3,color(0xe8eef2));
    d.drawPixel(27,46,color(0xe8eef2));
    d.fillRect(22,48,6,1,color(0xc8ccd4));
    d.fillRect(9,45,7,3,color(0xb06a5a));
    d.fillRect(10,43,6,2,color(0x6ea3c9));
    d.fillEllipse(16,57,10,2,color(night?0x58363d:0x94556a));
    if(t.sceneDetail>=2) {
      roomSteam(24,44,11);roomSteam(26,44,37);
    }
    // Paper lamp: rounded washi column, two faint ribs, warm at night. Three
    // hard ribs plus a bare block read as a ladder in the earlier pass.
    d.fillRect(36,51,2,5,color(0x5a3a2e));
    d.fillRect(33,56,9,2,color(0x78513b));
    const int pulse=night?int(8*std::sin(frame()*0.06f)):0;
    const uint32_t lampPhase=frame()%100u;
    const int sway=lampPhase<25u?0:(lampPhase<50u?1:(lampPhase<75u?0:-1));
    d.fillRoundRect(34+sway,37,7,15,2,mix(0xbba174u,0xffe6b2,night?190+pulse:155));
    d.drawFastVLine(35+sway,39,10,color(0xbca17a));
    d.drawFastHLine(35+sway,37,5,color(0xc9a878));
    d.drawFastHLine(34+sway,44,7,color(0xc9b087));
    d.drawFastHLine(35+sway,51,5,color(0x9a7d58));
    if(night) {
      d.drawFastHLine(32,58,11,mix(0,0xffd9a0,40+pulse));
    }
    d.drawFastHLine(0,61,64,color(night?0x2c3247:0x39434f));
    catCurl(10,56);
    // A brief ear-tip twitch; sleeping body, head and paws do not resize.
    if(frame()%160u>=100u&&frame()%160u<104u) {
      d.drawPixel(21,53,color(0xf5e6c8));
      d.drawPixel(21,54,color(0xd0b894));
    }
  }
  // Steam rises in small wisps, then disappears before the next wisp starts.
  // Independent phases keep the two cups from puffing in lockstep.
  void roomSteam(int x,int y,uint32_t phase) {
    const uint32_t f=(frame()+phase)%56u;
    if(f>=40u) return;
    const int rise=int(f/8u);
    const int sway=rise>=2?1:0;
    const int fade=180-int(f)*3;
    d.drawPixel(x+sway,y-rise,mix(0,0xc7d2de,fade));
    if(f>=8u&&f<32u) d.drawPixel(x+1,y-rise+1,mix(0,0xaebac9,fade/2));
  }
  // Sitting cat for the café counter: brief blinks and ear twitches, with
  // long rests between them. The paws always stay planted on the counter.
  void catSit(int x,int y) {                       // y = bottom of the paws
    const uint16_t fur=color(0xf5e6c8),deep=color(0xd0b894),pink=color(0xe8837e),ink=color(0x4a2c1f);
    d.fillEllipse(x+4,y-3,4,3,fur);
    d.fillRect(x+1,y-1,7,1,fur);
    d.fillRect(x+2,y-1,1,1,deep);d.fillRect(x+6,y-1,1,1,deep);  // paws
    d.fillCircle(x+4,y-8,2,fur);
    d.fillTriangle(x+2,y-9,x+3,y-11,x+4,y-9,fur);               // ears
    d.fillTriangle(x+5,y-9,x+6,y-11,x+7,y-9,fur);
    const uint32_t blink=frame()%83u;
    const bool closed=blink>=69u&&blink<72u;
    d.drawPixel(x+3,y-8,closed?deep:ink);d.drawPixel(x+5,y-8,closed?deep:ink);
    if(frame()%127u>=119u&&frame()%127u<122u) {
      d.drawPixel(x+6,y-11,0);d.drawPixel(x+7,y-10,deep);
    }
    d.drawPixel(x+4,y-7,pink);
    const uint32_t tail=frame()%97u;
    const int sway=tail<65u?0:(tail<75u?1:(tail<86u?0:-1));
    d.drawLine(x+8,y-1,x+10+sway,y-2,deep);
    d.drawLine(x+10+sway,y-2,x+11+sway,y-4,deep);
  }
  // Curled cat: a breathing oval with the tail ticking. Used on the cushion
  // and on the matting.
  void catCurl(int x,int y) {
    const uint16_t fur=color(0xf5e6c8),deep=color(0xd0b894),pink=color(0xe8837e),ink=color(0x4a2c1f);
    // Fixed silhouette: no radius changes or whole-body bobbing. The chest
    // highlight moves one row inside the body over a quiet five-second breath.
    d.fillEllipse(x+5,y,5,2,fur);
    d.fillEllipse(x+9,y-1,3,2,fur);
    d.drawPixel(x+11,y-3,deep);d.drawPixel(x+10,y-3,deep);
    d.drawFastHLine(x+9,y-1,2,ink);             // closed eye
    d.drawPixel(x+12,y,pink);                  // nose tucked to the paws
    d.drawFastHLine(x+8,y+1,4,deep);            // folded front paws
    d.drawLine(x,y+1,x+2,y+2,deep);
    d.drawFastHLine(x+2,y+2,4,deep);            // tail curled around the body
    d.drawLine(x+6,y+2,x+7,y,deep);
    const float breath=(1.0f-std::cos(float(frame()%52u)*6.2831853f/52.0f))*0.5f;
    const uint16_t chest=mix(0xd0b894u,0xf5e6c8u,185-int(breath*30));
    d.drawFastHLine(x+3,y+(breath>0.6f?-1:0),3,chest);
    // A tiny, occasional sleeping mark; the animal itself stays still.
    const uint32_t ph=(frame()+uint32_t(x)*7u)%237u;
    if(ph<28u) {
      const int zy=y-6-int(ph/14u);
      d.drawFastHLine(x+10,zy,2,mix(0,0xc4b69bu,135));
      d.drawFastHLine(x+10,zy+2,2,mix(0,0xc4b69bu,135));
      d.drawPixel(x+11,zy+1,mix(0,0xc4b69bu,135));
    }
  }
  // Local calendar day. The renderer only carries epoch + zone offset, and a
  // day is all the moon and the weather need.
  uint32_t loftDay() const {
    if(!c.hasClock) return 0;
    const int64_t e=c.epoch()+c.timezoneOffset;
    return uint32_t(e<0?0:e/86400);
  }
  // 0 clear, 1 cloudy, 2 rain, 3 snow, 4 fog. Decorative, not a forecast:
  // it is a hash of the day, so every panel in the same room agrees and the
  // window changes character overnight without any network call.
  uint8_t loftWeather() const {
    return uint8_t(skyHash(loftDay()*0x9e3779b9u+0x7f4a7c15u)%5u);
  }
  // Eight phases off the synodic month, referenced to the 2000-01-06 new moon.
  // Day resolution is plenty: the window can only show eight faces.
  uint8_t loftMoon() const {
    if(!c.hasClock) return 4;
    constexpr double synodic=29.530588853;
    const double age=std::fmod(double(loftDay())-10962.0,synodic);
    const double norm=age<0?age+synodic:age;
    return uint8_t(int(norm/synodic*8.0+0.5)&7);
  }
  // Four window frames, one window. Everything inside the glass — sky colour,
  // phase, drifting cloud, plane, skyline, rain/snow/fog — is written once and
  // shared: a room may move or reshape its window, never what the window
  // shows. That is the whole point of the rewrite: the furniture changes, the
  // sky keeps its rules.
  enum : uint8_t { WIN_CROSS=0, WIN_ARCH=1, WIN_SASH=2, WIN_ROUND=3 };

  static bool inGlass(uint8_t style,int X,int Y,int W,int H,int x,int y) {
    if(x<X||x>=X+W||y<Y||y>=Y+H) return false;
    if(style==WIN_ARCH) {
      const int arch=std::min(H/3+1,7);
      if(y<Y+arch) {
        // A parabola is a quarter-round at this size and costs one multiply:
        // straight 45° shoulders made the window read as a factory roof.
        const int u=Y+arch-y;
        const int inset=(u*u)/arch;
        return x>=X+inset&&x<X+W-inset;
      }
    } else if(style==WIN_ROUND) {
      const int dx=x-(X+W/2),dy=y-(Y+H/2);
      const int r=W/2;
      return dx*dx+dy*dy<=r*r;
    }
    return true;
  }

  void drawWindow(int X,int Y,int W,int H,uint8_t style,uint16_t frameInk,
                  bool night,uint8_t weather,uint32_t slot) {
    auto glass=[&](int x,int y){ return inGlass(style,X,Y,W,H,x,y); };
    // Weather owns the sky colour, otherwise rain falls out of a clear blue
    // afternoon and reads as dirt on the glass.
    const uint32_t skyTop=weather>=2?(night?0x242c44u:0x6f8496u)
                        : weather==1?(night?0x232c48u:0x5f88b0u)
                        : (night?t.nightColor:0x3d8fd0u);
    const uint32_t skyLow=weather>=2?(night?0x2c3652u:0x9aa9b6u)
                        : weather==1?(night?0x2c3a5eu:0x9fc4d8u)
                        : (night?0x2a4a86u:0x9fd8f5u);
    auto skyAt=[&](int y)->uint16_t{
      const int k=std::clamp((y-Y)*255/std::max(1,H),0,255);
      return mix(skyTop,skyLow,k);
    };
    for(int y=Y;y<Y+H;++y)d.drawFastHLine(X,y,W,skyAt(y));

    // Same celestial phase, different framing. Size the disc to the actual
    // upper pane; a porthole or narrow sash needs fewer pixels than an arch.
    const int mx=(style==WIN_ROUND)?X+W/2:(W<17?X+W/2:X+W*3/4);
    const int radius=(W<18||style==WIN_ROUND)?2:3;
    const int my=Y+std::max(radius+1,H/4);
    const uint8_t phase=loftMoon();
    const uint16_t celestial=mix(night?0xffe9a8u:0xffd257u,skyTop,weather>=2?150:0);
    if(night) {
      static constexpr int8_t edge[8]={3,2,0,-2,-3,2,0,-2};
      for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx) {
        if(dx*dx+dy*dy>radius*radius||!glass(mx+dx,my+dy)) continue;
        const int cut=edge[phase]*radius/3;
        const bool lit=phase!=0&&(phase<=4?dx>=cut:dx<=cut);
        d.drawPixel(mx+dx,my+dy,lit?celestial:mix(skyTop,0x647194u,40));
      }
    } else {
      for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx)
        if(dx*dx+dy*dy<=radius*radius&&glass(mx+dx,my+dy))
          d.drawPixel(mx+dx,my+dy,celestial);
    }

    if(t.sceneDetail>=2) {
      const bool overcast=weather>=1;
      const uint16_t cloudInk=color(night?(overcast?0x4a5878:0x5d6e94)
                                        :(overcast?0xc8d6e0:0xeaf4ff));
      const int count=weather==0?1:2;
      for(int i=0;i<count;++i) {
        const uint32_t r=skyHash(slot*31u+uint32_t(i)*977u);
        const int w=std::clamp((6+int((r>>4)%4))*W/22,5,11);
        const int travel=W+w+2;
        const int ccx=X-w+int((frame()/7+uint32_t((r>>9)%23))%uint32_t(travel));
        const int ccy=Y+2+int((r>>16)%2)+i*3;
        // Draw through the glass mask, including the curved shoulders. Each
        // cloud has a soft lower edge rather than a solid horizontal brick.
        for(int yy=0;yy<3;++yy) {
          const int inset=yy==0?2:(yy==1?0:1);
          for(int xx=inset;xx<w-inset;++xx)
            if(glass(ccx+xx,ccy+yy)) d.drawPixel(ccx+xx,ccy+yy,
              yy==2?mix(skyAt(ccy+yy),cloudInk,150):cloudInk);
        }
      }
      if(t.sceneDetail>=3) drawPlane(style,X,Y,W,H);
    }

    // One world, three views: a grounded neighbourhood, a hill cottage,
    // and the opposite bank of a river. Weather and celestial time stay shared.
    if(t.sceneDetail>=2) {
      const int landscape=style==WIN_ARCH?1:(style==WIN_SASH?2:0);
      auto put=[&](int x,int y,uint16_t ink){ if(glass(x,y)) d.drawPixel(x,y,ink); };
      auto fill=[&](int x,int y,int w,int h,uint16_t ink){
        for(int yy=y;yy<y+h;++yy) for(int xx=x;xx<x+w;++xx) put(xx,yy,ink);
      };
      auto lamp=[&](int x,int y,int id){
        if(!night) return;
        const uint32_t r=skyHash(uint32_t(id)*977u+slot*31u);
        const uint32_t period=14000u+r%11000u;
        const uint32_t ph=(c.uptime()+r%period)%period;
        const int level=ph<1600u?int(ph*150u/1600u):
          ph<period*2u/3u?150:ph<period*2u/3u+1800u?
          150-int((ph-period*2u/3u)*150u/1800u):0;
        const int steady=(r%4u)==0u?85:0;
        put(x,y,mix(night?0x25364au:0x49697au,0xffc884u,steady?steady:level));
      };
      if(landscape==0) {
        const int ground=Y+H;
        fill(X,ground-2,W,2,color(night?0x202e40:0x466273));
        const int count=W<18?2:3;
        for(int i=0;i<count;++i) {
          const int bx=X+1+i*W/count,bw=std::max(3,W/count-2);
          const int top=Y+H*(i==1?49:63)/100;
          fill(bx,top,bw,ground-top,color(night?0x26374f:0x496b80));
          fill(bx,top,bw,1,color(night?0x4a5a73:0x7696a6));
          if(i==1) fill(bx+bw/2,top-1,1,1,color(night?0x4a5a73:0x7696a6));
          for(int yy=top+2;yy<ground-1;yy+=3)
            for(int xx=bx+1;xx<bx+bw-1;xx+=2) lamp(xx,yy,i*113+(yy-Y)*7+xx-X);
        }
      } else if(landscape==1) {
        // Distant ridge, wooded mid-slope, then the garden and its path.
        for(int x=X;x<X+W;++x) {
          const int far=Y+H/2+std::abs(x-X-W/3)/3;
          const int mid=Y+H*2/3+std::abs(x-X-W*4/5)/6;
          const int near=Y+H*4/5+(x-X-W/2)/7;
          for(int y=far;y<Y+H;++y) put(x,y,color(night?0x263e59:0x7b9da4));
          if(weather==3) put(x,far,color(night?0x708396:0xc2d4db));
          for(int y=mid;y<Y+H;++y) put(x,y,color(night?0x294d50:0x5a8370));
          for(int y=near;y<Y+H;++y) put(x,y,color(night?0x263c37:0x466e50));
        }
        const int hx=X+W*2/3,hy=Y+H*4/5+(W*2/3-W/2)/7;
        // Garden path narrows toward the door and reaches the window sill.
        for(int y=hy;y<Y+H;++y) {
          const int cx=hx-1-(y-hy)/2;
          fill(cx,y,1+(y-hy)/3,1,color(night?0x6f6752:0xb7ae82));
        }
        fill(hx-4,hy-4,8,5,color(night?0x715440:0xc3a17b));
        for(int row=0;row<3;++row) fill(hx-row-2,hy-7+row,2*row+4,1,color(night?0x65484a:0xa86d58));
        fill(hx-3,hy-8,1,3,color(night?0x795a51:0x9d7d69));
        fill(hx-1,hy-2,2,3,color(night?0x403833:0x796047));
        lamp(hx-3,hy-3,401);lamp(hx+2,hy-3,402);
        if(W>=22) {
          // Smoke drifts above the chimney, with a pause between wisps.
          const uint32_t ph=frame()%79u;
          if(ph<48u) {
            const int rise=int(ph/12u),drift=rise/2;
            put(hx-3+drift,hy-9-rise,mix(0,night?0x8c9aabu:0xc8d4d8u,130));
            if(ph>12u) put(hx-4+drift,hy-8-rise,mix(0,night?0x8c9aabu:0xc8d4d8u,75));
          }
        }
        // Leafy trees instead of a bare trunk; only the outermost leaf moves.
        const int trees=W>=22?2:1;
        for(int i=0;i<trees;++i) {
          const int tx=X+3+i*5,ty=Y+H-2-i;
          fill(tx,ty-3,1,4,color(0x665343));
          fill(tx-2,ty-5,5,2,color(night?0x456959:0x568553));
          fill(tx-1,ty-6,3,1,color(night?0x507362:0x719b65));
          const int sway=(frame()+uint32_t(i)*17u)%121u>101u?1:0;
          put(tx-2+sway,ty-4,color(night?0x3b5d4f:0x477949));
          if(weather==3) fill(tx-1,ty-6,3,1,color(night?0x7b9190:0xcbded5));
        }
        // A short picket fence and a few garden flowers scale out on tiny panes.
        if(W>=22) {
          fill(X+1,Y+H-2,8,1,color(night?0x796954:0xb7a27b));
          for(int x=X+1;x<X+9;x+=3) fill(x,Y+H-3,1,3,color(night?0x796954:0xb7a27b));
          put(hx+5,hy+2,color(night?0x9a8160:0xe3ba79));
          put(hx+3,hy+3,color(night?0x8c6b71:0xc794a1));
        }
      } else {
        const int bank=Y+H*3/4;
        fill(X,bank,W,Y+H-bank,color(night?0x1a3b54:0x508da4));
        for(int i=0;i<2;++i) {
          const int bx=X+2+i*W/2,bw=std::max(3,W/3-1),top=bank-3-i;
          fill(bx,top,bw,bank-top,color(night?0x33445a:0x6d8190));
          fill(bx,top,bw,1,color(night?0x53627a:0x95a6ad));
          lamp(bx+1,top+1,610+i*47);
        }
        fill(X,bank-1,W,1,color(night?0x4a5967:0x8a998b));
        for(int y=bank+1;y<Y+H;y+=2) {
          const int drift=int((frame()/9u+uint32_t(y))%3u)-1;
          const int x=X+W/2+drift;
          fill(x-(y-bank)/3,y,2+(y-bank)/3,1,color(night?0x728998:0x9ccbd2));
          if(night) put(X+3+drift,y,mix(0x1a3b54u,0xffc884u,100));
        }
      }
    }

    // Weather in front of the city: rain streaks, drifting snow, fog banks.
    // Rain and snow never paint over the moon: a drop on the disc reads as a
    // dead pixel, not as weather in front of it.
    auto inMoon=[&](int x,int y){ return std::abs(x-mx)<=radius&&std::abs(y-my)<=radius; };
    if(t.sceneDetail>=2&&weather==2) {
      const uint16_t drop=color(0x86b4e6);
      const int n=std::max(3,W*H/85);
      for(int i=0;i<n;++i) {
        const int y=Y+2+int((frame()/2+uint32_t(i)*5u)%uint32_t(std::max(1,H-2)));
        const int x=X+3+int((uint32_t(i)*7u+uint32_t(y)*3u)%uint32_t(std::max(1,W-3)));
        if(!glass(x,y)||inMoon(x,y)) continue;
        d.drawPixel(x,y,drop);
        if(glass(x-1,y+1)) d.drawPixel(x-1,y+1,mix(0x24406cu,drop,200));
        if(glass(x-2,y+2)) d.drawPixel(x-2,y+2,mix(0x1e3a62u,drop,150));
      }
    } else if(t.sceneDetail>=2&&weather==3) {
      const int n=std::max(4,W*H/55);
      for(int i=0;i<n;++i) {
        const int y=Y+int((frame()/3+uint32_t(i)*4u)%uint32_t(std::max(1,H)));
        const int x=X+int((uint32_t(i)*6u+frame()/9)%uint32_t(std::max(1,W)));
        if(!glass(x,y)||inMoon(x,y)) continue;
        d.drawPixel(x,y,color(0xf2f8ff));
        if(i%4==0&&glass(x,y+1)) d.drawPixel(x,y+1,mix(0x7f9fc8u,0xf2f8ff,190));
      }
    } else if(t.sceneDetail>=2&&weather==4) {
      for(int i=0;i<3;++i) {
        const int y=Y+3+i*std::max(3,H/4);
        if(y>=Y+H) break;
        int a=-1,b=-1;
        for(int x=X;x<X+W;++x) if(glass(x,y)) { if(a<0)a=x; b=x; }
        if(a<0) continue;
        const int off=int((frame()/6+uint32_t(i)*5u)%8u);
        const int s0=a+off, s1=b-off/2;
        if(s1>s0) d.drawFastHLine(s0,y,s1-s0,mix(skyAt(y),0xdfe8ff,night?70:120));
      }
    }

    // Mask: everything outside the glass goes back to black, so an arch or a
    // porthole is exactly that shape and no sky spills onto the wall.
    for(int y=Y;y<Y+H;++y)
      for(int x=X;x<X+W;++x)
        if(!glass(x,y)) d.drawPixel(x,y,0);

    // Frame: two pixels just outside the glass, traced row by row and then
    // column by column. One routine gives every style its own outline — no
    // per-shape corner cases, and the arch gets its sloped shoulders free.
    for(int y=Y-2;y<Y+H+2;++y) {
      int a=-1,b=-1;
      for(int x=X-2;x<X+W+2;++x) if(glass(x,y)) { if(a<0)a=x; b=x; }
      if(a<0) continue;
      d.drawPixel(a-1,y,frameInk);d.drawPixel(a-2,y,frameInk);
      d.drawPixel(b+1,y,frameInk);d.drawPixel(b+2,y,frameInk);
    }
    for(int x=X-2;x<X+W+2;++x) {
      int a=-1,b=-1;
      for(int y=Y-2;y<Y+H+2;++y) if(glass(x,y)) { if(a<0)a=y; b=y; }
      if(a<0) continue;
      d.drawPixel(x,a-1,frameInk);d.drawPixel(x,a-2,frameInk);
      d.drawPixel(x,b+1,frameInk);d.drawPixel(x,b+2,frameInk);
    }

    // Mullions last: they are what makes a rectangle read as a window rather
    // than a framed picture. Clipped to the glass, so a round pane keeps its
    // cross inside the circle.
    auto bar=[&](int x0,int y0,int x1,int y1) {
      const int dx=(x1>x0)?1:(x1<x0?-1:0), dy=(y1>y0)?1:(y1<y0?-1:0);
      int x=x0,y=y0;
      for(;;) {
        if(glass(x,y)) d.drawPixel(x,y,frameInk);
        if(x==x1&&y==y1) break;
        if(x!=x1) x+=dx;
        if(y!=y1) y+=dy;
      }
    };
    if(style==WIN_CROSS) {
      bar(X+W/2,Y,X+W/2,Y+H-1); bar(X,Y+H/2,X+W-1,Y+H/2);
    } else if(style==WIN_SASH) {
      // A sash window: one horizontal rail, two panes. On a narrow glass a
      // vertical rail would slice straight through the moon.
      bar(X,Y+H/2,X+W-1,Y+H/2);
    } else if(style==WIN_ROUND) {
      // Horizontal only: a cross turned the porthole into a clock face.
      bar(X,Y+H/2,X+W-1,Y+H/2);
    }
  }
  void clipCloud(int x,int y,int w,uint16_t ink,int left,int right) {
    // Four offset bands, each clipped to the glass. One rectangle reads as a
    // brick pasted on the sky; stacking shrinking bands reads as a cloud.
    auto band=[&](int bx,int by,int bw) {
      const int a=std::max(bx,left),b=std::min(bx+bw,right);
      if(b>a) d.fillRect(a,by,b-a,1,ink);
    };
    band(x+3,y-1,w-6);
    band(x+1,y,w-2);
    band(x,y+1,w);
    band(x+4,y+2,w-9);
  }
  // A plane crosses the glass roughly every 40 s, slowly and with a blinking
  // beacon. It uses uptime rather than page time, so leaving the page and
  // coming back does not restart the flight. Clipped to the glass: an arch or
  // porthole must not have wings hanging on the wall.
  void drawPlane(uint8_t style,int X,int Y,int W,int H) {
    constexpr uint32_t period=41000u;
    const uint32_t now=c.uptime(),cycle=now/period,ph=now%period;
    const uint32_t r=skyHash(cycle*0x9e3779b9u+0x51ed270bu);
    if((r&3u)==0u) return;
    const uint32_t start=2500u+(r>>4)%7000u;
    const uint32_t dur=7000u+(r>>12)%4000u;
    if(ph<start||ph>=start+dur) return;
    const int y=Y+std::max(2,H*24/100)+int((r>>20)%2);
    const bool ltr=(r>>24)&1u;
    const int travel=std::max(8,W-4);
    const int step=int((ph-start)*uint32_t(travel)/dur);
    const int x=ltr?X+1+step:X+W-2-step;
    if(x<X+1||x>X+W-2) return;
    const uint16_t body=color(0xe8eefc),wing=color(0x9fb0cc);
    auto put=[&](int px,int py,uint16_t ink){ if(inGlass(style,X,Y,W,H,px,py)) d.drawPixel(px,py,ink); };
    put(x,y,body);
    put(ltr?x+1:x-1,y,body);
    put(ltr?x+2:x-2,y,wing);
    put(x,y-1,wing);
    if(frame()&1u) put(ltr?x-1:x+1,y+1,color(0xff6b6b));
  }
  // Five-pixel moon for narrow glass, same eight-phase logic, same terminator
  // trick. A 9 px disc does not fit between the frame and a sash rail.
  void drawMoonTiny(int cx,int cy,uint8_t phase,uint16_t lit,uint16_t dark) {
    static constexpr int8_t span[5]={0,1,2,1,0};
    static constexpr int8_t edge[8]={2,1,0,-1,-2,1,0,-1};
    for(int dx=-2;dx<=2;++dx) {
      const int s=span[dx+2];
      const bool on=phase!=0&&(phase<=4?dx>=edge[phase]:dx<=edge[phase]);
      for(int dy=-s;dy<=s;++dy) d.drawPixel(cx+dx,cy+dy,on?lit:dark);
      if(!s) d.drawPixel(cx+dx,cy,dark);
    }
  }
  // Eight-phase moon from a straight terminator. The disc is 9 px wide, so a
  // line and an ellipse are indistinguishable here — and the line is cheaper.
  void drawMoon(int cx,int cy,uint8_t phase,uint16_t lit,uint16_t dark) {
    static constexpr int8_t span[9]={0,2,3,3,4,3,3,2,0};
    static constexpr int8_t edge[8]={4,2,0,-3,-4,3,0,-2};
    for(int dx=-4;dx<=4;++dx) {
      const int s=span[dx+4];
      const bool on=phase!=0&&(phase<=4?dx>=edge[phase]:dx<=edge[phase]);
      for(int dy=-s;dy<=s;++dy) d.drawPixel(cx+dx,cy+dy,on?lit:dark);
      if(!s) d.drawPixel(cx+dx,cy,dark);
    }
  }
  // Wall art between the lamp and the window (x22..37, y32..41). Six options
  // picked per 30-minute slot; "empty wall" is one of them so the room can
  // breathe instead of looking like a gallery.
  void drawWallArt(uint8_t kind,bool night) {
    constexpr int X=22,Y=32,W=16,H=10;
    if(kind==5) return;
    if(kind==4) {                                  // long, gently draped lights
      const uint16_t wire=color(0x67574b);
      for(int x=14;x<=38;++x) {
        const int sag=x>=20&&x<=32?1:0;
        d.drawPixel(x,32+sag,wire);
      }
      static constexpr uint32_t bulbs[]={0xffd166u,0xff9c7au,0x9ce4c3u,0xd6b2ffu};
      for(int i=0;i<6;++i) {
        const int bx=15+i*4,by=34+(i>=1&&i<=4?1:0);
        d.drawPixel(bx,by-1,wire);
        const int glow=185+int(16*std::sin(frame()*0.025f+i*1.7f));
        d.drawFastHLine(bx,by,2,mix(0,bulbs[i%4],glow));
      }
      return;
    }
    const uint16_t rail=color(night?0x6b4a3e:0x9a6a52);
    const uint16_t canvas=color(night?0x152238:0x2f5f77);
    d.fillRect(X,Y,W,H,rail);
    d.fillRect(X+1,Y+1,W-2,H-2,canvas);
    if(kind==0) {                                  // mountains
      d.fillTriangle(X+2,Y+H-2,X+7,Y+3,X+12,Y+H-2,color(night?0x35507a:0x4f7f63));
      d.fillTriangle(X+8,Y+H-2,X+12,Y+4,X+15,Y+H-2,color(night?0x2a3f61:0x3a6650));
      d.drawPixel(X+13,Y+3,color(0xffe2a0));
    } else if(kind==1) {                           // sunset over water
      // Deliberately not a moon: a second blue disc next to the window made
      // the wall read as two windows instead of a picture.
      d.fillRect(X+1,Y+1,W-2,H-2,color(0x3a2a4a));
      d.fillCircle(X+8,Y+5,2,color(0xff9a5c));
      d.drawFastHLine(X+1,Y+7,W-2,color(0x8a4a6a));
      d.drawFastHLine(X+2,Y+8,5,color(0xd88a6a));
      d.drawFastHLine(X+9,Y+8,4,color(0xb86a5a));
    } else if(kind==2) {                           // three snapshots
      d.fillRect(X+1,Y+1,W-2,H-2,color(0x232a36));
      for(int i=0;i<3;++i) {
        const int px=X+2+i*5;
        d.fillRect(px,Y+3,4,5,color(0xdfe6ea));
        d.fillRect(px,Y+3,4,1,color(0xb8c4cc));
        d.drawPixel(px+1,Y+5,color(0x6b5a4a));
        d.drawPixel(px+2,Y+5,color(0x6b5a4a));
      }
    } else {                                       // shelf: books and a plant
      d.fillRect(X+1,Y+1,W-2,H-2,color(0x1a2230));
      d.fillRect(X+2,Y+7,12,1,color(0x8a6a4a));
      static constexpr uint32_t spines[]={0xc96a5c,0x6ea3c9,0xe0b060,0x8ac9a0};
      for(int i=0;i<4;++i) {
        const int bx=X+3+i*2,bh=3+i%2;
        d.fillRect(bx,Y+8-bh,2,bh,color(spines[i]));
      }
      d.fillRect(X+11,Y+5,3,2,color(0xb06a4a));
      d.drawPixel(X+11,Y+4,color(0x63c48a));
      d.drawPixel(X+12,Y+3,color(0x3f9f6d));
      d.drawPixel(X+13,Y+4,color(0x63c48a));
    }
  }
  // Desk: the table never moves and the prop no longer does either. The desk
  // is pinned to the fishbowl (see the note in drawDesk); the old rotating
  // picker was retired after an hourglass kept appearing next to the clock.
  void drawDesk(bool night,uint8_t weather,int hour,uint32_t seed) {
    const uint16_t wood=color(night?0xa86a55:(weather>=2?0xa87258:0xba795d));
    d.fillRect(14,50,24,2,wood);
    d.fillRect(16,52,2,7,color(0x694439));
    d.fillRect(34,52,2,7,color(0x694439));
    // The desk keeps its fishbowl. The rotating prop pool (turntable, plant,
    // candle, hourglass, book) is retired: on a clock page a random object
    // that swaps every few hours reads as a bug, and the hourglass especially
    // looked like a mystery icon sitting next to the time.
    const uint8_t prop=2;
    (void)hour;(void)seed;
    switch(prop) {
      case 1: loftTurntable(); break;
      case 2: loftFishbowl(); break;
      case 3: loftPlant(); break;
      case 4: loftCandle(); break;
      case 5: loftHourglass(); break;
      default: loftBook(); break;
    }
  }
  void loftBook() {
    d.fillRoundRect(16,44,12,5,1,color(0xa4556a));
    d.fillRect(18,44,4,3,color(0xf2bf88));d.fillRect(24,44,3,3,color(0xffdda6));
    d.fillRect(22,45,1,4,color(0x9b6651));
    d.fillRect(31,46,4,3,color(0xd9e2ea));d.fillRect(30,49,6,1,color(0x8fa0b8));
    if(t.sceneDetail>=3) {
      roomSteam(32,44,23);
    }
  }
  void loftTurntable() {
    d.fillRect(23,47,13,3,color(0x4a3129));        // cabinet
    d.fillRect(24,45,11,2,color(0x7a5546));        // deck
    d.fillEllipse(29,45,4,1,color(0x14141a));       // platter
    d.drawPixel(29,45,color(0xe8d8a8));            // label
    d.drawLine(34,44,32,46,color(0xc9b48a));       // tonearm
    // The record's highlight runs around the platter: the one cue that says
    // "spinning" without spending pixels on a sleeve.
    static constexpr int8_t ring[8][2]={{-3,0},{-2,1},{0,1},{2,1},{3,0},{2,-1},{0,-1},{-2,-1}};
    const auto& p=ring[frame()%8u];
    d.drawPixel(29+p[0],45+p[1],color(0x5f6070));
    // Two notes drift up the bare wall left of the picture.
    for(int i=0;i<2;++i) {
      const int rise=int((frame()/4+uint32_t(i)*6u)%10u);
      const int nx=20-i,ny=44-rise;
      d.drawPixel(nx,ny,color(0xffd8a0));
      d.drawPixel(nx,ny-1,color(0xffd8a0));
      d.drawPixel(nx+1,ny-2,color(0xffb26b));
    }
  }
  void loftFishbowl() {
    constexpr int x=16,y=38;
    // Larger, left-aligned glass. It fills the desk without touching the lamp.
    static constexpr uint8_t widths[12]={14,18,20,22,22,22,22,20,18,16,12,10};
    for(int row=0;row<12;++row) {
      const int w=widths[row],left=x+(22-w)/2;
      d.drawFastHLine(left,y+row,w,color(row<2?0x29424c:0x153d4a));
      d.drawPixel(left,y+row,color(row<6?0x83afbb:0x537d8c));
      d.drawPixel(left+w-1,y+row,color(0x527986));
    }
    d.drawFastHLine(x+4,y,14,color(0xa6c9cf));
    d.drawFastHLine(x+2,y+2,18,color(0x4b8492));
    d.drawFastHLine(x+6,y+11,10,color(0x8bafb8));
    d.drawFastVLine(x+2,y+3,3,color(0xa2c4c9));
    const int sway=int((frame()/12u)%2u);
    d.drawFastVLine(x+5,y+7,3,color(0x3e9b82));
    d.drawPixel(x+4+sway,y+7,color(0x6cae8d));
    d.drawPixel(x+6,y+8,color(0x6cae8d));
    d.drawFastVLine(x+15,y+8,2,color(0x3e9b82));
    d.drawPixel(x+14+sway,y+7,color(0x6cae8d));
    const uint32_t bubble=(frame()+9u)%61u;
    if(bubble<36u) d.drawPixel(x+17,y+8-int(bubble/6u),color(0x79b7c8));
    auto smallFish=[&](int fx,int fy,bool right,uint32_t body,uint32_t tail){
      d.drawFastHLine(fx-1,fy,3,color(body));
      d.drawFastHLine(fx-1,fy+1,2,color(tail));
      d.drawPixel(fx+(right?1:-1),fy,color(0x49362d));
      d.drawPixel(fx+(right?-2:2),fy+1,color(tail));
    };
    // Two lanes, different speeds and phases; no overlap into one big fish.
    const uint32_t lap=(frame()/5u)%24u;
    const bool right=lap<12u;
    smallFish(x+5+int(right?lap:23u-lap),y+4,right,0xffc477,0xe99a57);
    const uint32_t lap2=(frame()/7u+11u)%24u;
    const bool right2=lap2>=12u;
    smallFish(x+5+int(right2?lap2-12u:11u-lap2),y+7,right2,0xe9a1b5,0xb9769b);
  }
  void loftPlant() {
    d.fillRect(28,47,7,3,color(0xb06a4a));         // pot
    d.fillRect(28,47,7,1,color(0xc97f5c));
    for(int i=0;i<3;++i) {
      const int sway=int((frame()/9+uint32_t(i)*2u)%3u)-1;
      const int bx=30+i*2;
      for(int seg=0;seg<4+i%2;++seg)
        d.drawPixel(bx+sway*seg/2,46-seg,color(seg>1?0x63c48a:0x3f9f6d));
    }
  }
  void loftCandle() {
    d.fillRect(28,49,9,1,color(0x8a6a4a));         // dish
    d.fillRect(31,45,3,4,color(0xf0e2c4));         // candle
    const bool tall=skyHash(frame()/4u+0x4357u)%5u!=0u;
    d.drawPixel(32,44,tall?color(0xffd166):color(0xffa64d));
    d.drawPixel(32,43,tall?color(0xfff0c0):color(0xffd166));
    d.drawPixel(31,45,mix(0,0xffca7e,90));         // glow on the table
    d.drawPixel(34,46,mix(0,0xffca7e,60));
  }
  void loftHourglass() {
    constexpr int x=30,y=43;
    d.fillRect(x,y,6,1,color(0x8a6a4a));           // caps
    d.fillRect(x,y+6,6,1,color(0x8a6a4a));
    d.drawLine(x+1,y+1,x+4,y+5,color(0xa8b9c6));   // glass
    d.drawLine(x+4,y+1,x+1,y+5,color(0xa8b9c6));
    const uint32_t t=frame()%18u;
    const int top=3-int(t/6);                      // upper bulb drains
    for(int i=0;i<top;++i) d.drawFastHLine(x+1+i,y+2+i,4-i*2,color(0xe0b060));
    const int pile=int(t/4);                       // lower pile grows
    for(int i=0;i<=pile&&i<3;++i) d.drawFastHLine(x+1+i,y+5-i,4-i*2,color(0xe0b060));
    if(t<12) d.drawPixel(x+3,y+3+int(t%3),color(0xf2d090));  // falling grain
  }
  // Loft cat: paces wall to wall, with occasional shows (nap / stretch).
  // The pacing clock (walkMs) only advances while the cat is actually
  // walking — it freezes during a show — so when the show ends the cat
  // resumes from the exact spot where it stopped instead of teleporting
  // to wherever the global timeline says it should be.
  void catProwl(int y) {
    static uint32_t walkMs=0;   // accumulated pacing time (paused in shows)
    static uint32_t lastT=0;
    const uint32_t t=c.uptime();
    const uint32_t dt=(t>lastT)?(t-lastT):0u;
    lastT=t;
    constexpr uint32_t slotMs=28000u;
    const uint32_t slot=t/slotMs, st=t%slotMs;
    const uint32_t rnd=skyHash(slot*0x27d4eb2fu+0x1b873593u);
    const bool rest=(rnd%3)==0;                   // 1 in 3 slots: pure pacing
    const uint32_t start=4500u+((rnd>>3)%2500u);  // 4.5..7s into the slot
    const uint32_t dur=12000u+((rnd>>13)%4000u);   // 12..16s, ends inside slot
    const int kind=int((rnd>>23)%3u);             // 0=nap, 1=scratch, 2=crouch
    const bool acting=!rest && st>=start && st<start+dur;
    if (!acting) walkMs+=dt;                      // only walking advances it
    const uint32_t wp=(walkMs/100u)%140u;
    const int x=1+((wp<70?wp:139-wp)*46)/70;
    const bool right=wp<70;
    const uint32_t elapsed=acting?st-start:0;
    if (acting && kind==1) {
      drawCatScratch(x,y,right,elapsed*100u/dur);
      return;
    }
    if (acting && kind==2) {
      drawCatCrouch(x,y,right,elapsed*100u/dur);
      return;
    }
    if(acting&&kind==0) {
      if(elapsed<900u) { drawCatCrouch(x,y,right,elapsed*20u/900u); return; }
      if(elapsed+900u>=dur) { drawCatCrouch(x,y,right,80u+(elapsed+900u-dur)*20u/900u); return; }
    }
    drawCat(x,y,right,int((acting?elapsed:walkMs)/300u),acting&&kind==0);
  }
  void drawCatScratch(int x,int y,bool right,uint32_t progress) {
    // 后腿挠耳朵：椭圆身体保持，眯眼，尾巴高扬，后腿弯向头部
    uint16_t fur=color(0xf5e6c8),deep=color(0xd0b894),ink=color(0x4a2c1f),pink=color(0xe8837e);
    // 尾巴高扬
    if(right){ d.drawLine(x+1,y+3,x-1,y+1,deep); d.drawLine(x-1,y+1,x-2,y-1,deep); }
    else     { d.drawLine(x+11,y+3,x+13,y+1,deep); d.drawLine(x+13,y+1,x+14,y-1,deep); }
    // 身体（椭圆，不变）
    d.fillEllipse(x+6,y+3,5,2,fur);
    // 两条前腿踩地
    d.fillRect(x+3,y+5,2,2,fur); d.fillRect(x+8,y+5,2,2,fur);
    d.fillRect(x+3,y+6,1,1,deep); d.fillRect(x+8,y+6,1,1,deep);
    // 头
    const int hx=right?x+10:x+1;
    d.fillRect(hx,y,4,4,fur);
    d.drawPixel(hx,y-1,deep); d.drawPixel(hx+3,y-1,deep); d.drawPixel(hx+1,y-1,pink);
    const bool bliss=progress>=15&&progress<80;
    d.drawPixel(hx+(right?2:1),y+1,bliss?deep:ink);  // 眯眼表情
    d.drawPixel(hx+(right?3:0),y+2,pink);             // 鼻子
    // 挠耳的后腿（progress 10–85 阶段）
    if(progress>=10&&progress<85) {
      const bool tick=(progress/4)%2==0;
      if(right){
        d.drawLine(x+4,y+4,x+9,y+1,deep);
        d.drawPixel(x+10,y-(tick?1:0),fur);
      } else {
        d.drawLine(x+10,y+4,x+5,y+1,deep);
        d.drawPixel(x+4,y-(tick?1:0),fur);
      }
    }
  }
  void drawCatCrouch(int x,int y,bool right,uint32_t progress) {
    // 猫趴伏成饼：身体下沉，尾巴收拢后绕到身侧轻弹尾尖
    uint16_t fur=color(0xf5e6c8),deep=color(0xd0b894),ink=color(0x4a2c1f),pink=color(0xe8837e);
    // squat: 0→2 下沉，20–80 保持，80→0 起身
    const int squat=progress<20?(int)(progress/10):progress<80?2:2-(int)((progress-80)/10);
    // 尾尖抖动（趴稳后）
    const int tw=(progress>=20&&progress<80)?int((progress-20)/15)%3:0;
    // 尾巴
    if(right){
      if(squat<2){ d.drawLine(x+1,y+3,x-2,y+2,deep); d.drawLine(x-2,y+2,x-3,y+0,deep); }
      else       { d.drawLine(x+1,y+4,x+4,y+6,deep); d.drawLine(x+4,y+6,x+7+tw,y+5,deep); }
    } else {
      if(squat<2){ d.drawLine(x+11,y+3,x+14,y+2,deep); d.drawLine(x+14,y+2,x+15,y+0,deep); }
      else       { d.drawLine(x+13,y+4,x+10,y+6,deep); d.drawLine(x+10,y+6,x+7-tw,y+5,deep); }
    }
    // 身体下沉
    d.fillEllipse(x+6,y+4+squat/2,5,2,fur);
    // 腿缩短
    const int lt=y+5+squat/2, lh=std::max(1,2-squat/2);
    d.fillRect(x+3,lt,2,lh,fur); d.fillRect(x+8,lt,2,lh,fur);
    d.fillRect(x+3,lt+lh-1,1,1,deep); d.fillRect(x+8,lt+lh-1,1,1,deep);
    // 头随身体下沉，趴好后目光偏向尾巴方向
    const int hx=right?x+10:x+1, hy=y+squat/2;
    d.fillRect(hx,hy,4,4,fur);
    d.drawPixel(hx,hy-1,deep); d.drawPixel(hx+3,hy-1,deep); d.drawPixel(hx+1,hy-1,pink);
    const int es=(squat>=2)?(right?-1:1):0;
    d.drawPixel(hx+(right?2:1)+es,hy+1,ink);
    d.drawPixel(hx+(right?3:0),hy+2,pink);
  }
  void drawCat(int x,int y,bool right,int step,bool napping) {
    uint16_t fur=color(0xf5e6c8),deep=color(0xd0b894),ink=color(0x4a2c1f),pink=color(0xe8837e);
    if (napping) {
      d.fillEllipse(x+6,y+5,5,2,fur);
      const int hx=right?x+10:x+1;
      d.fillRect(hx,y+2,4,3,fur);
      d.drawPixel(hx,y+1,deep);d.drawPixel(hx+3,y+1,deep);
      d.drawFastHLine(hx+1,y+3,2,ink);
      d.drawPixel(hx+(right?3:0),y+4,pink);
      d.fillRect(right?hx-1:hx+4,y+5,2,1,deep);
      d.drawLine(x+1,y+5,x+3,y+6,deep);
      d.drawFastHLine(x+3,y+6,3,deep);
      const int shade=180+int(15*std::sin(frame()*0.12f));
      d.drawFastHLine(x+4,y+4,3,mix(0xd0b894u,0xf5e6c8u,shade));
      return;
    }
    // 4-phase tail: high / horizontal / low / S-curve
    const int tp=(step/2)%4;
    if(right) {
      if     (tp==0){ d.drawLine(x+1,y+3,x-1,y+1,deep); d.drawLine(x-1,y+1,x-3,y-1,deep); }
      else if(tp==1){ d.drawLine(x+1,y+3,x-2,y+2,deep); d.drawLine(x-2,y+2,x-4,y+1,deep); }
      else if(tp==2){ d.drawLine(x+1,y+3,x-1,y+4,deep); d.drawLine(x-1,y+4,x-3,y+3,deep); }
      else          { d.drawLine(x+1,y+3,x-2,y+2,deep); d.drawLine(x-2,y+2,x-4,y+0,deep); }
    } else {
      if     (tp==0){ d.drawLine(x+11,y+3,x+13,y+1,deep); d.drawLine(x+13,y+1,x+15,y-1,deep); }
      else if(tp==1){ d.drawLine(x+11,y+3,x+14,y+2,deep); d.drawLine(x+14,y+2,x+16,y+1,deep); }
      else if(tp==2){ d.drawLine(x+11,y+3,x+13,y+4,deep); d.drawLine(x+13,y+4,x+15,y+3,deep); }
      else          { d.drawLine(x+11,y+3,x+14,y+2,deep); d.drawLine(x+14,y+2,x+16,y+0,deep); }
    }
    d.fillEllipse(x+6,y+3,5,2,fur);
    bool a=(step&1)==0;
    d.fillRect(x+3+(a?1:0),y+5,2,2,fur);
    d.fillRect(x+8+(a?0:1),y+5,2,2,fur);
    d.fillRect(x+3+(a?1:0),y+6,1,1,deep);
    d.fillRect(x+8+(a?0:1),y+6,1,1,deep);
    int hx=right?x+10:x+1;
    d.fillRect(hx,y,4,4,fur);
    d.drawPixel(hx,y-1,deep);d.drawPixel(hx+3,y-1,deep);
    d.drawPixel(hx+1,y-1,pink);
    d.drawPixel(hx+(right?2:1),y+1,ink);
    d.drawPixel(hx+(right?3:0),y+2,pink);
  }
  void drawChinese12(int x, int y, int idx, uint16_t col) {
    if (idx < 0 || idx >= 8) return;
    for (int r = 0; r < 12; ++r) {
      uint16_t bits = CHINESE_12x12[idx][r];
      for (int col_i = 0; col_i < 12; ++col_i) {
        if ((bits >> (11 - col_i)) & 1) {
          d.drawPixel(x + col_i, y + r, col);
        }
      }
    }
  }
  void drawDigits4x7(int x, int y, int dig, uint16_t col) {
    if (dig < 0 || dig > 9) return;
    for (int r = 0; r < 7; ++r) {
      uint8_t row = DIGITS_4x7[dig][r];
      for (int c_i = 0; c_i < 4; ++c_i) {
        if ((row >> (3 - c_i)) & 1) {
          d.drawPixel(x + c_i, y + r, col);
        }
      }
    }
  }
  void drawStock() {
    // Phase anchor is the moment this page appeared, not boot uptime: every
    // entry starts with SH for 6s, then NASDAQ for 6s, deterministically.
    uint32_t shown = c.uptime() - v.pageShownAt;
    int sub = (shown / 6000) % 2;
    const StockQuote& q = (sub == 0) ? v.stock.sh : v.stock.nasdaq;
    bool isUp = (q.change >= 0);
    uint16_t cChange = isUp ? color(0xff4455) : color(0x2ed573);

    // 1. Header (Y=2): Chinese Title (12px) + Region Tag (CN/US without box, bold vibrant)
    if (sub == 0) {
      uint16_t cTitle = color(0x6ee7b7);
      drawChinese12(2, 2, 0, cTitle); // 上
      drawChinese12(14, 2, 1, cTitle); // 证
      drawChinese12(26, 2, 2, cTitle); // 指
      drawChinese12(38, 2, 3, cTitle); // 数
      // A shut market dims its tag: the numbers are the last close, and the
      // panel should not look like it is quoting live.
      micro(50, 7, "CN", q.closed ? color(0x8a7440) : color(0xffd166));
    } else {
      uint16_t cTitle = color(0xffbe76);
      drawChinese12(2, 2, 4, cTitle); // 纳
      drawChinese12(14, 2, 5, cTitle); // 斯
      drawChinese12(26, 2, 6, cTitle); // 达
      drawChinese12(38, 2, 7, cTitle); // 克
      micro(50, 7, "US", q.closed ? color(0x3f7588) : color(0x70d6ff));
    }

    if (q.price > 0) {
      char price[16];
      snprintf(price,sizeof(price),sub==0?"%.2f":"%.1f",q.price);
      int x=3;
      for (const char* ch=price;*ch && x<60;++ch) {
        if (*ch>='0' && *ch<='9') {
          digit(x,15,*ch-'0',1,color(0xf5f8ff));
          x+=6;
        } else if (*ch=='.') {
          d.drawPixel(x,23,color(0xf5f8ff));
          x+=3;
        }
      }
      char pct[16];
      snprintf(pct,sizeof(pct),"%+.2f%%",q.pct);
      micro(62-int(std::strlen(pct))*4,28,pct,cChange);
    } else {
      micro(3,19,"NO QUOTE",color(0x8799adu));
    }

    char detail[16];
    const float gold = sub==0?v.stock.goldPrice:v.stock.goldUsd;
    const char* symbol = sub==0?"AU":"XAU";
    if (gold>0) snprintf(detail,sizeof(detail),"%s%.0f",symbol,gold);
    else snprintf(detail,sizeof(detail),"%s--",symbol);
    // Dim amber + two little bars when this is the last close rather than a
    // live print. The bars sit in the gap before the percentage, so the fixed
    // layout does not move by a single pixel.
    // CNY and USD gold close at different times, so each line carries its own
    // flag: dimming spot gold during the SGE evening gap would be a lie.
    const bool shut = (sub==0 ? v.stock.goldClosed : v.stock.goldUsdClosed) && gold>0;
    micro(3,28,detail,shut?color(0x9a7f4b):color(0xffd47bu));
    if (shut) {
      const int x = 5 + int(std::strlen(detail))*4;
      for (int dy = 0; dy < 4; ++dy) {
        d.drawPixel(x, 29+dy, color(0x9a7f4b));
        d.drawPixel(x+2, 29+dy, color(0x9a7f4b));
      }
    }
    d.drawFastHLine(3,35,58,color(0x26384cu));

    // Both the actual minute prices and prior close use the same vertical scale.
    if (q.price<=0 || q.trendCount < 2 || q.trendRef < 0) {
      micro(3,48,"NO TREND",color(0x8799adu));
      return;
    }
    constexpr int chartBottom=61,chartHeight=24;
    const auto chartY=[&](int value) { return chartBottom-std::clamp(value,0,18)*chartHeight/18; };
    for (int x = 2; x < 62; x += 3) {
      d.drawPixel(x, chartY(q.trendRef), color(0x53647b));
    }
    uint8_t count = q.trendCount;
    int prevX = -1, prevY = -1;
    for (int i = 0; i < count; ++i) {
      int tx = 3 + (q.timedTrend ? q.trendX[i] : (i * 57) / (count - 1));
      int ty = chartY(q.trend[i]);

      if (prevX >= 0) {
        d.drawLine(prevX, prevY, tx, ty, cChange);
      }
      prevX = tx; prevY = ty;
    }

    // Glowing dot at tip
    if (prevX >= 0) {
      uint16_t cPulse = isUp ? color(0xffa0b0) : color(0xa0ffc8);
      d.drawPixel(prevX, prevY, cPulse);
      d.drawPixel(prevX, prevY + 1, cPulse);
    }
  }
  void drawMessage() {
    if(c.uptime()>=v.messageUntil||!v.message[0])return;
    size_t length=std::strlen(v.message),offset=length>14?((c.uptime()-v.messageStarted)/400)%(length+14):0;
    char text[15]={};for(int i=0;i<14;++i){size_t p=offset+i;text[i]=p<length?v.message[p]:' ';}
    d.fillRect(1,1,62,9,color(0x172521));d.drawFastVLine(1,1,9,color(t.accentColor));
    micro(4,3,text,color(0xe3d3ad));
  }

 public:
  // ---------------------------------------------------------------------
  // Reaction faces. Five moods share one marshmallow: LEAN (picked up and
  // tipped), SHAKE (waved about), FLIP (turned upside down in its own plane),
  // TAP (a fingernail on the glass) and DIZZY (the hangover after a shake).
  //
  // Living in the shared renderer buys the same two things the old copies in
  // the sketch could not: every colour goes through color(), so the sleep
  // window turns these frames red like everything else, and the Mac preview
  // renders the exact same pixels without the chip on the desk.
  //
  // FLIP is painted through a 180° coordinate mirror on purpose. The panel is
  // physically upside down at that moment, so mirroring in device space is what
  // makes the face — and the sentence — read the right way up to the person
  // holding it. Every primitive below is authored in "what the viewer sees"
  // coordinates; only the mirror knows about device space.
  void drawGesture(const Gesture& g) {
    latchNight();
    d.setTextWrap(false); d.fillScreen(0);
    const uint8_t mood = std::min<uint8_t>(g.mood, GESTURE_HELD);
    if (!mood) return;
    const bool flip = mood == GESTURE_FLIP;
    auto rect = [&](int x, int y, int w, int h, uint32_t hex) {
      const uint16_t ink = color(hex);
      if (flip) d.fillRect(63 - (x + w - 1), 63 - (y + h - 1), w, h, ink);
      else      d.fillRect(x, y, w, h, ink);
    };
    auto px = [&](int x, int y, uint32_t hex) { rect(x, y, 1, 1, hex); };

    // Enhanced Mint Aurora Cream palette: refreshing, soothing, high contrast on HUB75 black
    const uint32_t peach      = 0x64e2b6;   // 清透嫩薄荷绿 (Crisp Mint)
    const uint32_t peachLight = 0xc0ffea;   // 冰霜奶白薄荷轮廓光 (Frost Mint Rim)
    const uint32_t peachDark  = 0x2e9a74;   // 柔深翡翠阴影 (Jade Shadow)
    const uint32_t milk       = 0xf5fffa;   // 纯净雪白高光 & 眼睛 (Snow White)
    const uint32_t ink        = 0x10221c;   // 深松石墨镜屏底 (Deep Spruce Visor)
    const uint32_t blush      = 0xff6292;   // 甜美珊瑚樱花粉 (Vivid Coral Sakura Blush)
    const uint32_t blushHi    = 0xffa4c2;   // 腮红晶亮反光
    const uint32_t sweat      = 0x60d8ff;   // 晶莹冰蓝冷汗滴 (Cyan Sweat Beads)

    const uint32_t accent = mood == GESTURE_LEAN   ? 0xffba3b
                          : mood == GESTURE_SHAKE  ? 0xff70a5
                          : mood == GESTURE_FLIP   ? 0xffb050
                          : mood == GESTURE_TAP    ? 0x64d0ff
                          : mood == GESTURE_SETTLE ? 0x58f4a6
                          : mood == GESTURE_HELD   ? 0x60e0ff
                                                   : 0x90f0d0;
    const uint32_t mint = mood == GESTURE_FLIP ? 0xccaaff
                        : (mood == GESTURE_SHAKE || mood == GESTURE_DIZZY) ? 0xffd270
                        : 0x82fcd0;
    const float t = float(g.now);
    const int breathe = int(sinf(t / 220.0f) * 1.5f);

    // ---- Body anchor & dynamic movement physics -------------------------
    int cx = 32, cy = 30;
    int bodySquashX = 0, bodySquashY = 0;  // squash & stretch deformation

    if (mood == GESTURE_LEAN) {
      // 物理果冻滑冰冲浪：身体随倾角滑向低侧边框，并产生贴壁果冻挤压形变！
      const int targetSlip = std::clamp(int(g.leanX) * 18 / 100, -17, 17);
      const int jitter = ((g.now / 35) % 2) * (g.leanX > 0 ? 1 : -1);
      cx = 32 + targetSlip + (std::abs(targetSlip) >= 12 ? jitter : 0);
      cy = 28 + std::abs(int(g.leanX)) * 4 / 100;
      if (std::abs(targetSlip) >= 10) {
        bodySquashX = -3; bodySquashY = 2; // 贴壁挤压扁平感
      }
    } else if (mood == GESTURE_HELD) {
      // 悬空被捧在手里：轻柔微浮动
      cy = 24 + int(sinf(t / 140.0f) * 1.5f);
    } else if (mood == GESTURE_SHAKE) {
      // Violent jelly vibration & squash/stretch
      const int phase = (g.now / 55) % 4;
      cx += ((g.now / 40) % 5) - 2;
      cy += ((g.now / 60) % 3) - 1;
      if (phase == 0 || phase == 2) { bodySquashX = 2; bodySquashY = -2; }
      else                          { bodySquashX = -2; bodySquashY = 1; }
    } else if (mood == GESTURE_DIZZY) {
      // Drunken stumble & wobble figure-8
      cx += int(sinf(t / 260.0f) * 3.0f);
      cy += int(cosf(t / 130.0f) * 1.2f);
    } else if (mood == GESTURE_TAP) {
      // Startled jerk back & defensive posture
      const float p = float(g.since) / 80.0f;
      cy -= int(std::max(0.0f, 6.0f - p));
      cx += int(sinf(float(g.since) / 30.0f) * std::max(0.0f, 1.5f - p * 0.2f));
    } else if (mood == GESTURE_SETTLE) {
      // Sinks down in deep relief, exhalation deflation
      const float p = float(g.since) / 100.0f;
      cy += int(std::max(0.0f, 2.5f - p));
    } else if (mood == GESTURE_FLIP) {
      // Swing pendulum on wrists
      const float slip = g.since < 200
          ? 9.0f * float(g.since) * float(g.since) / 40000.0f
          : 9.0f * std::max(0.0f, 1.0f - float(g.since - 200) / 165.0f);
      const float swing = sinf(t / 150.0f) * (2.2f + 3.0f * expf(-float(g.since) / 1200.0f));
      cx += int(swing);
      cy = 23 + int(slip);
    }

    // ---- LEAN: 物理滑坡轨道 + 贴壁死撑爪 + 绝望高处挥手 + 滚落粒子 -----
    if (mood == GESTURE_LEAN) {
      const int slope = std::clamp(int(g.leanX) / 6, -12, 12);
      const int baseY = std::clamp(43 + int(g.leanY) / 22, 38, 47);
      const int l0 = baseY + slope, r0 = baseY - slope;

      // 动感重力倾斜坡道
      d.drawLine(0, l0,     63, r0,     color(0x2d6854));  // 荧光轨顶
      d.drawLine(0, l0 + 1, 63, r0 + 1, color(0x1a4034));
      d.drawLine(0, l0 + 2, 63, r0 + 2, color(0x0e241d));  // 坡底阴影

      // 沿坡道飞速滚落逃逸的能量火星光斑
      const bool leanLeft = g.leanX < 0;
      for (int i = 0; i < 4; ++i) {
        const int speed = 20 + i * 7;
        int px0 = (int(g.now / speed) + i * 16) % 64;
        if (!leanLeft) px0 = 63 - px0; // 滚向重力低侧
        const int py0 = l0 - (2 * slope * px0) / 63 + 1;
        if (py0 >= 0 && py0 < 63) {
          px(px0, py0, (i % 2 == 0) ? 0x80ffe0 : 0x40bfa0);
          if (i == 0) px(px0 + (leanLeft ? 1 : -1), py0, 0xffffff);
        }
      }

      // 低侧贴壁撑墙小爪（拼死顶住屏幕玻璃壁）与 高侧拼命摇摆呼救手臂
      const bool leanAway = std::abs(int(g.leanX)) >= 14;
      if (leanAway) {
        const int wallX = leanLeft ? 2 : 61; // 贴壁侧屏幕边框
        // 贴壁死撑的双爪
        const int pushY1 = cy - 4, pushY2 = cy + 4;
        const int pawX = leanLeft ? (wallX + 1) : (wallX - 3);
        rect(pawX, pushY1, 3, 2, milk);
        rect(pawX, pushY2, 3, 2, milk);
        // 支撑受力火星
        px(wallX, pushY1, 0xffd240); px(wallX, pushY2, 0xff8030);
        // 手臂线条连接到身体
        d.drawLine(cx + (leanLeft ? -9 : 9), cy - 2, pawX, pushY1, color(peachLight));
        d.drawLine(cx + (leanLeft ? -9 : 9), cy + 2, pawX, pushY2, color(peachLight));

        // 高处悬空手臂狂乱挥舞呼救
        const int flailArmX = cx + (leanLeft ? 12 : -12);
        const int flailHandX = flailArmX + (leanLeft ? 3 : -3);
        const int flailHandY = cy - 8 + int(sinf(t / 45.0f) * 4.0f);
        rect(flailHandX - 1, flailHandY - 1, 3, 3, milk);
        d.drawLine(cx + (leanLeft ? 8 : -8), cy - 2, flailHandX, flailHandY, color(peach));
      }
    }

    // ---- FLIP: Overhead dual-wrist hanging ------------------------------
    if (mood == GESTURE_FLIP) {
      for (int side = -1; side <= 1; side += 2) {
        const int handX = 32 + side * 13, shoulderX = cx + side * 11;
        const int span = std::max(1, cy - 5);
        for (int y = 5; y <= cy + 2; ++y) {
          const int x = handX + (shoulderX - handX) * (y - 5) / span;
          px(x, y, peachLight); px(x + 1, y, peach);
        }
        rect(handX - 2, 3, 5, 3, milk);               // white-knuckle fingers
        px(handX - 3, 2, 0xffd270); px(handX + 3, 2, 0xffd270); // grip strain sparks
      }
    }

    // ---- Marshmallow Pillowy Body (with Squash & Stretch) ---------------
    const int bx = cx, by = cy;
    const int hw = 11 + bodySquashX / 2;
    const int hh = 11 + bodySquashY / 2;

    // Antenna ears with pulsing tips
    for (int side = -1; side <= 1; side += 2) {
      const int ox = bx + side * 8;
      int ear = 2 + breathe;
      if (mood == GESTURE_TAP) ear = int(std::max(1.0f, 6.0f - float(g.since) / 180.0f));
      else if (mood == GESTURE_SHAKE) ear = (g.now / 50) % 2;
      else if (mood == GESTURE_LEAN) {
        // Flatten high ear, point low ear down
        ear = (side * g.leanX > 0) ? 3 : 1;
      }
      rect(ox - 2, by - hh - ear + 1, 4, 3 + ear, peach);
      px(ox - 1, by - hh - ear, milk);  // illuminated tip
      px(ox,     by - hh - ear, (g.now % 600 < 300) ? 0xffffff : accent);
    }

    // Main pillowy body
    rect(bx - hw, by - hh, hw * 2, hh * 2, peach);
    rect(bx - hw - 3, by - 6 + bodySquashY / 2, (hw + 3) * 2, 13, peach); // plump cheeks

    // Top & side rim 3D lighting highlights
    d.drawLine(bx - hw + 2, by - hh, bx + hw - 3, by - hh, color(peachLight));
    d.drawLine(bx - hw - 2, by - 5, bx - hw - 2, by + 4, color(peachLight));
    // Soft under-body shadow
    d.drawLine(bx - hw + 2, by + hh - 1, bx + hw - 3, by + hh - 1, color(peachDark));

    // Visor dark display window
    rect(bx - 11, by - 7, 22, 14, ink);
    rect(bx - 9, by - 6, 18, 1, 0x3d274c); // inner screen shadow

    // Corner bevels (ultra rounded pillowy silhouette)
    px(bx - hw,     by - hh,     0); px(bx + hw - 1, by - hh,     0);
    px(bx - hw,     by + hh - 1, 0); px(bx + hw - 1, by + hh - 1, 0);
    px(bx - hw - 3, by - 6,      0); px(bx + hw + 2, by - 6,      0);
    px(bx - hw - 3, by + 6,      0); px(bx + hw + 2, by + 6,      0);

    // Sweet blushing cheeks (with glowing specular dot!)
    const int blushW = (mood == GESTURE_FLIP) ? 4 : 3;
    const int blushH = (mood == GESTURE_FLIP) ? 3 : 2;
    rect(bx - 14, by + 2, blushW, blushH, blush);
    rect(bx + 14 - blushW, by + 2, blushW, blushH, blush);
    px(bx - 13, by + 2, blushHi); px(bx + 12, by + 2, blushHi);

    // ---- Eyes & Rich Facial Expressions --------------------------------
    const int lookX = std::clamp(int(g.leanX) / 30, -2, 2);
    const int lookY = std::clamp(int(g.leanY) / 30, -2, 2);
    const bool blink = mood == GESTURE_LEAN && (g.now % 4200 > 4080);

    for (int side = -1; side <= 1; side += 2) {
      const int ex = bx + side * 5, ey = by - 2;

      if (mood == GESTURE_LEAN) {
        // Terrified wide eyes: pupils pulled completely to the low fall side
        rect(ex - 3, ey - 3, 6, 7, milk);
        const int pOffX = (g.leanX < 0) ? -2 : 2;
        const int pJit  = (g.now / 60) % 2;
        rect(ex - 1 + pOffX + (side > 0 ? 0 : pJit), ey - 1, 2, 3, ink);
        px(ex - 2 + (pOffX > 0 ? 3 : 0), ey - 2, 0x88eeff); // watery specular

        // Big tear/sweat droplet flying off upper eye toward slope
        if (side * g.leanX < 0) {
          const int dropPhase = (g.now / 50) % 10;
          const int sx0 = ex + (g.leanX < 0 ? 5 : -5) + (g.leanX < 0 ? -dropPhase : dropPhase);
          const int sy0 = ey - 4 + dropPhase;
          if (sx0 >= 1 && sx0 < 63 && sy0 < 60) {
            px(sx0, sy0, sweat);
            px(sx0 + (g.leanX < 0 ? 1 : -1), sy0 - 1, 0xcff4ff);
          }
        }
      } else if (mood == GESTURE_HELD) {
        // 超萌水汪汪好奇大圆眼，瞳孔略微向下张望
        rect(ex - 3, ey - 3, 6, 7, milk);
        rect(ex - 1, ey, 2, 2, ink);
        px(ex - 2, ey - 2, 0xffffff); // 水汪汪大高光
        px(ex + 1, ey - 1, 0x80ffee);
      } else if (mood == GESTURE_FLIP || mood == GESTURE_TAP) {
        // Shocked huge round eyes
        rect(ex - 3, ey - 3, 6, 7, milk);
        rect(ex - 1, ey - 1, 2, 2, ink);
        px(ex - 2, ey - 2, mint);
      } else if (mood == GESTURE_DIZZY) {
        // Classic comic dizzy X X eyes
        for (int i = 0; i < 4; ++i) {
          px(ex - 2 + i, ey - 2 + i, mint);
          px(ex + 1 - i, ey - 2 + i, mint);
        }
      } else if (mood == GESTURE_SHAKE) {
        // Spinning spiral vortex pupil
        const float a = t / 110.0f + (side > 0 ? 1.57f : 0.0f);
        rect(ex - 3, ey - 2, 6, 5, 0x180e22);
        for (int i = 0; i < 3; ++i) {
          const float ang = a + float(i) * 2.094f;
          px(ex + int(cosf(ang) * 2.5f), ey + int(sinf(ang) * 2.0f), mint);
        }
        px(ex, ey, milk);
      } else if (mood == GESTURE_SETTLE) {
        // Relieved, happy peaceful closed crescent eyes ^ ^
        d.drawLine(ex - 2, ey,     ex,     ey - 2, color(mint));
        d.drawLine(ex,     ey - 2, ex + 2, ey,     color(mint));
        px(ex, ey - 1, milk);
      } else if (blink) {
        rect(ex - 2, ey, 4, 1, mint);
      } else {
        // Calm idle glance
        rect(ex - 2, ey - 2, 4, 6, mint);
        rect(ex - 1 + lookX, ey - 1 + lookY, 2, 2, ink);
        px(ex - 2, ey - 2, milk);
      }
    }

    // ---- Mouth with dramatic emotions ----------------------------------
    if (mood == GESTURE_LEAN) {
      // Screaming gaping "~O~" mouth with tiny panicked tooth
      rect(bx - 2, by + 3, 5, 4, ink);
      rect(bx - 1, by + 3, 3, 1, milk); // top tooth
      px(bx - 3, by + 4, ink); px(bx + 3, by + 4, ink); // wave corner
    } else if (mood == GESTURE_SETTLE) {
      // Warm contented smile
      rect(bx - 2, by + 5, 5, 1, milk);
      px(bx - 3, by + 4, milk); px(bx + 3, by + 4, milk);
      // "Phew~" exhaled breath bubble drifting away
      const int exhY = by + 2 - ((g.since / 90) % 8);
      const int exhX = bx + 6 + (g.since / 120);
      if (g.since < 1600 && exhX < 58) {
        rect(exhX, exhY, 2, 2, 0xd0f4ff);
        px(exhX + 1, exhY - 1, 0xffffff);
      }
    } else if (mood == GESTURE_TAP) {
      // Pouting indignant wavy mouth
      rect(bx - 2, by + 4, 4, 2, milk);
      px(bx - 3, by + 5, milk); px(bx + 2, by + 3, milk);
    } else if (mood == GESTURE_DIZZY) {
      // Dazed wavy slack mouth + wandering soul bubble!
      rect(bx - 2, by + 4, 4, 2, ink);
      px(bx, by + 5, milk);
      // Floating wandering soul wisp
      const int soulProg = (g.now / 70) % 18;
      const int sx = bx + 5 + int(sinf(float(soulProg) * 0.5f) * 2.5f);
      const int sy = by + 2 - soulProg;
      if (sy > 8) {
        rect(sx, sy, 3, 3, 0xb8e4ff);
        px(sx + 1, sy + 1, 0xffffff);
        px(sx + 1, sy + 3, 0x80c0ff); // tail
      }
    } else if (mood == GESTURE_HELD) {
      // 呆萌好奇小圆嘴 "o"
      rect(bx - 1, by + 4, 3, 3, ink);
      px(bx, by + 5, milk);
    } else {
      rect(bx - 1, by + 4, 3, 2, milk);
    }

    // ---- Feet: Kicking, planted or scrambling ---------------------------
    for (int side = -1; side <= 1; side += 2) {
      if (mood == GESTURE_LEAN) {
        // Low foot flailing in mid-air scrambling for footing; high foot wedged
        const bool isLowFoot = (side * g.leanX < 0);
        if (isLowFoot) {
          const int scramble = int(sinf(t / 40.0f) * 3.0f);
          rect(bx + side * 8 - 2, by + 10 + scramble, 5, 4, mint);
        } else {
          rect(bx + side * 8 - 2, by + 11, 5, 4, mint); // braced against cliff
        }
      } else if (mood == GESTURE_FLIP) {
        const int kick = int(sinf(t / 75.0f + float(side) * 1.5f) * 2.5f);
        rect(bx + side * 7 - 2, by + 10, 4, 4 + kick, mint);
      } else if (mood == GESTURE_HELD) {
        // 悬空荡秋千小短腿：交替前后微晃
        const float legSwing = sinf(t / 80.0f + float(side) * 1.57f) * 2.0f;
        rect(bx + side * 7 - 2, by + 10, 4, 4 + int(legSwing), mint);
        px(bx + side * 7 - 1, by + 13 + int(legSwing), peachLight); // 萌萌脚趾
      } else {
        rect(bx + side * 8 - 2, by + 11, 5, 4, mint);
        px(bx + side * 8 - 2 + (side < 0 ? 0 : 4), by + 14, 0); // toe notch
      }
    }

    // ---- Mood Extras & Dynamic FX ---------------------------------------
    if (mood == GESTURE_HELD) {
      // 两只白嫩软糯小爪子搭在下边框玻璃上，像猫咪扒窗台
      rect(bx - 9, by + 4, 4, 3, milk);
      rect(bx + 5, by + 4, 4, 3, milk);
      px(bx - 8, by + 5, peach); px(bx + 6, by + 5, peach);
    } else if (mood == GESTURE_TAP) {
      // Defensive paws raised to chest: "Stop poking me!"
      rect(bx - 6, by + 2, 4, 3, milk);
      rect(bx + 2, by + 2, 4, 3, milk);
      px(bx - 6, by + 4, peachDark); px(bx + 5, by + 4, peachDark);

      // Angered comic vein mark (💢) popping over head
      const int veinX = bx + 9, veinY = by - 14;
      rect(veinX - 2, veinY, 5, 1, 0xff3b66);
      rect(veinX, veinY - 2, 1, 5, 0xff3b66);
      px(veinX - 2, veinY - 2, 0xff3b66); px(veinX + 2, veinY + 2, 0xff3b66);
      px(veinX - 1, veinY - 1, 0xffa0b4);
    } else if (mood == GESTURE_SHAKE) {
      // Dynamic anime speed lines bursting
      const int d1 = int((g.now / 50) % 7);
      rect(2, 8 + d1, 2, 8, accent);   rect(60, 18 - d1, 2, 8, accent);
      rect(5, 34 - d1, 2, 6, accent);  rect(57, 30 + d1, 2, 6, accent);
    } else if (mood == GESTURE_DIZZY) {
      // 3 golden stars orbiting in 3D tilted oval halo
      for (int i = 0; i < 3; ++i) {
        const float a = t / 420.0f + float(i) * 2.0944f;
        const int sx = 32 + int(cosf(a) * 15.0f);
        const int sy = by - 18 + int(sinf(a) * 3.5f);
        px(sx,     sy,     0xffffff);
        px(sx - 1, sy,     mint); px(sx + 1, sy,     mint);
        px(sx,     sy - 1, mint); px(sx,     sy + 1, mint);
      }
    } else if (mood == GESTURE_SETTLE) {
      // Soothing hand patting chest
      rect(bx - 2, by + 3, 5, 3, milk);
      px(bx, by + 2, peachLight);
    }

    rect(0, 0, 64, 3, accent); // mood top status bar

    // ---- Subtitle Plate & Chinese Characters ----------------------------
    const uint8_t* line = CN_LINES[mood][(g.since / 2200) & 1];
    int count = 0;
    while (count < 4 && line[count] != CN_DONE) ++count;
    if (!count) return;
    const int width = count * 13 - 1;
    const int left = (64 - width) / 2;
    rect(left - 2, 50, width + 4, 14, 0x000000u); // crisp black plate
    for (int i = 0; i < count; ++i) {
      const uint16_t* rows = CN_MASKS[line[i]];
      for (int r = 0; r < 12; ++r)
        for (int col = 0; col < 12; ++col)
          if (rows[r] & (1u << (11 - col))) px(left + i * 13 + col, 51 + r, milk);
    }
  }
};
}
