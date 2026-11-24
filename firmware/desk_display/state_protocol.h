#pragma once
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include "display_controller.h"

namespace desk {
struct StockQuote {
  float price = 0;
  float change = 0;
  float pct = 0;
  // The numbers are the last close, not a live session: the panel dims its
  // market tag instead of pretending the market is open.
  bool closed = false;
  int8_t trendRef = -1;
  uint8_t trendCount = 0;
  uint8_t trend[20] = {};
  bool timedTrend = false;
  uint8_t trendX[20] = {};
};

struct StockState {
  bool available = false;
  float goldPrice = 0;
  bool goldClosed = false;  // last published Au99.99 close, market shut
  bool goldUsdClosed = false;  // spot gold stopped ticking (weekend)
  float goldUsd = 0;
  StockQuote sh;
  StockQuote nasdaq;
};

struct Snapshot {
  bool valid = false;
  bool wifi = false;
  int8_t macLocked = -1;
  int httpCode = 0;
  uint32_t requestStarted = 0;
  int64_t epoch = 0;
  Command manual, presence, automatic;
  int quota5h = -1, quotaWeek = -1;
  int64_t quotaAt = 0, reset5hAt = 0;
  int brightness = -1;
  uint64_t brightnessRevision = 0;
  uint32_t brightnessAgeMs = 0;
  uint64_t receivedAt = 0;
  uint64_t resumeRevision = 0;
  uint32_t resumeAgeMs = 0;
  bool quotaStale = true;
  char message[161] = {};
  uint32_t messageMs = 0;
  char ip[16] = {};
  StockState stock;
};

inline void readCommand(JsonVariantConst obj, Command& command) {
  if (!obj.is<JsonObjectConst>()) return;
  const char* mode = obj["mode"] | "";
  const char* id = obj["id"] | "";
  if ((std::strcmp(mode, "work") && std::strcmp(mode, "fun")) || !id[0] || std::strlen(id) >= sizeof(command.id)) return;
  uint32_t remaining = obj["remaining_ms"] | 0UL;
  if (!remaining || remaining > 86400000UL) return;
  std::strcpy(command.id, id);
  command.mode = std::strcmp(mode, "work") == 0 ? WORK : FUN;
  int page = obj["page"] | 0;
  command.page = page >= 0 && page < PAGE_COUNT ? page : 0;
  command.remainingMs = remaining;
  command.ageMs = obj["age_ms"] | 0UL;
}

inline int readPercent(JsonVariantConst value) {
  if (!value.is<float>()) return -1;
  float pct = value.as<float>();
  if (!std::isfinite(pct) || pct < 0 || pct > 100) return -1;
  return int(std::lround(pct));
}

inline bool decodeSnapshot(const char* body, size_t count, Snapshot& next) {
  if (count > 4096) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body, count) || doc["protocol"] != 2 ||
      !doc["data"].is<JsonObject>() || !doc["control"].is<JsonObject>()) return false;
  int64_t epoch = doc["epoch"] | int64_t(0);
  if (epoch < 1700000000LL || epoch >= 4102444800LL) return false;
  next.epoch = epoch;
  if (doc["mac_locked"].is<bool>()) next.macLocked = doc["mac_locked"].as<bool>();
  next.resumeRevision = doc["resume_revision"] | uint64_t(0);
  next.resumeAgeMs = doc["resume_age_ms"] | uint32_t(0);
  readCommand(doc["control"]["manual"], next.manual);
  readCommand(doc["control"]["presence"], next.presence);
  readCommand(doc["control"]["auto"], next.automatic);
  JsonVariantConst quota = doc["data"]["quota"];
  if (quota["available"] | false) {
    next.quota5h = readPercent(quota["five_hour"]["remaining_percent"]);
    next.quotaWeek = readPercent(quota["weekly"]["remaining_percent"]);
    double stamp = quota["snapshot_at_epoch"] | 0.0;
    if (std::isfinite(stamp) && stamp >= 1700000000.0 && stamp < 4102444800.0) next.quotaAt = int64_t(stamp);
    next.quotaStale = quota["stale"] | true;
    int64_t reset = quota["five_hour"]["resets_at_epoch"] | int64_t(0);
    if (reset >= 1700000000LL && reset < 4102444800LL) next.reset5hAt = reset;
  }
  int brightness = doc["data"]["brightness"] | -1;
  if (brightness >= 8 && brightness <= 160) {
    next.brightness = brightness;
    next.brightnessRevision = doc["data"]["brightness_revision"] | uint64_t(0);
    next.brightnessAgeMs = doc["data"]["brightness_age_ms"] | uint32_t(0);
  }
  JsonVariantConst stock = doc["data"]["stock"];
  if (stock["available"] | false) {
    next.stock.available = true;
    const float gold = stock["gold"]["price"] | 0.0f;
    if (std::isfinite(gold) && gold > 0 && gold < 10000) next.stock.goldPrice = gold;
    next.stock.goldClosed = stock["gold"]["closed"] | false;
    const float goldUsd = stock["gold_usd"]["price"] | 0.0f;
    if (std::isfinite(goldUsd) && goldUsd > 0 && goldUsd < 100000) next.stock.goldUsd = goldUsd;
    next.stock.goldUsdClosed = stock["gold_usd"]["closed"] | false;
    auto readQ = [](JsonVariantConst q, StockQuote& target) {
      if (!q.is<JsonObjectConst>()) return;
      target.price = q["price"] | 0.0f;
      target.change = q["change"] | 0.0f;
      target.pct = q["pct"] | 0.0f;
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
    readQ(stock["sh"], next.stock.sh);
    readQ(stock["nasdaq"], next.stock.nasdaq);
  }
  const char* input = doc["data"]["message"] | "";
  int n = 0;
  while (*input && n < 160) {
    unsigned char c = *input++;
    if (c >= 32 && c < 127) next.message[n++] = c;
    else if (c >= 128 && (c & 0xc0) != 0x80) next.message[n++] = '?';
  }
  next.message[n] = 0;
  uint32_t messageMs = doc["data"]["message_remaining_ms"] | 0UL;
  next.messageMs = messageMs <= 86400000UL ? messageMs : 0;
  next.valid = true;
  return true;
}
}
