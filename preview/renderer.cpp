#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <stdexcept>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include "display_renderer.h"
#include "state_protocol.h"

using namespace desk;
class Preview {
 public:
  Controller controller;
  ViewState view;
  Theme theme;
  uint32_t raw = 0, lastContact = 0, lastPresence = 0;
  uint64_t lockStarted = 0;
  uint32_t visiblePageSince = 0;
  Page lastVisiblePage = LOFT_PAGE;
  uint32_t sequence = 0;
  bool online = true, paused = false, presenceEnabled = true;
  bool wifiEverHadIp = true;
  Mode presence = FUN;
  // Reaction faces (MPU6050 emulation). 0 = off, else one of the five moods in
  // display_renderer.h. `motionMs` is a free-running clock (wobble, blink) and
  // `motionSince` counts from the moment the mood took over, so entrances play
  // exactly like they do on the panel.
  uint8_t motion = 0;
  int selectedLoftScene = -1;
  uint32_t motionMs = 0;
  uint32_t motionSince = 0;
  GFXcanvas16 canvas{64, 64};
  Preview() { std::srand(unsigned(std::time(nullptr))); preset("afterhours"); }

  // Same palette as the panel: a fresh hero-clock ink whenever the clock page
  // comes back, so the preview demonstrates the rotation the device performs.
  // A pinned hue (picked from the console) wins until it is released.
  void rotateInk() {
    if (clockInkPinned()) return;
    loftClockInk() = pickClockInk(uint32_t(std::rand()));
  }

  void presenceSignal() {
    Command command;
    snprintf(command.id, sizeof(command.id), "preview:%u", ++sequence);
    command.mode = presence; command.remainingMs = 12UL * 3600 * 1000;
    command.page = presence == WORK ? 1 : 0;  // mode home: quota / loft
    controller.remote(command, MAC_PRESENCE, raw);
    lastPresence = raw;
  }
  void advance(uint32_t ms) {
    // Small steps retain heartbeats during fast-forward when Mac is online.
    while (ms) {
      uint32_t step = std::min(ms, uint32_t(1000)); raw += step; ms -= step;
      controller.tick(raw);
      if (online && raw - lastContact >= 3000) { controller.contact(); lastContact = raw; }
      if (online && presenceEnabled && raw - lastPresence >= 60000) presenceSignal();
      controller.tick(raw);
    }
  }
  void preset(const std::string& name) {
    loftCycle() = LoftCycle();  // A preset is a fresh simulated boot.
    selectedLoftScene = -1;
    nightOverride() = 0;  // fresh scenario starts on the auto night schedule
    rotateInk();          // new scene, new clock hue
    controller = Controller(); controller.tick(0); raw = lastContact = lastPresence = visiblePageSince = 0;
    view = ViewState(); view.wifi = wifiEverHadIp = true; view.httpCode = 200;
    std::strcpy(view.ip, "192.0.2.2"); view.quota5h = 55; view.quotaWeek = 24; view.quotaStale = false;
    // The Mac preview has no radio: seed the SYSTEM page with the same shape
    // of values the panel reports (own SNTP clock, healthy link, free heap).
    view.rssi = -54; view.heapKb = 84; view.clockSrc = 1;
    controller.syncClock(1794793260LL);  // Mon 2026-11-16 09:41 Shanghai
    online = true; presence = WORK; presenceEnabled = name != "schedule";
    if (name == "afterhours" || name == "manual" || name == "nightclock") { controller.syncClock(1794825000LL); presence = FUN; }
    controller.contact(); if (presenceEnabled) presenceSignal(); controller.tick(raw);
    view.quotaAt = controller.epoch() - 60; view.reset5hAt = controller.epoch() + 2*3600 + 15*60;
    if (name == "low") view.quota5h = 12;
    else if (name == "stale") { view.quotaAt -= 7200; view.quotaStale = true; }
    else if (name == "noquota") view.quota5h = view.quotaWeek = -1;
    else if (name == "offline") { online = false; view.httpCode = -1; advance(61000); }
    else if (name == "cold") {
      controller = Controller(); controller.tick(0); online = false;
      view = ViewState();
      wifiEverHadIp = false;
    } else if (name == "manual") controller.button(1);
    else if (name == "status") {controller.button(2);controller.button(0);}
    else if (name != "work" && name != "afterhours" && name != "schedule" && name != "nightclock") throw std::runtime_error("unknown preset");
  }
  void apply(JsonDocument& input) {
    const uint32_t delta = input["delta_ms"] | 0U;
    motionMs += delta;
    if (motion) motionSince += delta;
    if (!paused) advance(delta);
    std::string op = input["op"] | "frame";
    if (op == "preset") preset(input["value"] | "afterhours");
    else if (op == "scene") {
      if (!input["value"].is<int>()) throw std::runtime_error("scene must be an integer");
      int scene = input["value"].as<int>();
      if (scene < -1 || scene > 7) throw std::runtime_error("scene must be -1..7");
      selectedLoftScene = scene;
      if (scene >= 0) { motion = 0; paused = false; }
    }
    else if (op == "button") controller.button(input["value"] | 0);
    else if (op == "skip") {
      // Fast-forward means the whole simulation, reaction faces included.
      advance(input["value"] | 0U);
      motionMs += input["value"] | 0U;
      if (motion) motionSince += input["value"] | 0U;
    }
    else if (op == "pause") paused = input["value"] | false;
    else if (op == "motion") {
      const uint8_t next = uint8_t(std::clamp(input["value"] | 0, 0, int(GESTURE_SETTLE)));
      if (next != motion) motionSince = 0;
      motion = next;
    }
    else if (op == "night") nightOverride() = input["value"] | 0;
    else if (op == "ink") {
      // "theme"/"auto" hands the hero clock back to the compiled colour and
      // restarts the rotation; "next" rotates one step; a hex value pins the
      // hue, which also keeps the frame tests deterministic.
      std::string value = input["value"] | "theme";
      if (value == "theme" || value == "auto") {
        clockInkPinned() = false;
        loftClockInk() = 0u;
      } else if (value == "next") {
        clockInkPinned() = false;
        rotateInk();
      } else if (value.size() == 7 && value[0] == '#') {
        loftClockInk() = uint32_t(std::strtoul(value.c_str() + 1, nullptr, 16));
        clockInkPinned() = true;
      }
    }
    else if (op == "mac") {
      std::string value = input["value"] | "disconnect";
      if (value == "disconnect") { online = false; view.httpCode = -1; }
      else {
        view.wifi = wifiEverHadIp = true;
        online = true; view.httpCode = 200; presenceEnabled = true;
        if (value == "lock" && !controller.macLocked) lockStarted = controller.uptime();
        controller.setMacLocked(value == "lock");
        presence = controller.macLocked ? FUN : WORK;
        controller.contact(); presenceSignal(); controller.tick(raw);
      }
    } else if (op == "page") {
      std::string value = input["value"] | "loft";
      if (value == "clock") value = "loft";  // deprecated alias
      Mode mode = value == "loft" ? FUN : (value == "quota" ? WORK : controller.mode);
      if (controller.mode != mode) controller.button(1);
      controller.button(2);
      int target = value == "quota" ? 1 : (value == "status" ? 2 : (value == "stock" ? 3 : 0));
      for (int i = 0; i < target; ++i) controller.button(0);
    } else if (op == "values") {
      if (input["wifi"].is<bool>()) {
        view.wifi = input["wifi"].as<bool>();
        if (view.wifi) wifiEverHadIp = true;
        else view.rssi = 0;
      }
      if (input["httpCode"].is<int>()) view.httpCode = input["httpCode"].as<int>();
      if (input["rssi"].is<int>()) view.rssi = int8_t(input["rssi"].as<int>());
      if (input["heapKb"].is<int>()) view.heapKb = uint16_t(input["heapKb"].as<int>());
      if (input["clockSrc"].is<int>()) view.clockSrc = uint8_t(input["clockSrc"].as<int>());
      if (input["mpu"].is<bool>()) view.mpuPresent = input["mpu"].as<bool>();
      if (input["mpuPose"].is<int>()) view.mpuPose = uint8_t(input["mpuPose"].as<int>());
      if (input["tiltDeg"].is<int>()) view.tiltDeg = uint8_t(input["tiltDeg"].as<int>());
      if (input["ota"].is<bool>()) view.otaInProgress = input["ota"].as<bool>();
      if (input["otaProgress"].is<int>()) view.otaProgressPct = uint8_t(input["otaProgress"].as<int>());
      if (input["ip"].is<const char*>()) snprintf(view.ip, sizeof(view.ip), "%s", input["ip"].as<const char*>());
      if (input["quota5h"].is<int>()) view.quota5h = input["quota5h"].as<int>();
      if (input["quotaWeek"].is<int>()) view.quotaWeek = input["quotaWeek"].as<int>();
      if (input["ageMinutes"].is<int>()) { view.quotaAt = controller.epoch() - input["ageMinutes"].as<int>() * 60; view.quotaStale = input["ageMinutes"].as<int>() > 5; }
      if (input["resetMinutes"].is<int>()) view.reset5hAt = controller.epoch() + input["resetMinutes"].as<int>() * 60;
      if (input["reset5hAt"].is<int64_t>()) view.reset5hAt = input["reset5hAt"].as<int64_t>();
      if (input["epoch"].is<int64_t>()) {
        // epoch 0 is a test hook simulating the pre-sync boot clock.
        if (input["epoch"].as<int64_t>() == 0) controller.hasClock = false;
        else controller.syncClock(input["epoch"].as<int64_t>());
      }
      if (input["message"].is<const char*>()) {
        snprintf(view.message, sizeof(view.message), "%s", input["message"].as<const char*>());
        view.messageUntil = controller.uptime() + 60000; view.messageStarted = controller.uptime();
      }
      JsonVariantConst stock = input["stock"];
      if (stock.is<JsonObjectConst>()) {
        view.stock = StockState();
        view.stock.available = stock["available"] | false;
        const float gold = stock["gold"]["price"] | 0.0f;
        if (std::isfinite(gold) && gold > 0 && gold < 10000) view.stock.goldPrice = gold;
        view.stock.goldClosed = stock["gold"]["closed"] | false;
        view.stock.goldUsdClosed = stock["gold_usd"]["closed"] | false;
        const float goldUsd = stock["gold_usd"]["price"] | 0.0f;
        if (std::isfinite(goldUsd) && goldUsd > 0 && goldUsd < 100000) view.stock.goldUsd = goldUsd;
        auto readQ = [](JsonVariantConst q, StockQuote& target) {
          if (!q.is<JsonObjectConst>()) return;
          target.price = q["price"] | target.price;
          target.change = q["change"] | target.change;
          target.pct = q["pct"] | target.pct;
          target.closed = q["closed"] | false;
          int ref = q["trendRef"] | -1;
          if (ref >= 0 && ref <= 18) target.trendRef = ref;
          JsonArrayConst arr = q["trend"];
          JsonArrayConst positions = q["trendX"];
          if (arr) {
            target.trendCount = 0;
            for (JsonVariantConst v : arr) {
              if (target.trendCount < 20) {
                target.trend[target.trendCount++] = v.as<uint8_t>();
              }
            }
          }
          if (positions && positions.size() == target.trendCount) {
            target.timedTrend = true;
            for (uint8_t i = 0; i < target.trendCount; ++i) {
              int x = positions[i] | -1;
              if (x < 0 || x > 57 || (i && x < target.trendX[i-1])) {
                target.timedTrend = false; break;
              }
              target.trendX[i] = x;
            }
          }
        };
        readQ(stock["sh"], view.stock.sh);
        readQ(stock["nasdaq"], view.stock.nasdaq);
      }
      controller.tick(raw);
    }
    JsonVariantConst settings = input["theme"];
    if (settings.is<JsonObjectConst>()) {
      theme.brightness = settings["brightness"] | theme.brightness;
      theme.animationStepMs = settings["animationStepMs"] | theme.animationStepMs;
      theme.sceneDetail = settings["sceneDetail"] | theme.sceneDetail;
      auto readColor = [&](const char* key, uint32_t& field) {
        const char* hex = settings[key] | "";
        if (hex[0] == '#' && std::strlen(hex) == 7) field = std::stoul(hex + 1, nullptr, 16);
      };
      readColor("nightColor", theme.nightColor); readColor("accentColor", theme.accentColor);
      readColor("clockColor", theme.clockColor); readColor("quotaColor", theme.quotaColor);
    }
  }
  void output() {
    JsonDocument out;
    const char* names[] = {"loft", "quota", "status", "stock"};
    Page active = currentPage(controller, view);
    if (controller.macLocked) {
      const uint32_t phase=(controller.uptime()-lockStarted)%(FUN_LOFT_MS+STOCK_PAGE_MS);
      active=phase<FUN_LOFT_MS ? LOFT_PAGE : STOCK_PAGE;
    } else if (!wifiEverHadIp && controller.source != MANUAL) {
      active = STATUS_PAGE;
    }
    if (active != lastVisiblePage) {
      lastVisiblePage = active;
      visiblePageSince = raw;
      if (active == LOFT_PAGE) rotateInk();  // new hue each time the clock returns
    }
    // Same RGB565 canvas and same hex encoding for pages and personas, so the
    // JS renderer needs no special case beyond picking the key.
    static const char hex[] = "0123456789abcdef";
    auto encode = [&]() {
      std::string pixels; pixels.resize(64 * 64 * 4);
      const uint16_t* buffer = canvas.getBuffer();
      for (int i = 0; i < 4096; ++i) for (int shift = 0; shift < 4; ++shift)
        pixels[i * 4 + shift] = hex[(buffer[i] >> ((3 - shift) * 4)) & 15];
      return pixels;
    };
    for (int page = 0; page < 4; ++page) {
      ViewState frameView = view;
      frameView.pageShownAt = page == int(active) ? visiblePageSince : 0;
      frameView.loftScenePreview = selectedLoftScene;
      Renderer<GFXcanvas16> renderer(canvas, controller, frameView, theme);
      renderer.draw(Page(page), canvas.getBuffer());
      out["frames"][names[page]] = encode();
    }
    const char* activeName = names[active];
    if (motion) {
      // Mirrors the sketch's override branch: the face owns the whole panel
      // until the gesture ends, whatever page the carousel is on.
      Renderer<GFXcanvas16> renderer(canvas, controller, view, theme);
      Gesture g;
      g.mood = motion; g.now = motionMs; g.since = motionSince;
      if (motion == GESTURE_LEAN) {
        // No accelerometer in a browser, so sweep the gravity vector the way a
        // hand would and let the face follow it.
        g.leanX = int8_t(78.0 * std::sin(double(motionMs) / 620.0));
        g.leanY = int8_t(42.0 * std::cos(double(motionMs) / 880.0));
        out["lean"][0] = int(g.leanX); out["lean"][1] = int(g.leanY);
      }
      renderer.drawGesture(g);
      out["frames"]["motion"] = encode();
      activeName = "motion";
    }
    out["active"] = activeName;
    auto state = out["state"].to<JsonObject>();
    state["mode"] = controller.mode == WORK ? "work" : "fun";
    state["source"] = controller.source == MANUAL ? "manual" : (controller.source == MAC_PRESENCE ? "mac" : "auto");
    state["page"] = int(active);
    state["online"] = controller.serviceOnline; state["macConnected"] = online;
    state["macLocked"] = controller.macLocked;
    state["manualSeconds"] = controller.manualRemainingMs() / 1000;
    state["epoch"] = controller.epoch(); state["uptimeMs"] = controller.uptime();
    state["quota5h"] = view.quota5h; state["quotaWeek"] = view.quotaWeek;
    state["ageMinutes"] = view.quotaAt ? std::max(int64_t(0), (controller.epoch() - view.quotaAt) / 60) : 0;
    state["paused"] = paused;
    state["motion"] = motion;
    state["loftScene"] = selectedLoftScene >= 0 ? selectedLoftScene : loftSceneIndex();
    state["loftSceneSelected"] = selectedLoftScene;
    char inkHex[8];
    snprintf(inkHex, sizeof inkHex, "#%06x", unsigned(loftClockInk()));
    state["ink"] = loftClockInk() ? inkHex : "theme";
    state["inkPin"] = clockInkPinned();
    state["resetMinutes"] = view.reset5hAt ? std::max(int64_t(0),(view.reset5hAt-controller.epoch())/60) : 0;
    serializeJson(out, std::cout); std::cout << '\n' << std::flush;
  }
};
int main() {
  Preview preview;
  std::string line;
  while (std::getline(std::cin, line)) {
    try {
      JsonDocument input;
      if (deserializeJson(input, line)) throw std::runtime_error("invalid JSON");
      preview.apply(input); preview.output();
    } catch (const std::exception& exc) {
      JsonDocument error; error["error"] = exc.what(); serializeJson(error, std::cout); std::cout << '\n' << std::flush;
    }
  }
}
