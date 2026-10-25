// Animated Minion eye
// ESP32-C3 Super Mini + Waveshare 1.28" round LCD (GC9A01, 240x240)
// Only the parts of the screen that changed are redrawn and sent to the display.
// A small web page lets you pick the mood; with no mood chosen it cycles randomly.

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <math.h>
#include "wifi_config.h"

// ---------------- Pins (see README.md) ----------------
constexpr int PIN_SCLK = 4;
constexpr int PIN_MOSI = 6;
constexpr int PIN_CS   = 7;
constexpr int PIN_DC   = 1;
constexpr int PIN_RST  = -1;     // display RST is wired to GPIO21 (held high), see setup()
constexpr int PIN_RST_HIGH = 21; // acts as a 3V3 source for the display RST line
constexpr int PIN_BL   = 3;

// SPI clock to the display. 80 MHz is the ESP32-C3 maximum; if the picture glitches or
// goes blank with long jumper wires, drop back to 40000000.
constexpr uint32_t SPI_WRITE_HZ = 40000000;

// true = only changed regions are redrawn; false = whole screen every frame (for troubleshooting)
constexpr bool PARTIAL_UPDATES = true;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = SPI_WRITE_HZ;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = true;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_SCLK;
      cfg.pin_mosi = PIN_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc   = PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = PIN_CS;
      cfg.pin_rst  = PIN_RST;
      cfg.pin_busy = -1;
      cfg.memory_width  = 240;
      cfg.memory_height = 240;
      cfg.panel_width   = 240;
      cfg.panel_height  = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = false;
      cfg.invert   = true;   // GC9A01 modules need inversion
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_BL;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 1;  // ESP32-C3 only has LEDC channels 0-5
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX lcd;
LGFX_Sprite bg(&lcd);      // static layer: skin, goggle ring, bolts, shaded eyeball (full screen)
LGFX_Sprite strip(&lcd);   // working buffer: a band of the screen is composed here, then sent
LGFX_Sprite irisTex(&lcd); // pre-rendered iris

constexpr int STRIP_H = 48;
int gOY = 0;               // screen row that is row 0 of the strip

// ---------------- Geometry ----------------
constexpr int   CX = 120, CY = 120;
constexpr float CENTRE = 119.5f;        // pixel-centre coordinate of the screen middle
constexpr float R_OUT   = 117.0f;       // outer edge of the goggle ring
constexpr int   R_GLASS = 91;           // inner edge of the ring = visible eye area
constexpr float BOLT_RADIUS = 114.0f;   // bolts sit on the outer rim of the ring
constexpr int   R_IRIS  = 47;
constexpr float MAX_OFFSET = R_GLASS - R_IRIS - 4;  // keep the iris inside the glass
constexpr float LX = -0.7071f, LY = -0.7071f;       // light comes from the top-left
constexpr int   LID_SHADE_ROWS = 14;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint16_t c565(int r, int g, int b) {
  r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }
static inline float quarter(float v) { return roundf(v * 4.0f) / 4.0f; }

struct V3 { float r, g, b; };
static inline V3 mix(V3 a, V3 b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
static inline uint16_t toC(V3 c) { return c565((int)c.r, (int)c.g, (int)c.b); }

// Drawing helpers: take screen coordinates, draw into the strip
static inline void sPixel(int x, int y, uint16_t c) { strip.drawPixel(x, y - gOY, c); }
static inline void sVLine(int x, int y, int h, uint16_t c) { strip.drawFastVLine(x, y - gOY, h, c); }
static inline void sCircle(int x, int y, int r, uint16_t c) { strip.fillCircle(x, y - gOY, r, c); }
static inline void sRectAlpha(int x, int y, int w, int h, uint8_t a, uint16_t c) {
  strip.fillRectAlpha(x, y - gOY, w, h, a, c);
}

// Per-column data for the eyelids (filled once in setup)
int16_t  colHalf[240];                       // half-height of the glass circle at each column (-1 = outside)
uint16_t shadeCnt[240];                      // rows, counted from the circle edge, that lie in the ring's shadow
uint16_t shadeOff[240];                      // where each column's entries start in shadeTop/shadeBot
uint8_t* shadeTop = nullptr;                 // brightness 0..255 per shaded pixel, counted from the column top
uint8_t* shadeBot = nullptr;                 // same, counted from the column bottom
constexpr int LID_R = 247, LID_G = 209, LID_B = 38;

static inline uint16_t lidShadeCol(uint8_t f) {
  return c565(LID_R * f / 255, LID_G * f / 255, LID_B * f * 7 / (255 * 8));
}
uint16_t C_LID, C_LID_EDGE, C_SHADOW;

// Strength of the soft shadow the lids cast on the eyeball: 0 = none, 1 = the original strong look
constexpr float LID_SHADOW_STRENGTH = 0.25f;

// ---------------- Static background ----------------
void renderBackground() {
  bg.fillScreen(0);
  const float Rin = R_GLASS;

  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      float dx = x + 0.5f - 120.0f, dy = y + 0.5f - 120.0f;
      float d = sqrtf(dx * dx + dy * dy);
      if (d > 121.0f) continue;
      float inv = d > 0.001f ? 1.0f / d : 0.0f;
      float cosA = (dx * LX + dy * LY) * inv;   // 1 = towards the light

      // Skin outside the ring
      float dn = d / 120.0f;
      float sk = 1.0f - 0.10f * dn * dn + 0.05f * cosA * dn;
      V3 skin = {250 * sk, 212 * sk, 42 * sk};

      // Metal ring: treat the cross-section as a half-round tube lit from the top-left
      float t = clampf((d - Rin) / (R_OUT - Rin), 0, 1);
      float s = (t - 0.5f) * 2.0f;                       // -1 inner edge .. +1 outer edge
      float z = sqrtf(fmaxf(0.0f, 1.0f - s * s));
      float lam = clampf(z * 0.72f + s * cosA * 0.68f, 0, 1.1f);
      float v = 0.28f + 0.78f * lam;
      if (t < 0.07f) v *= 0.6f + 0.4f * t / 0.07f;       // dark outlines at both edges
      else if (t > 0.93f) v *= 0.6f + 0.4f * (1.0f - t) / 0.07f;
      float l2 = lam * lam, l6 = l2 * l2 * l2;
      float spec = l6 * 70.0f;
      V3 ring = {196 * v + spec, 202 * v + spec, 214 * v + spec};

      // Eyeball: white, shaded towards the edge and bottom-right, shadowed by the ring
      float u = d / Rin;
      float k = 1.0f - 0.30f * u * u * u - 0.10f * fmaxf(0.0f, (dx + dy) / (1.414f * Rin));
      float sh = smooth01((Rin - d) / 16.0f);
      k *= 1.0f - (1.0f - sh) * (0.14f + 0.30f * fmaxf(0.0f, cosA));
      V3 glass = {252 * k, 250 * k, 244 * k * (1.0f - 0.05f * u * u)};

      V3 outer = mix(skin, ring, clampf(R_OUT - d + 0.5f, 0, 1));
      V3 col   = mix(glass, outer, clampf(d - Rin + 0.5f, 0, 1));
      bg.drawPixel(x, y, toC(col));
    }
  }

  // Bolts on the ring
  for (int i = 0; i < 8; i++) {
    float a = (i * 45.0f + 22.5f) * DEG_TO_RAD;
    int bx = (int)roundf(CENTRE + BOLT_RADIUS * cosf(a));
    int by = (int)roundf(CENTRE + BOLT_RADIUS * sinf(a));
    bg.fillCircle(bx + 1, by + 1, 4, c565(70, 74, 84));       // drop shadow
    bg.fillCircle(bx, by, 4, c565(120, 126, 138));
    bg.fillCircle(bx - 1, by - 1, 3, c565(190, 196, 208));
    bg.fillCircle(bx - 1, by - 1, 1, c565(250, 250, 255));
  }
}

// Same shadow the ring casts on the eyeball in renderBackground(), so lids and eyeball match
static float ringShadeFactor(float dx, float dy) {
  float d = sqrtf(dx * dx + dy * dy);
  float inv = d > 0.001f ? 1.0f / d : 0.0f;
  float cosA = (dx * LX + dy * LY) * inv;
  float sh = smooth01((R_GLASS - d) / 16.0f);
  return 1.0f - (1.0f - sh) * (0.14f + 0.30f * fmaxf(0.0f, cosA));
}

bool buildLidTables() {
  uint32_t total = 0;
  for (int x = 0; x < 240; x++) {
    float dx = x + 0.5f - 120.0f;
    float h2 = (float)R_GLASS * R_GLASS - dx * dx;
    if (h2 <= 0) { colHalf[x] = -1; shadeCnt[x] = 0; shadeOff[x] = total; continue; }
    int h = (int)sqrtf(h2);
    colHalf[x] = h;
    int cnt = 0;
    while (cnt < 2 * h) {   // rows from the top edge that are within the ring's shadow
      float dy = (CY - h + cnt) + 0.5f - 120.0f;
      if (R_GLASS - sqrtf(dx * dx + dy * dy) >= 16.0f) break;
      cnt++;
    }
    shadeCnt[x] = cnt;
    shadeOff[x] = total;
    total += cnt;
  }

  shadeTop = (uint8_t*)malloc(total);
  shadeBot = (uint8_t*)malloc(total);
  if (!shadeTop || !shadeBot) return false;

  for (int x = 0; x < 240; x++) {
    int h = colHalf[x];
    if (h < 0) continue;
    float dx = x + 0.5f - 120.0f;
    for (int i = 0; i < shadeCnt[x]; i++) {
      float dyT = (CY - h + i) + 0.5f - 120.0f;
      float dyB = (CY - 1 + h - i) + 0.5f - 120.0f;
      shadeTop[shadeOff[x] + i] = (uint8_t)(ringShadeFactor(dx, dyT) * 255.0f);
      shadeBot[shadeOff[x] + i] = (uint8_t)(ringShadeFactor(dx, dyB) * 255.0f);
    }
  }
  return true;
}

// Iris texture, rendered once per pixel (smooth gradient, ordered dither against banding)
constexpr int IRIS_TEX = 2 * R_IRIS + 2;
constexpr uint16_t IRIS_KEY = 0xF81F;   // transparent colour
constexpr float IRIS_FIBRE_STRENGTH = 0.07f;   // 0 = plain gradient, 0.15 = clearly visible fibres

void buildIrisTexture() {
  static const int8_t bayer[4][4] = {{-2, 0, -1, 1}, {1, -1, 2, 0}, {-1, 1, -2, 0}, {2, 0, 1, -1}};
  const V3 inner = {168, 82, 58}, mid = {112, 46, 36}, outer = {62, 24, 20}, rim = {36, 14, 12};
  const float c = IRIS_TEX / 2.0f;
  irisTex.fillScreen(IRIS_KEY);
  for (int y = 0; y < IRIS_TEX; y++) {
    for (int x = 0; x < IRIS_TEX; x++) {
      float dx = x + 0.5f - c, dy = y + 0.5f - c;
      float d = sqrtf(dx * dx + dy * dy);
      if (d > R_IRIS) continue;
      float u = clampf((d - 8.0f) / (R_IRIS - 8.0f), 0, 1);
      V3 col = u < 0.55f ? mix(inner, mid, smooth01(u / 0.55f))
                         : mix(mid, outer, smooth01((u - 0.55f) / 0.45f));
      col = mix(col, rim, smooth01((d - (R_IRIS - 3.5f)) / 3.5f));   // soft dark rim
      float lit = 1.0f + 0.10f * ((dx * LX + dy * LY) / R_IRIS);       // slightly lighter top-left

      // Faint radial fibres: low-contrast brightness variation by angle, faded out at pupil and rim
      float a = atan2f(dy, dx);
      float fib = 0.5f * sinf(23 * a + 1.3f) + 0.3f * sinf(41 * a + 0.4f) + 0.2f * sinf(67 * a + 2.1f);
      float fade = smooth01((d - 9.0f) / 6.0f) * (1.0f - smooth01((d - (R_IRIS - 6.0f)) / 5.0f));
      lit *= 1.0f + IRIS_FIBRE_STRENGTH * fib * fade;

      int dither = bayer[y & 3][x & 3];
      uint16_t out = c565((int)(col.r * lit) + dither, (int)(col.g * lit) + dither, (int)(col.b * lit) + dither);
      if (out == IRIS_KEY) out ^= 0x0020;
      irisTex.drawPixel(x, y, out);
    }
  }
}

// ---------------- Emotions ----------------
// The face is described by a few numbers that ease towards the current emotion's values.
struct Face {
  float top;       // fraction of the eye covered by the upper lid (0 = wide open)
  float bulge;     // how far the upper lid droops in the middle (px)
  float tilt;      // slant of the upper lid (px); unused by the current moods
  float lowRise;   // fraction of the eye covered by the lower lid ("cheek", happy)
  float lowBulge;  // how much the lower lid arches up in the middle (px)
};

enum Emotion { NEUTRAL, HAPPY, CUTE, SLEEPY, SURPRISED, EMOTION_COUNT };

struct EmotionDef {
  const char* name;
  Face face;
  int pupilMin, pupilMax;   // pupil radius range
  float gazeBias;           // -1 = look up .. +1 = look down
  uint16_t blinkMs;
  uint8_t weight;           // how often it is picked in the random cycle
};

const EmotionDef EMOTIONS[EMOTION_COUNT] = {
  //            top    bulge tilt lowRise lowBulge
  {"neutral",    {0.15f,  7,    0,  0.00f,  4},  15, 19,  0.0f, 240, 28},
  {"happy",      {0.08f,  3,    0,  0.10f, 12},  18, 23,  0.0f, 200, 28},
  {"cute",       {0.02f,  1,    0,  0.00f,  3},  22, 27, -0.1f, 130, 28},   // wide eyes, big pupil, quick blinks
  {"sleepy",     {0.42f,  4,    0,  0.02f,  4},  14, 17,  0.5f, 420, 10},
  {"surprised",  {0.00f,  0,    0,  0.00f,  0},  12, 14, -0.1f, 180,  6},
};

Face face = EMOTIONS[NEUTRAL].face;
Face faceTgt = EMOTIONS[NEUTRAL].face;
Emotion emotion = NEUTRAL;
bool autoMode = true;      // true = random cycle, false = mood chosen from the web page
float tiltSign = 1;
uint32_t nextEmotion = 0;

// ---------------- Animation state ----------------
float eyeX = 0, eyeY = 0;           // current iris offset
float tgtX = 0, tgtY = 0;           // target iris offset
float pupilR = 14, tgtPupilR = 14;  // dilation
float blink = 0;                    // 0 open .. 1 closed

uint32_t nextLook = 0, nextBlink = 0, blinkStart = 0;
bool blinking = false;

static float ease(float cur, float tgt, float k, float dt) {
  return cur + (tgt - cur) * (1.0f - expf(-k * dt));
}

void applyEmotion(Emotion pick, uint32_t now) {
  emotion = pick;
  faceTgt = EMOTIONS[pick].face;
  tiltSign = random(2) ? 1.0f : -1.0f;
  nextEmotion = now + random(4000, 9000);
  nextLook = now;                       // re-aim immediately
  if (pick == CUTE) {
    nextBlink = now + 1000;             // eyes open and looking up first, then a single blink
  } else if (!blinking) {
    nextBlink = now + 100;              // blink as the expression changes
  }
  Serial.printf("emotion: %s%s\n", EMOTIONS[pick].name, autoMode ? " (auto)" : " (manual)");
}

void pickEmotion(uint32_t now) {
  int total = 0;
  for (int i = 0; i < EMOTION_COUNT; i++) total += (i == emotion) ? 0 : EMOTIONS[i].weight;
  int r = random(total);
  Emotion pick = NEUTRAL;
  for (int i = 0; i < EMOTION_COUNT; i++) {
    if (i == emotion) continue;
    if (r < EMOTIONS[i].weight) { pick = (Emotion)i; break; }
    r -= EMOTIONS[i].weight;
  }
  applyEmotion(pick, now);
}

void pickLook(uint32_t now) {
  const EmotionDef& e = EMOTIONS[emotion];
  if (emotion == CUTE) {   // wide-eyed, big pupil, gazing up
    tgtX = random(-8, 9);
    tgtY = -MAX_OFFSET * 0.85f;
    tgtPupilR = random(e.pupilMax - 1, e.pupilMax + 1);
    nextLook = now + random(1500, 3000);
    return;
  }
  if (random(100) < 25) {  // look straight ahead
    tgtX = random(-8, 9);
    tgtY = random(-8, 9);
  } else {
    float a = random(0, 628) / 100.0f;
    float d = MAX_OFFSET * (0.4f + random(0, 61) / 100.0f);
    tgtX = cosf(a) * d;
    tgtY = sinf(a) * d * 0.8f;
  }
  tgtY = clampf(tgtY * 0.6f + e.gazeBias * MAX_OFFSET * 0.7f, -MAX_OFFSET, MAX_OFFSET);
  tgtPupilR = random(e.pupilMin, e.pupilMax + 1);
  nextLook = now + random(500, 2600);
}

void updateAnim(uint32_t now, float dt) {
  if (autoMode && now >= nextEmotion) pickEmotion(now);
  if (now >= nextLook) pickLook(now);

  eyeX = ease(eyeX, tgtX, 14.0f, dt);
  eyeY = ease(eyeY, tgtY, 14.0f, dt);
  pupilR = ease(pupilR, tgtPupilR, 6.0f, dt);

  face.top      = ease(face.top,      faceTgt.top,      7.0f, dt);
  face.bulge    = ease(face.bulge,    faceTgt.bulge,    7.0f, dt);
  face.tilt     = ease(face.tilt,     faceTgt.tilt,     7.0f, dt);
  face.lowRise  = ease(face.lowRise,  faceTgt.lowRise,  7.0f, dt);
  face.lowBulge = ease(face.lowBulge, faceTgt.lowBulge, 7.0f, dt);

  if (!blinking && now >= nextBlink) {
    blinking = true;
    blinkStart = now;
  }
  if (blinking) {
    float t = (now - blinkStart) / (float)EMOTIONS[emotion].blinkMs;
    if (t >= 1.0f) {
      blinking = false;
      blink = 0;
      // always a single blink; cute keeps its eyes open and gazing up for a bit longer than ~1 s
      nextBlink = now + (emotion == CUTE ? random(1000, 1600) : random(1800, 5500));
    } else {
      blink = t < 0.4f ? t / 0.4f : 1.0f - (t - 0.4f) / 0.6f;   // fast close, slower open
      blink = blink * blink * (3 - 2 * blink);
    }
  }
}

// ---------------- Drawing ----------------
struct LidParams {
  int upperY;
  float bulge, tilt;
  int lowerY;
  float lowBulge;
};

LidParams computeLid() {
  LidParams L;
  float top = clampf(face.top + blink * (1.0f - face.top), 0, 1);
  L.upperY = CY - R_GLASS + (int)(top * 2 * R_GLASS);
  L.bulge = quarter(face.bulge * (1.0f - blink * 0.7f));
  L.tilt = quarter(face.tilt * tiltSign * (1.0f - blink));
  L.lowerY = CY + R_GLASS - (int)(face.lowRise * 2 * R_GLASS);   // only the upper lid blinks
  L.lowBulge = quarter(face.lowBulge);
  return L;
}

void drawIris(int ix, int iy, int pr) {
  irisTex.pushSprite(&strip, ix - IRIS_TEX / 2, iy - IRIS_TEX / 2 - gOY, IRIS_KEY);

  // Pupil and highlights
  sCircle(ix, iy, pr + 1, c565(40, 16, 14));
  sCircle(ix, iy, pr, c565(14, 6, 8));
  sCircle(ix - 15, iy - 15, 9, c565(205, 150, 130));   // soft glare
  sCircle(ix - 15, iy - 15, 6, c565(250, 225, 215));
  sCircle(ix - 15, iy - 15, 4, c565(255, 255, 255));   // glossy glint
  sCircle(ix + 12, iy + 14, 2, c565(235, 175, 150));   // small secondary glint
}

// Draws the eyelids for screen columns xa..xb, only in the rows of the current strip
void drawEyelids(const LidParams& L, int xa, int xb) {
  static const uint8_t shadowAlpha[6] = {120, 95, 70, 48, 28, 12};
  static const uint8_t lowShadowAlpha[4] = {70, 45, 25, 10};
  const int sy0 = gOY, sy1 = gOY + STRIP_H - 1;

  for (int x = xa; x <= xb; x++) {
    int h = colHalf[x];
    if (h < 0) continue;
    int ytop = CY - h, ybot = CY - 1 + h;
    float dxn = (x + 0.5f - 120.0f) / R_GLASS;
    float curve = 1.0f - dxn * dxn;

    // Lower lid ("cheek"), drawn first so a closing upper lid covers it
    int ledge = L.lowerY - (int)(L.lowBulge * curve);
    if (ledge < ytop) ledge = ytop;
    if (ledge <= ybot && ledge - 10 <= sy1 && ybot >= sy0) {
      int n = ybot - ledge + 1;
      int k = n < shadeCnt[x] ? n : shadeCnt[x];
      const uint8_t* fb = shadeBot + shadeOff[x];
      int i0 = max(0, ybot - sy1), i1 = min(k - 1, ybot - sy0);   // only the rows inside this strip
      for (int i = i0; i <= i1; i++) sPixel(x, ybot - i, lidShadeCol(fb[i]));
      if (n > k) sVLine(x, ledge, n - k, C_LID);
      sPixel(x, ledge, C_LID_EDGE);
      for (int b = 0; b < 4; b++) {   // soft shadow above the lower lid
        int y = ledge - 2 - b * 2;
        if (y < ytop) break;
        sRectAlpha(x, y, 1, 2, (uint8_t)(lowShadowAlpha[b] * LID_SHADOW_STRENGTH), C_SHADOW);
      }
    }

    // Upper lid
    int edge = L.upperY + (int)(L.bulge * curve + L.tilt * dxn);
    if (edge > ybot) edge = ybot;
    if (edge >= ytop && ytop <= sy1 && edge + 13 >= sy0) {
      int n = edge - ytop + 1;
      int k = n < shadeCnt[x] ? n : shadeCnt[x];
      const uint8_t* ft = shadeTop + shadeOff[x];
      int i0 = max(0, sy0 - ytop), i1 = min(k - 1, sy1 - ytop);   // only the rows inside this strip
      for (int i = i0; i <= i1; i++) sPixel(x, ytop + i, lidShadeCol(ft[i]));
      if (n > k) sVLine(x, ytop + k, n - k, C_LID);
      sPixel(x, edge, C_LID_EDGE);
      if (edge > ytop) sPixel(x, edge - 1, C_LID_EDGE);
      for (int b = 0; b < 6; b++) {   // soft shadow cast onto the eyeball
        int y = edge + 1 + b * 2;
        if (y > ybot) break;
        int hh = ybot - y + 1 < 2 ? ybot - y + 1 : 2;
        sRectAlpha(x, y, 1, hh, (uint8_t)(shadowAlpha[b] * LID_SHADOW_STRENGTH), C_SHADOW);
      }
    }
  }
}

// ---------------- Dirty-region rendering ----------------
struct Rect { int x0, y0, x1, y1; };   // inclusive, screen coordinates

static inline Rect clipRect(Rect r) {
  return {clampi(r.x0, 0, 239), clampi(r.y0, 0, 239), clampi(r.x1, 0, 239), clampi(r.y1, 0, 239)};
}

// Compose one rectangle band by band and send it to the display
uint32_t renderRect(Rect r, int ix, int iy, int pr, const LidParams& L) {
  r = clipRect(r);
  if (r.x1 < r.x0 || r.y1 < r.y0) return 0;
  const int w = r.x1 - r.x0 + 1;

  for (int ys = r.y0; ys <= r.y1; ys += STRIP_H) {
    const int h = (r.y1 - ys + 1 < STRIP_H) ? (r.y1 - ys + 1) : STRIP_H;
    gOY = ys;

    strip.setClipRect(r.x0, 0, w, h);
    bg.pushSprite(&strip, 0, -ys);                 // restore the static background
    drawIris(ix, iy, pr);
    drawEyelids(L, r.x0, r.x1);
    strip.clearClipRect();

    lcd.setClipRect(r.x0, ys, w, h);               // send only this part to the panel
    strip.pushSprite(0, ys);
    lcd.clearClipRect();
  }
  return (uint32_t)w * (r.y1 - r.y0 + 1);
}

// Renders whatever changed since the last frame. Returns the number of pixels sent.
uint32_t renderFrame() {
  static bool first = true;
  static int pIx = 0, pIy = 0, pPr = 0;
  static LidParams pL;

  const int ix = CX + (int)eyeX, iy = CY + (int)eyeY, pr = (int)pupilR;
  const LidParams L = computeLid();
  const int half = IRIS_TEX / 2 + 1;
  uint32_t px = 0;

  if (first || !PARTIAL_UPDATES) {
    first = false;
    px = renderRect({0, 0, 239, 239}, ix, iy, pr, L);
  } else {
    Rect rects[3];
    int n = 0;

    if (ix != pIx || iy != pIy || pr != pPr) {   // iris moved or pupil resized
      rects[n++] = {min(ix, pIx) - half, min(iy, pIy) - half, max(ix, pIx) + half, max(iy, pIy) + half};
    }
    if (L.upperY != pL.upperY || L.bulge != pL.bulge || L.tilt != pL.tilt) {   // upper lid moved
      float eMaxNew = L.upperY + L.bulge + fabsf(L.tilt);
      float eMaxOld = pL.upperY + pL.bulge + fabsf(pL.tilt);
      rects[n++] = {CX - R_GLASS, CY - R_GLASS, CX + R_GLASS, (int)fmaxf(eMaxNew, eMaxOld) + 14};
    }
    if (L.lowerY != pL.lowerY || L.lowBulge != pL.lowBulge) {                  // lower lid moved
      float eMinNew = L.lowerY - L.lowBulge;
      float eMinOld = pL.lowerY - pL.lowBulge;
      rects[n++] = {CX - R_GLASS, (int)fminf(eMinNew, eMinOld) - 12, CX + R_GLASS, CY + R_GLASS};
    }
    for (int i = 0; i < n; i++) px += renderRect(rects[i], ix, iy, pr, L);
  }

  pIx = ix; pIy = iy; pPr = pr; pL = L;
  return px;
}

// ---------------- Web page ----------------
WebServer server(80);
bool serverUp = false, apMode = false;
uint32_t wifiStart = 0;

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Minion Eye</title>
<style>
  :root{--bg:#fff7d6;--fg:#2b2410;--card:#ffe27a;--accent:#2f5d9e;--on:#fff}
  @media (prefers-color-scheme:dark){:root{--bg:#1d1a10;--fg:#f4ecc8;--card:#3a3318;--accent:#6aa3ee;--on:#10151f}}
  *{box-sizing:border-box}
  body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,sans-serif}
  main{max-width:420px;margin:0 auto}
  h1{font-size:1.5rem;margin:0 0 4px}
  p{margin:0 0 20px;opacity:.8}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
  button{padding:16px 8px;border:2px solid transparent;border-radius:14px;background:var(--card);
    color:var(--fg);font:inherit;font-weight:600;text-transform:capitalize;cursor:pointer}
  button.on{background:var(--accent);color:var(--on)}
  button.auto{grid-column:1/-1}
  #st{margin-top:18px;font-size:.9rem;opacity:.8}
</style></head><body><main>
<h1>Minion Eye</h1>
<p>Pick a mood, or choose Auto to let the eye cycle through them by itself.</p>
<div class="grid" id="g"></div>
<div id="st"></div>
</main>
<script>
const g=document.getElementById('g'),st=document.getElementById('st');
let built=false;
function build(s){
  g.innerHTML='';
  const a=document.createElement('button');a.className='auto';a.textContent='Auto (random cycle)';a.dataset.n='auto';g.appendChild(a);
  s.moods.forEach(m=>{const b=document.createElement('button');b.textContent=m;b.dataset.n=m;g.appendChild(b)});
  g.onclick=e=>{if(e.target.dataset.n)set(e.target.dataset.n)};
  built=true;
}
function show(s){
  if(!built)build(s);
  [...g.children].forEach(b=>b.classList.toggle('on',s.auto?b.dataset.n==='auto':b.dataset.n===s.mood));
  st.textContent=(s.auto?'Auto - now showing: ':'Manual - ')+s.mood;
}
function load(u){fetch(u).then(r=>r.json()).then(show).catch(()=>{st.textContent='Connection lost'})}
function set(n){load('/api/mood?name='+encodeURIComponent(n))}
load('/api/state');setInterval(()=>load('/api/state'),2000);
</script></body></html>)HTML";

String stateJson() {
  String s = "{\"auto\":";
  s += autoMode ? "true" : "false";
  s += ",\"mood\":\"";
  s += EMOTIONS[emotion].name;
  s += "\",\"moods\":[";
  for (int i = 0; i < EMOTION_COUNT; i++) {
    if (i) s += ',';
    s += '"'; s += EMOTIONS[i].name; s += '"';
  }
  s += "]}";
  return s;
}

void handleMood() {
  String name = server.arg("name");
  if (name == "auto") {
    autoMode = true;
    nextEmotion = millis() + 2000;
    Serial.println("mode: auto");
  } else {
    bool found = false;
    for (int i = 0; i < EMOTION_COUNT; i++) {
      if (name == EMOTIONS[i].name) {
        autoMode = false;
        applyEmotion((Emotion)i, millis());
        found = true;
        break;
      }
    }
    if (!found) { server.send(400, "application/json", "{\"error\":\"unknown mood\"}"); return; }
  }
  server.send(200, "application/json", stateJson());
}

void startServer() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET, []() { server.send(200, "application/json", stateJson()); });
  server.on("/api/mood", HTTP_GET, handleMood);
  server.begin();
  serverUp = true;
}

void startAP() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  startServer();
  Serial.printf("WiFi AP \"%s\" (password \"%s\") - open http://%s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
}

void startWifi() {
  if (strlen(WIFI_SSID) == 0) { startAP(); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // Super Mini boards often connect more reliably at reduced power
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiStart = millis();
  Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
}

// Called every loop: finishes the WiFi connection without blocking the animation
void serviceWifi() {
  if (!serverUp) {
    if (strlen(WIFI_SSID) != 0) {
      if (WiFi.status() == WL_CONNECTED) {
        startServer();
        if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
        Serial.printf("WiFi connected - open http://%s  or  http://%s.local\n",
                      WiFi.localIP().toString().c_str(), MDNS_NAME);
        Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
      } else if (millis() - wifiStart > 15000) {
        Serial.println("WiFi connection failed, starting access point instead");
        startAP();
      }
    }
  } else {
    server.handleClient();
  }
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  delay(1500);  // give the USB serial monitor time to attach
  randomSeed(esp_random());

  pinMode(PIN_RST_HIGH, OUTPUT);
  digitalWrite(PIN_RST_HIGH, HIGH);
  delay(50);

  Serial.println("Init LCD...");
  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(200);

  // Wiring self-test: red, green, blue, white full-screen flashes
  const uint32_t testColours[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  for (uint32_t c : testColours) {
    lcd.fillScreen(c);
    delay(400);
  }
  lcd.fillScreen(TFT_BLACK);
  Serial.printf("LCD init done. Free heap: %u\n", ESP.getFreeHeap());

  bg.setColorDepth(16);      bg.setPsram(false);
  strip.setColorDepth(16);   strip.setPsram(false);
  irisTex.setColorDepth(16); irisTex.setPsram(false);
  if (!bg.createSprite(240, 240) || !strip.createSprite(240, STRIP_H) ||
      !irisTex.createSprite(IRIS_TEX, IRIS_TEX)) {
    Serial.printf("Sprite alloc failed. Free heap: %u\n", ESP.getFreeHeap());
    lcd.fillScreen(TFT_RED);
    while (true) delay(1000);
  }
  Serial.printf("Sprites OK. Free heap: %u\n", ESP.getFreeHeap());

  C_LID      = c565(LID_R, LID_G, LID_B);
  C_LID_EDGE = c565(176, 122, 22);
  C_SHADOW   = c565(60, 38, 12);

  if (!buildLidTables()) {
    Serial.println("Lid table alloc failed");
    lcd.fillScreen(TFT_RED);
    while (true) delay(1000);
  }
  renderBackground();
  buildIrisTexture();

  uint32_t now = millis();
  nextLook = nextBlink = now + 1000;
  nextEmotion = now + 3000;

  startWifi();
  Serial.println("Minion eye ready");
}

void loop() {
  static uint32_t last = millis();
  const uint32_t now = millis();
  const float dt = (now - last) / 1000.0f;
  last = now;

  serviceWifi();
  updateAnim(now, dt);
  uint32_t px = renderFrame();

  // Print the average frame rate and amount of screen redrawn every 2 seconds
  static uint32_t fpsStart = now, frames = 0, drawn = 0, pixels = 0;
  frames++;
  if (px) { drawn++; pixels += px; }
  if (now - fpsStart >= 2000) {
    Serial.printf("%.1f fps, %u/%u frames changed, avg %u px sent\n",
                  frames * 1000.0f / (now - fpsStart), drawn, frames, drawn ? pixels / drawn : 0);
    fpsStart = now; frames = drawn = pixels = 0;
  }

  // Cap at ~60 fps and give the WiFi stack time to run
  uint32_t spent = millis() - now;
  delay(spent < 16 ? 16 - spent : 1);
}
