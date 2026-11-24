#pragma once
#include <cstdint>
namespace desk {
// No sockets here: this policy runs in an independent supervisor task even
// while HTTP is waiting on a slow client. Unsigned deltas survive millis wrap.
class WifiRecovery {
 public:
  enum Action { NONE, CONNECT, RESET_STATION, RESET_RADIO, REBOOT };
  Action update(bool connected, uint32_t now) {
    if (connected) { seenConnection = true; offline = false; stage = 0; return NONE; }
    if (!offline) {
      offline = true; since = lastAttempt = now; stage = 0;
      if (seenConnection) return CONNECT;  // begin recovery on the first check
    }
    if (seenConnection && now - since >= 90000u) return REBOOT;
    if (now - lastAttempt < 10000u) return NONE; // let association/DHCP finish
    lastAttempt = now;
    ++stage;
    if (stage % 3u == 0) return RESET_RADIO;
    if (stage % 3u == 2) return RESET_STATION;
    return CONNECT;
  }
 private:
  bool offline = false, seenConnection = false;
  uint32_t since = 0, lastAttempt = 0, stage = 0;
};

// HTTP watchdog liveness: /ota/stream parks netTask in one long handler, so
// its loop beat (webLastBeat) freezes during a healthy transfer. OTA refreshes
// otaLastActivity on every frame; use the newer of the two so a live OTA is
// never mistaken for a wedged HTTP task. otaAct==0 means "no OTA in flight"
// and must not enter the wrap-safe millis comparison (it is a sentinel, not a
// timestamp). Once the OTA ends the producer clears otaAct to 0, so this
// returns beat again and the watchdog resumes its strict budget automatically.
inline uint32_t httpAliveBeat(uint32_t beat, uint32_t otaAct) {
  return (otaAct && int32_t(otaAct - beat) > 0) ? otaAct : beat;
}
}
