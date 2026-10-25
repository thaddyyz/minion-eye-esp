#pragma once

// Defaults only. Put your real settings in the .env file in the project root
// (copy .env.example to .env); scripts/load_env.py passes them in at build time.

// Home WiFi to join. Empty means: skip this and always start the eye's own access point.
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// Fallback access point, started when WIFI_SSID is empty or the connection fails.
// Join it with your phone/PC, then open http://192.168.4.1
#ifndef AP_SSID
#define AP_SSID "MinionEye"
#endif
#ifndef AP_PASS
#define AP_PASS "minion123"   // at least 8 characters
#endif

// With a home network you can also use http://minioneye.local
#ifndef MDNS_NAME
#define MDNS_NAME "minioneye"
#endif
