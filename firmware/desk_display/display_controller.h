#pragma once
#include <stdint.h>
#include <string.h>

// No Arduino dependency: the same state machine runs in host regression tests.
namespace desk {
enum Mode : uint8_t { FUN, WORK };
enum Source : uint8_t { AUTOMATIC, MAC_PRESENCE, MANUAL };
static const uint32_t MANUAL_MS = 30UL * 60 * 1000;
static const uint32_t OFFLINE_MS = 60UL * 1000;
static const uint8_t PAGE_COUNT = 4;

struct Command {
  char id[64] = {};
  Mode mode = FUN;
  uint8_t page = 0;
  uint32_t remainingMs = 0;
  uint32_t ageMs = 0;
};

struct Lease {
  bool active = false;
  Mode mode = FUN;
  uint8_t page = 0;
  uint64_t until = 0;
};

class Controller {
 public:
  Mode mode = FUN;
  uint8_t page = 0;
  Source source = AUTOMATIC;
  bool serviceOnline = false;
  bool macLocked = false;
  bool hasClock = false;
  // Local time defaults to Shanghai; schedule can be configured in config.h.
  int timezoneOffset = 8 * 3600;
  uint16_t workStartMinute = 9 * 60;
  uint16_t workEndMinute = 18 * 60;
  uint8_t workDays = 0x3e;  // Sunday bit 0, Monday-Friday bits 1..5.

  void tick(uint32_t rawMillis) {
    if (!ticked) { lastRaw = rawMillis; ticked = true; }
    uint32_t delta = rawMillis - lastRaw;
    lastRaw = rawMillis;
    now += delta;
    if (hasClock) epochMillis += delta;
    serviceOnline = !macLocked && received && now - lastContact < 25000;
    resolve();
  }

  // Soft clock sync. epochMillis advances continuously via tick()'s millis()
  // delta; the network epoch is whole-second only and arrives ~every 750ms,
  // so hard-setting it every time zeroes the sub-second phase and makes the
  // seconds readout stall then jump. Adopt network time only on first lock or
  // on real drift (>1.5s). Returns true when the clock was actually re-stamped.
  bool syncClock(int64_t epochSeconds) {
    if (epochSeconds < 1700000000LL || epochSeconds >= 4102444800LL) return false;
    const int64_t target = epochSeconds * 1000LL;
    if (!hasClock) { epochMillis = target; hasClock = true; return true; }
    const int64_t drift = target - epochMillis;
    if (drift > 1500LL || drift < -1500LL) { epochMillis = target; return true; }
    return false;
  }
  int64_t epoch() const { return hasClock ? epochMillis / 1000 : 0; }
  uint64_t uptime() const { return now; }
  uint32_t manualRemainingMs() const {
    return manual.active && manual.until > now ? uint32_t(manual.until - now) : 0;
  }

  void contact() { received = true; lastContact = now; serviceOnline = !macLocked; }

  void setMacLocked(bool locked) {
    if (macLocked == locked) return;
    macLocked = locked;
    if (locked) {
      manual.active = false;
      serviceOnline = false;
    }
    resolve();
  }

  void resumeCarousel() {
    manual.active = false;
    localInput = true;
    lastLocalInput = now;
    resolve();
  }

  // Snapshots carry identities, not authoritative current pages. Replaying an
  // old snapshot must never undo a local action or renew an expired override.
  void remote(const Command& command, Source kind, uint32_t requestStarted) {
    if (!command.id[0] || !command.remainingMs) return;
    char* seen = kind == MANUAL ? seenManual : (kind == MAC_PRESENCE ? seenPresence : seenAuto);
    if (strcmp(seen, command.id) == 0) return;
    strncpy(seen, command.id, 63); seen[63] = 0;
    if (macLocked && kind == MANUAL) return;
    if (kind == MANUAL && localInput) {
      // A local press during the request wins; an older server command also
      // stays consumed on reconnect. A later explicit Mac manual command wins.
      if (uint32_t(lastRaw - requestStarted) >= uint32_t(now - lastLocalInput) ||
          command.ageMs >= now - lastLocalInput) return;
    }
    Lease& target = kind == MANUAL ? manual : (kind == MAC_PRESENCE ? presence : automatic);
    target.active = true;
    target.mode = command.mode;
    target.page = command.page % PAGE_COUNT;
    target.until = now + command.remainingMs;
    if (kind == MANUAL) localInput = false;
    resolve();
    if (kind == MANUAL) page = target.page;
  }

  void button(uint8_t action) {  // 0 next, 1 toggle mode, 2 home
    if (macLocked) return;
    resolve();
    Mode nextMode = action == 1 ? (mode == WORK ? FUN : WORK) : mode;
    // Home is the mode's front page: quota at work, loft at play.
    uint8_t nextPage = action == 0 ? (page + 1) % PAGE_COUNT : (nextMode == WORK ? 1 : 0);
    manual = {true, nextMode, nextPage, now + MANUAL_MS};
    localInput = true; lastLocalInput = now;
    mode = nextMode; page = nextPage; source = MANUAL;
  }

 private:
  uint64_t now = 0, lastContact = 0, lastLocalInput = 0;
  uint32_t lastRaw = 0;
  int64_t epochMillis = 0;
  bool ticked = false, received = false, localInput = false;
  Lease manual, presence, automatic;
  char seenManual[64] = {}, seenPresence[64] = {}, seenAuto[64] = {};

  Mode scheduled() const {
    if (!hasClock) return FUN;
    int64_t local = epoch() + timezoneOffset;
    int weekday = (local / 86400 + 4) % 7;
    int minute = (local % 86400) / 60;
    return (workDays & (1 << weekday)) && minute >= workStartMinute && minute < workEndMinute ? WORK : FUN;
  }

  void resolve() {
    if (macLocked) {
      mode = FUN; page = 0; source = MAC_PRESENCE;
      return;
    }
    Source nextSource = AUTOMATIC;
    Mode next = scheduled();
    if (automatic.active && automatic.until > now) next = automatic.mode;
    // No server at boot, or a stopped/sleeping Mac: keep the toy alive.
    if ((!received && now >= OFFLINE_MS) || (received && now - lastContact >= OFFLINE_MS)) next = FUN;
    else if (presence.active && presence.until > now) { next = presence.mode; nextSource = MAC_PRESENCE; }
    if (manual.active && manual.until > now) { next = manual.mode; nextSource = MANUAL; }
    if (next != mode || nextSource != source) {
      mode = next; source = nextSource;
      // Non-manual leases land on the mode home page; manual commands carry
      // their own absolute page (loft 0, quota 1, status 2, stock 3).
      page = nextSource == MANUAL ? manual.page : (next == WORK ? 1 : 0);
    }
  }
};

// Polling debounce avoids ISR races and duplicate long-press/release events.
class Button {
 public:
  // -1 no event, 0 short, 1 long, 2 double; short waits for double-click window.
  int update(bool down, uint32_t now) {
    if (down != raw) { raw = down; changed = now; }
    int result = -1;
    if (now - changed >= 30 && stable != raw) {
      stable = raw;
      if (stable) { pressed = now; held = false; }
      else if (!held) {
        if (pending && now - released <= 300) { pending = false; result = 2; }
        else { pending = true; released = now; }
      }
    }
    if (stable && !held && now - pressed >= 800) {
      held = true; pending = false; result = 1;
    }
    if (!stable && pending && now - released > 300) { pending = false; result = 0; }
    return result;
  }
 private:
  bool raw = false, stable = false, held = false, pending = false;
  uint32_t changed = 0, pressed = 0, released = 0;
};
}
