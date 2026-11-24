#pragma once
// Copy to config.h (gitignored). The display also works with no configuration.
#define DESK_WIFI_SSID "your-2.4GHz-wifi"
#define DESK_WIFI_PASS "your-password"
#define DESK_MAC_HOST "http://192.0.2.1:8765"
// Start server/server.py once, then copy the contents of .local/token here.
#define DESK_API_TOKEN "paste-token-here"
#define DESK_TIMEZONE_OFFSET (8 * 3600)
#define DESK_WORK_START_MINUTE (9 * 60)
#define DESK_WORK_END_MINUTE (18 * 60)
#define DESK_WORK_DAYS 0x3e
// No physical button needed; Mac commands and local automatic rules work.
#define DESK_BUTTON_ENABLED 0
