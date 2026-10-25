// Animated Minion eye
// ESP32-C3 Super Mini + Waveshare 1.28" round LCD (GC9A01, 240x240)

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <math.h>

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
constexpr uint32_t SPI_WRITE_HZ = 80000000;

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
LGFX_Sprite canvas(&lcd);  // frame buffer, pushed to the LCD every frame
LGFX_Sprite bg(&lcd);      // static layer: skin, goggle ring, bolts, shaded eyeball

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

struct V3 { float r, g, b; };
static inline V3 mix(V3 a, V3 b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
static inline uint16_t toC(V3 c) { return c565((int)c.r, (int)c.g, (int)c.b); }

// Per-column data for the eyelids (filled once in setup)
int16_t  colHalf[240];                       // half-height of the glass circle at each column (-1 = outside)
uint16_t lidCol[240][LID_SHADE_ROWS];        // lid skin colour for the first rows next to the ring
constexpr int LID_R = 247, LID_G = 209, LID_B = 38;
uint16_t C_LID, C_LID_EDGE, C_SHADOW;

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

void buildLidTables() {
  for (int x = 0; x < 240; x++) {
    float dx = x + 0.5f - 120.0f;
    float h2 = (float)R_GLASS * R_GLASS - dx * dx;
    if (h2 <= 0) { colHalf[x] = -1; continue; }
    int h = (int)sqrtf(h2);
    colHalf[x] = h;
    for (int i = 0; i < LID_SHADE_ROWS; i++) {
      float dy = (120 - h + i) + 0.5f - 120.0f;
      float depth = R_GLASS - sqrtf(dx * dx + dy * dy);   // distance from the ring
      float f = 0.60f + 0.40f * smooth01(depth / 14.0f);   // ring shadow on the lid
      lidCol[x][i] = c565((int)(LID_R * f), (int)(LID_G * f * 0.97f), (int)(LID_B * f * 0.85f));
    }
  }
}

// ---------------- Emotions ----------------
// The face is described by a few numbers that ease towards the current emotion's values.
struct Face {
  float top;       // fraction of the eye covered by the upper lid (0 = wide open)
  float bulge;     // how far the upper lid droops in the middle (px)
  float tilt;      // slant of the upper lid, + = lower on the right (px, angry/sad)
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
  uint8_t weight;           // how often it is picked
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
float tiltSign = 1;
uint32_t nextEmotion = 0;

// ---------------- Animation state ----------------
float eyeX = 0, eyeY = 0;           // current iris offset
float tgtX = 0, tgtY = 0;           // target iris offset
float pupilR = 14, tgtPupilR = 14;  // dilation
float blink = 0;                    // 0 open .. 1 closed

uint32_t nextLook = 0, nextBlink = 0, blinkStart = 0;
bool blinking = false;
int blinkChain = 0;        // blinks in a row so far (normal moods: max 2)
int cuteBurstLeft = 0;     // quick blinks still to come in the cute-mood burst

static float ease(float cur, float tgt, float k, float dt) {
  return cur + (tgt - cur) * (1.0f - expf(-k * dt));
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
  emotion = pick;
  faceTgt = EMOTIONS[pick].face;
  tiltSign = random(2) ? 1.0f : -1.0f;
  nextEmotion = now + random(4000, 9000);
  nextLook = now;                       // re-aim immediately
  blinkChain = 0;
  if (pick == CUTE) {
    cuteBurstLeft = random(2, 4);
    nextBlink = now + 1000;             // eyes open and looking up first, then the blink burst
  } else if (!blinking) {
    nextBlink = now + 100;              // blink as the expression changes
  }
  Serial.printf("emotion: %s\n", EMOTIONS[pick].name);
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
  if (now >= nextEmotion) pickEmotion(now);
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
      if (emotion == CUTE) {
        // hold the eyes open for about a second, then a quick burst of blinks
        if (cuteBurstLeft > 0) {
          cuteBurstLeft--;
          nextBlink = now + 70;
        } else {
          cuteBurstLeft = random(2, 4);            // 2-3 more after the first = 3-4 total
          nextBlink = now + random(1000, 1500);
        }
      } else {
        blinkChain++;
        if (blinkChain == 1 && random(0, 100) < 20) {
          nextBlink = now + 120;                   // occasional double blink, never more than 2
        } else {
          blinkChain = 0;
          nextBlink = now + random(1800, 5500);
        }
      }
    } else {
      blink = t < 0.4f ? t / 0.4f : 1.0f - (t - 0.4f) / 0.6f;   // fast close, slower open
      blink = blink * blink * (3 - 2 * blink);
    }
  }
}

// ---------------- Drawing ----------------
// Iris texture, rendered once per pixel (smooth gradient, ordered dither against banding)
constexpr int IRIS_TEX = 2 * R_IRIS + 2;
constexpr uint16_t IRIS_KEY = 0xF81F;   // transparent colour
constexpr float IRIS_FIBRE_STRENGTH = 0.07f;   // 0 = plain gradient, 0.15 = clearly visible fibres
LGFX_Sprite irisTex(&lcd);

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

void drawIris(int ix, int iy, int pr) {
  irisTex.pushSprite(&canvas, ix - IRIS_TEX / 2, iy - IRIS_TEX / 2, IRIS_KEY);

  // Pupil and highlights
  canvas.fillCircle(ix, iy, pr + 1, c565(40, 16, 14));
  canvas.fillCircle(ix, iy, pr, c565(14, 6, 8));
  canvas.fillCircle(ix - 15, iy - 15, 9, c565(205, 150, 130));   // soft glare
  canvas.fillCircle(ix - 15, iy - 15, 6, c565(250, 225, 215));
  canvas.fillCircle(ix - 15, iy - 15, 4, c565(255, 255, 255));   // glossy glint
  canvas.fillCircle(ix + 12, iy + 14, 2, c565(235, 175, 150));   // small secondary glint
}

void drawEyelids() {
  const float top = clampf(face.top + blink * (1.0f - face.top), 0, 1);
  const int upperY = CY - R_GLASS + (int)(top * 2 * R_GLASS);
  const float bulge = face.bulge * (1.0f - blink * 0.7f);
  const float tilt = face.tilt * tiltSign * (1.0f - blink);
  const int lowerY = CY + R_GLASS - (int)(face.lowRise * 2 * R_GLASS);   // only the upper lid blinks
  const float lowBulge = face.lowBulge;
  static const uint8_t shadowAlpha[6] = {120, 95, 70, 48, 28, 12};
  static const uint8_t lowShadowAlpha[4] = {70, 45, 25, 10};

  for (int x = 0; x < 240; x++) {
    int h = colHalf[x];
    if (h < 0) continue;
    int ytop = CY - h, ybot = CY - 1 + h;
    float dxn = (x + 0.5f - 120.0f) / R_GLASS;
    float curve = 1.0f - dxn * dxn;

    // Lower lid ("cheek"), drawn first so a closing upper lid covers it
    int ledge = lowerY - (int)(lowBulge * curve);
    if (ledge < ytop) ledge = ytop;
    if (ledge <= ybot) {
      int n = ybot - ledge + 1;
      int k = n < LID_SHADE_ROWS ? n : LID_SHADE_ROWS;
      for (int i = 0; i < k; i++) canvas.drawPixel(x, ybot - i, lidCol[x][i]);
      if (n > LID_SHADE_ROWS) canvas.drawFastVLine(x, ledge, n - LID_SHADE_ROWS, C_LID);
      canvas.drawPixel(x, ledge, C_LID_EDGE);
      for (int b = 0; b < 4; b++) {   // soft shadow above the lower lid
        int y = ledge - 2 - b * 2;
        if (y < ytop) break;
        canvas.fillRectAlpha(x, y, 1, 2, lowShadowAlpha[b], C_SHADOW);
      }
    }

    // Upper lid
    int edge = upperY + (int)(bulge * curve + tilt * dxn);
    if (edge > ybot) edge = ybot;
    if (edge >= ytop) {
      int n = edge - ytop + 1;
      int k = n < LID_SHADE_ROWS ? n : LID_SHADE_ROWS;
      for (int i = 0; i < k; i++) canvas.drawPixel(x, ytop + i, lidCol[x][i]);
      if (n > LID_SHADE_ROWS) canvas.drawFastVLine(x, ytop + LID_SHADE_ROWS, n - LID_SHADE_ROWS, C_LID);
      canvas.drawPixel(x, edge, C_LID_EDGE);
      if (edge > ytop) canvas.drawPixel(x, edge - 1, C_LID_EDGE);
      for (int b = 0; b < 6; b++) {   // soft shadow cast onto the eyeball
        int y = edge + 1 + b * 2;
        if (y > ybot) break;
        int hh = ybot - y + 1 < 2 ? ybot - y + 1 : 2;
        canvas.fillRectAlpha(x, y, 1, hh, shadowAlpha[b], C_SHADOW);
      }
    }
  }
}

void drawEye() {
  bg.pushSprite(&canvas, 0, 0);                                // static layer
  drawIris(CX + (int)eyeX, CY + (int)eyeY, (int)pupilR);       // moving parts
  drawEyelids();
}

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

  canvas.setColorDepth(16);
  canvas.setPsram(false);
  bg.setColorDepth(16);
  bg.setPsram(false);
  irisTex.setColorDepth(16);
  irisTex.setPsram(false);
  if (!bg.createSprite(240, 240) || !canvas.createSprite(240, 240) ||
      !irisTex.createSprite(IRIS_TEX, IRIS_TEX)) {
    Serial.printf("Sprite alloc failed (need ~225KB heap). Free heap: %u\n", ESP.getFreeHeap());
    lcd.fillScreen(TFT_RED);
    while (true) delay(1000);
  }
  Serial.printf("Sprites OK. Free heap: %u\n", ESP.getFreeHeap());

  C_LID      = c565(LID_R, LID_G, LID_B);
  C_LID_EDGE = c565(176, 122, 22);
  C_SHADOW   = c565(60, 38, 12);

  buildLidTables();
  renderBackground();
  buildIrisTexture();

  uint32_t now = millis();
  nextLook = nextBlink = now + 1000;
  nextEmotion = now + 3000;
  Serial.println("Minion eye ready");
}

void loop() {
  static uint32_t last = millis();
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f;
  last = now;

  updateAnim(now, dt);
  drawEye();
  canvas.pushSprite(0, 0);

  // Print the average frame rate every 2 seconds
  static uint32_t fpsStart = now, frames = 0;
  frames++;
  if (now - fpsStart >= 2000) {
    Serial.printf("%.1f fps\n", frames * 1000.0f / (now - fpsStart));
    fpsStart = now;
    frames = 0;
  }
}
