# Animated Minion Eye – ESP32-C3 Super Mini + Waveshare 1.28" LCD (GC9A01)

## Wiring

| Waveshare LCD pin | ESP32-C3 Super Mini | Notes                    |
|-------------------|---------------------|--------------------------|
| VCC               | 3V3                 | 3.3 V (do not use 5 V)   |
| GND               | GND                 |                          |
| DIN (MOSI)        | GPIO6               | SPI data                 |
| CLK (SCK)         | GPIO4               | SPI clock                |
| CS                | GPIO7               | Chip select              |
| DC                | GPIO1               | Data/command             |
| RST               | GPIO21              | Held HIGH in code (acts as 3V3); no hardware reset |
| BL                | GPIO3               | Backlight (PWM)          |

Pins avoided on purpose: GPIO2/8/9 (strapping; 8 = onboard LED, 9 = BOOT button),
GPIO18/19 (USB D-/D+). Pins are defined at the top of `src/main.cpp`.

## Flashing (Windows)

1. Install **VS Code** and the **PlatformIO IDE** extension.
2. Open this folder (`File > Open Folder`). PlatformIO downloads the platform and
   LovyanGFX on first build.
3. Plug the Super Mini into the PC with a **data-capable USB-C cable**.
4. If it is not detected (no new COM port in Device Manager), enter bootloader mode:
   hold **BOOT**, tap **RESET**, release **BOOT**.
5. Click the **→ Upload** button in the bottom status bar (or run `pio run -t upload`).
6. Tap **RESET** afterwards. The eye should start animating.
7. Optional: **Serial Monitor** (plug icon) at 115200 baud prints the free heap.

If the upload picks the wrong port, add `upload_port = COMx` to `platformio.ini`.

## Troubleshooting

- **No serial output**: `ARDUINO_USB_CDC_ON_BOOT=1` is set; reopen the monitor after reset.
- **Colours inverted** (blue skin): set `cfg.invert = false` in `main.cpp`.
- **Red/blue swapped**: set `cfg.rgb_order = true`.
- **Image shifted/garbled**: lower `cfg.freq_write` to `40000000`.
- **Blank screen (backlight only)**: on boot the screen flashes red, green, blue, white.
  If you see nothing, re-check DIN/CLK/CS/DC/RST wiring (DIN = GPIO6, CLK = GPIO4) and
  that all grounds are shared. Open the Serial Monitor for boot messages.
- **Sprite alloc failed**: the 240×240 16-bit canvas needs ~112 KB of free heap.
