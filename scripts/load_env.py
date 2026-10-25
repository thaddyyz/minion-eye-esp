"""PlatformIO pre-build script: read settings from a .env file and pass them to the C++ code.

.env lines look like  KEY=value  (optionally quoted); blank lines and # comments are ignored.
Only the keys listed in KEYS are used; anything missing falls back to the default in
include/wifi_config.h.
"""
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

KEYS = ("WIFI_SSID", "WIFI_PASS", "AP_SSID", "AP_PASS", "MDNS_NAME")

env_path = os.path.join(env["PROJECT_DIR"], ".env")  # noqa: F821
values = {}

if os.path.isfile(env_path):
    with open(env_path, encoding="utf-8-sig") as f:   # utf-8-sig tolerates a Windows BOM
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            if key in KEYS:
                values[key] = value
    print("load_env: read %s (%s)" % (env_path, ", ".join(sorted(values)) or "no known keys"))
else:
    print("load_env: no .env file found - WiFi will use the eye's own access point")

for key, value in values.items():
    env.Append(CPPDEFINES=[(key, env.StringifyMacro(value))])  # noqa: F821
