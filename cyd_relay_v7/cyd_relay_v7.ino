
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <FS.h>
#include <time.h>
#include <LovyanGFX.hpp>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#include "EarthIcon2D.h"

// ======== CAU HINH ========
const char* WIFI_SSID = "----------";            // cung WiFi voi relay
const char* WIFI_PASS = "----------";
const char* RELAY_HOST = "esp-relay";         // http://esp-relay.local/
const char* RELAY_FALLBACK_IP = "";           // vd "192.168.1.50" neu mDNS khong tim ra (de "" de tat)
const char* CYD_HOSTNAME = "cyd-relay";
// ==========================

// ---------------- GOC XOAY MAN HINH ----------------
#define ROTATION 3
#define SCR_W 320
#define SCR_H 240

// ---------------- Pin man hinh (HSPI) ----------------
#define TFT_MISO_PIN 12
#define TFT_MOSI_PIN 13
#define TFT_SCLK_PIN 14
#define TFT_CS_PIN   15
#define TFT_DC_PIN   2
#define TFT_BL_PIN   21          // den nen man hinh (IO21)

// ---------------- Cam ung XPT2046: doc bang GPIO ----------------
#define TP_CLK  25
#define TP_CS   33
#define TP_DIN  32
#define TP_OUT  39
#define TP_IRQ  36

// ---------------- The SD (VSPI): CD/DAT3=IO5, CMD=IO23, CLK=IO18, DAT0=IO19 ----------------
#define SD_CS_PIN   5
#define SD_SCK_PIN  18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23

// ---------------- Dien tro quang + nut BOOT ----------------
#define BOOT_BTN_PIN  0
#define ANIM_STEP     2      // do min luc nut dang chuyen dong: 1 = day du, 2 = khoi 2x2 (nhanh ~4 lan)
#define HIT_MARGIN    20     // mo rong vung cham quanh nut (px)
#define PRESS_GAP_MS  400    // khoang cach toi thieu giua 2 lan bam nut
#define HOLD_MS       600    // giu man hinh bay nhieu ms de thoat man cho
#define MAIN_TAP_ANYWHERE 1  // 1 = o man den, cham 1 lan BAT KY cho nao cung bat/tat den; 0 = chi khi cham trung nut
#define SWIPE_MIN     40     // vuot toi thieu (px) de tinh la 1 lan vuot
#define SLIDE_MS      260    // thoi gian keo man hinh cho len (ms)
#define TP_POLL_MS    5      // chu ky doc cam ung cua task rieng (ms)
#define TP_ZOFF       120    // nguong ap luc (Z) de giu cham khi chan IRQ chap chon
#define TP_BRIDGE_MS  120    // toi da giu cham them bay nhieu ms khi IRQ chap chon
#define SD_FREQ       20000000   // thu 40000000 neu the SD tot -> doc anh nen nhanh ~2 lan (neu anh nen chop/loi thi ha lai)
#define WALL_CACHE    1      // 1 = dem anh nen trong RAM (do cao 1/2) de keo man hinh muot; tu tat neu thieu RAM
#define WC_ROWS       (SCR_H / 2)
#define WC_BYTES      (SCR_W * WC_ROWS * 2)

#define PANEL_INVERT     false
#define PANEL_RGB_ORDER  false

// ---------------- Thoi gian hieu ung (giong CSS) ----------------
#define ANIM_MS_MAIN     500
#define ANIM_MS_KNOB     300

// ---------------- Man hinh cho ----------------
#define BAND_H        16         // do cao 1 dai ghep anh (240 = 15 dai)
#define EARTH_REV_MS  14000UL    // 1 vong quay trai dat
#define LK_EX         244        // vi tri trai dat tren man hinh khoa
#define LK_EY         22
#define WALL_FB       153600UL   // 320*240*2
#define JPG_MAX       24576UL    // khung JPEG toi da (byte) cho anh nen CYJ1
#define WALL_FB_HALF  38400UL    // 160*120*2 (CYA2: phong to 2x khi hien -> doc nhanh 4 lan, video muot)
#define CYA_HDR       16

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = TFT_SCLK_PIN;
      cfg.pin_mosi   = TFT_MOSI_PIN;
      cfg.pin_miso   = TFT_MISO_PIN;
      cfg.pin_dc     = TFT_DC_PIN;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs       = TFT_CS_PIN;
      cfg.pin_rst      = -1;
      cfg.panel_width  = 240;
      cfg.panel_height = 320;
      cfg.invert       = PANEL_INVERT;
      cfg.rgb_order    = PANEL_RGB_ORDER;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
LGFX_Sprite pill(&tft);      // nut ngay/dem
LGFX_Sprite band(&tft);      // dai ghep man hinh cho (320 x 16)
LGFX_Sprite jspr(&tft);      // khung 160x120 giai ma tu JPEG (chi tao khi dung anh nen CYJ1)
Preferences prefs;
WebServer web(80);
SPIClass sdSPI(VSPI);

typedef lgfx::LovyanGFX GFX;
typedef const lgfx::IFont* FNT;

// Kieu dung trong tham so ham phai dat o day (Arduino tu chen khai bao ham len dau file)
struct PanelR { int x, y, w, h; };
struct R { int x, y, w, h; };

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// ---------------- Nut ngay/dem: kich thuoc (nhu v4) ----------------
static const int   CW = 225, CH = 100;          // container (cung la kich thuoc sprite)
static const int   PM = 6;                      // le quanh container cho vien sang (glow)
static const int   SW = CW + 2 * PM;            // vung ve ca vien sang: 237
static const int   SH = CH + 2 * PM;            // 112
static const int   SPR_X = 41, SPR_Y = 52;      // goc vung (gom vien sang) tren man hinh
static const int   PILL_X = SPR_X + PM, PILL_Y = SPR_Y + PM, PILL_W = CW, PILL_H = CH;

#define BG_R 12
#define BG_G 16
#define BG_B 25
static const uint16_t COL_BG     = RGB565(BG_R, BG_G, BG_B);
static const uint16_t COL_WHITE  = 0xFFFF;
static const uint16_t COL_RED    = 0xF800;
static const uint16_t COL_YELLOW = 0xFFE0;
static const uint16_t COL_GREY   = 0x7BEF;
static const uint16_t COL_BLACK  = 0x0000;
static const uint16_t COL_GREEN  = RGB565(80, 220, 120);
static const uint16_t COL_GOLD   = RGB565(255, 212, 59);
static const uint16_t COL_TXT    = RGB565(232, 238, 247);
static const uint16_t COL_MUTED  = RGB565(143, 157, 179);
static const uint16_t COL_DIM    = RGB565(70, 82, 104);
static const uint16_t COL_BLUE   = RGB565(90, 160, 255);
static const uint16_t COL_TRACK  = RGB565(30, 40, 58);
static const uint16_t COL_BTN    = RGB565(34, 44, 62);
static const uint16_t COL_BTNLN  = RGB565(90, 104, 130);

static bool pillOk = false, bandOk = false;


struct RGB { float r, g, b; };
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float circCov(float dist, float r) { return clampf(r - dist + 0.5f, 0.f, 1.f); }
static inline void  mixTo(float &r, float &g, float &b, const RGB& c, float a) {
  r += (c.r - r) * a; g += (c.g - g) * a; b += (c.b - b) * a;
}
static inline uint16_t pack565f(float rf, float gf, float bf) {
  int r = (int)(clampf(rf, 0.f, 255.f) + 0.5f);
  int g = (int)(clampf(gf, 0.f, 255.f) + 0.5f);
  int b = (int)(clampf(bf, 0.f, 255.f) + 0.5f);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Can bac hai nhanh (1/sqrt kieu Quake + 2 buoc Newton, sai so ~5e-6): ESP32 khong co lenh sqrt phan cung
static inline float fsqrt(float x) {
  if (x <= 0.f) return 0.f;
  uint32_t i;
  memcpy(&i, &x, sizeof(i));
  i = 0x5f3759dfu - (i >> 1);
  float y;
  memcpy(&y, &i, sizeof(y));
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  return x * y;
}

// ---- Mau (dung nhu CSS) ----
static const RGB C_WHITE   = {255.f, 255.f, 255.f};
static const RGB C_DAY_BG  = {61.f, 126.f, 174.f};     // --container-light-bg #3D7EAE
static const RGB C_NIGHT_BG= {29.f, 31.f, 44.f};       // --container-night-bg #1D1F2C
static const RGB C_SUN     = {236.f, 202.f, 47.f};     // --sun-bg #ECCA2F
static const RGB C_SUN_DK  = {161.f, 135.f, 42.f};     // #a1872a
static const RGB C_MOON    = {196.f, 201.f, 209.f};    // --moon-bg #C4C9D1
static const RGB C_MOON_DK = {150.f, 150.f, 150.f};    // #969696
static const RGB C_SPOT    = {149.f, 157.f, 177.f};    // --spot-color #959DB1
static const RGB C_RIM     = {254.f, 255.f, 239.f};    // rgba(254,255,239,.61)
static const RGB C_CLOUD   = {243.f, 253.f, 255.f};    // --clouds-color #F3FDFF
static const RGB C_BACKCL  = {170.f, 202.f, 223.f};    // --back-clouds-color #AACADF

// ---- May: tam (x,y) o trang thai ban ngay, ban kinh, 1 = may sau (mau xanh nhat) ----
// Thu tu ve tu SAU ra TRUOC (nguoc voi danh sach box-shadow), cuoi cung la phan tu goc.
// Khi chuyen sang dem ca dam may truot xuong 137.5px (bottom: -.625em -> -4.062em).
struct Cl { float x, y, r; uint8_t back; };
static const Cl CLOUDS[] = {
  {202.5f,  15.0f, 42.5f, 1}, {197.5f,  75.0f, 25.f, 1}, {222.5f,  30.0f, 42.5f, 0},
  {172.5f,  82.5f, 25.f, 1},  {217.5f,  87.5f, 25.f, 0}, {142.5f, 100.0f, 25.f, 1},
  {182.5f,  97.5f, 25.f, 0},  {117.5f,  87.5f, 25.f, 1}, {155.0f, 112.5f, 25.f, 0},
  { 87.5f,  97.5f, 25.f, 1},  {125.0f, 100.0f, 25.f, 0}, { 57.5f,  95.0f, 25.f, 1},
  { 95.0f, 115.0f, 25.f, 0},  { 25.0f,  87.5f, 25.f, 1}, { 75.0f, 112.5f, 25.f, 0},
  { 37.5f, 100.0f, 25.f, 0}
};
static const int NCL = sizeof(CLOUDS) / sizeof(CLOUDS[0]);
static const float CLOUD_DROP = 137.5f;        // (4.062 - 0.625)em * 40px

// ---- Sao: toa do trong viewBox 144x55 cua SVG; h = nua kich thuoc ----
// Khoi sao dai 110px (2.75em), cao ~42px; top: -100% (den tren container) -> 50% & translateY(-50%) = 29px
struct St { float x, y, h; };
static const St STARS[] = {
  {20.f, 12.f, 7.f}, {48.f, 40.f, 4.f}, {80.f, 14.f, 8.f}, {112.f, 38.f, 5.f}, {131.f, 12.f, 5.f},
  {66.f, 44.f, 3.f}, {100.f, 24.f, 3.f}, {34.f, 32.f, 3.f}, {8.f, 40.f, 3.f}
};
static const int NST = sizeof(STARS) / sizeof(STARS[0]);
static const float STAR_TOP_DAY = -100.f, STAR_TOP_NIGHT = 29.f;

// Do phu cua sao (4 canh nhon, canh cong): sqrt(|dx|/h) + sqrt(|dy|/h) <= 1, lay mau 3x3
// T = toa do y cua mep tren khoi sao (thay doi khi dang chuyen dong)
static float starCov(float px, float py, float T) {
  const float k = 110.f / 144.f;                 // svg rong 2.75em = 110px
  float vx0 = (px - 12.5f) / k;                  // container sao: left .312em
  float vy0 = (py - T) / k;
  if (vx0 < -2.f || vx0 > 146.f || vy0 < -2.f || vy0 > 57.f) return 0.f;
  bool nearStar = false;
  for (int i = 0; i < NST; i++) {
    if (fabsf(vx0 - STARS[i].x) < STARS[i].h + 1.5f && fabsf(vy0 - STARS[i].y) < STARS[i].h + 1.5f) { nearStar = true; break; }
  }
  if (!nearStar) return 0.f;
  int hit = 0;
  for (int sy = 0; sy < 3; sy++) {
    for (int sx = 0; sx < 3; sx++) {
      float vx = vx0 + ((sx - 1) * 0.3333f) / k;
      float vy = vy0 + ((sy - 1) * 0.3333f) / k;
      for (int i = 0; i < NST; i++) {
        float dx = fabsf(vx - STARS[i].x), dy = fabsf(vy - STARS[i].y), h = STARS[i].h;
        if (dx >= h || dy >= h) continue;
        if (fsqrt(dx / h) + fsqrt(dy / h) <= 1.f) { hit++; break; }
      }
    }
  }
  return hit / 9.f;
}

// Mau mat troi + mat trang (dang truot vao) tai (u,v) so voi tam nut. Ban kinh 2.125em/2 = 42.5
// shift = do dich ngang cua mat trang (translateX: 100% = 85px khi ban ngay, 0 khi ban dem)
static void knobRGB(float u, float v, float shift, float covK, float &r, float &g, float &b) {
  const float R = 42.5f;

  // --- mat troi ---
  r = C_SUN.r; g = C_SUN.g; b = C_SUN.b;
  float dd2 = R - fsqrt(u * u + (v + 2.5f) * (v + 2.5f));                 // inset 0 -.062em .062em #a1872a
  mixTo(r, g, b, C_SUN_DK, clampf(0.5f - dd2 / 2.5f, 0.f, 1.f));
  float dd1 = R - fsqrt((u - 2.5f) * (u - 2.5f) + (v - 2.5f) * (v - 2.5f));  // inset .062em .062em .062em rgba(254,255,239,.61)
  mixTo(r, g, b, C_RIM, 0.61f * clampf(0.5f - dd1 / 2.5f, 0.f, 1.f));

  // --- mat trang (bi cat boi hinh tron cua dia) ---
  float um = u - shift, vm = v;
  float dm2 = um * um + vm * vm;
  if (dm2 < 43.5f * 43.5f) {
    float covM = circCov(fsqrt(dm2), R);
    if (covM > 0.f) {
      float mr = C_MOON.r, mg = C_MOON.g, mb = C_MOON.b;
      float e2 = R - fsqrt(um * um + (vm + 2.5f) * (vm + 2.5f));
      mixTo(mr, mg, mb, C_MOON_DK, clampf(0.5f - e2 / 2.5f, 0.f, 1.f));
      float e1 = R - fsqrt((um - 2.5f) * (um - 2.5f) + (vm - 2.5f) * (vm - 2.5f));
      mixTo(mr, mg, mb, C_RIM, 0.61f * clampf(0.5f - e1 / 2.5f, 0.f, 1.f));

      // 3 "vet" (crater): tam lech so voi tam mat trang, ban kinh
      static const float SP[3][3] = { {-15.f, 2.5f, 15.f}, {20.f, 2.5f, 7.5f}, {-5.f, -25.f, 5.f} };
      for (int i = 0; i < 3; i++) {
        float du = um - SP[i][0], dv = vm - SP[i][1], sr = SP[i][2];
        if (fabsf(du) > sr + 1.f || fabsf(dv) > sr + 1.f) continue;
        float cov = circCov(fsqrt(du * du + dv * dv), sr);
        if (cov > 0.f) {
          float dd = sr - fsqrt(du * du + (dv - 1.25f) * (dv - 1.25f));    // inset 0 .0312em .062em rgba(0,0,0,.25)
          float dark = 1.f - 0.25f * clampf(0.5f - dd / 2.5f, 0.f, 1.f);
          float sr_ = C_SPOT.r * dark, sg_ = C_SPOT.g * dark, sb_ = C_SPOT.b * dark;
          mr += (sr_ - mr) * cov; mg += (sg_ - mg) * cov; mb += (sb_ - mb) * cov;
        }
      }
      // mat trang cung ban kinh voi dia: o mep dung ti le de khong lo vien vang cua mat troi
      float ratio = (covK > 0.001f) ? clampf(covM / covK, 0.f, 1.f) : 1.f;
      r += (mr - r) * ratio; g += (mg - g) * ratio; b += (mb - b) * ratio;
    }
  }
}

// Mau vien sang ngoai (box-shadow trang + bong den tren) tai (px,py) toa do container
static void glowColor(float px, float py, float &r, float &g, float &b) {
  float qx = px < 50.f ? 50.f : (px > 175.f ? 175.f : px);
  float ex = px - qx;
  float ds = py - 52.5f;                                   // capsule dich xuong 2.5px
  float du = py - 47.5f;                                   // capsule dich len 2.5px
  float dsd = (ex == 0.f) ? fabsf(ds) : fsqrt(ex * ex + ds * ds);
  float dud = (ex == 0.f) ? fabsf(du) : fsqrt(ex * ex + du * du);
  float gA = 0.94f * clampf(0.5f + (50.f - dsd) / 5.f, 0.f, 1.f);       // 0 .062em .125em rgba(255,255,255,.94)
  r = BG_R + (255.f - BG_R) * gA;
  g = BG_G + (255.f - BG_G) * gA;
  b = BG_B + (255.f - BG_B) * gA;
  float dA = 0.25f * clampf(0.5f + (50.f - dud) / 2.5f, 0.f, 1.f);      // 0 -.062em .062em rgba(0,0,0,.25)
  r *= (1.f - dA); g *= (1.f - dA); b *= (1.f - dA);
}

// 1 diem cua vien sang ngoai tai (sx,sy) trong vung SW x SH. false = diem chi co mau nen (bo qua)
static bool haloPix(int sx, int sy, uint16_t& col) {
  if (sx >= PM && sx < PM + CW && sy >= PM && sy < PM + CH) return false;   // phan trong container do sprite lo
  float px = (float)(sx - PM) + 0.5f;
  float py = (float)(sy - PM) + 0.5f;
  float qx = px < 50.f ? 50.f : (px > 175.f ? 175.f : px);
  float ex = px - qx, ey = py - 50.f;
  if (50.f - fsqrt(ex * ex + ey * ey) < -8.f) return false;
  float r, gg, b;
  glowColor(px, py, r, gg, b);
  if (fabsf(r - BG_R) < 0.6f && fabsf(gg - BG_G) < 0.6f && fabsf(b - BG_B) < 0.6f) return false;
  col = pack565f(r, gg, b);
  return true;
}
// Vien sang khong doi -> tinh 1 lan luc khoi dong, ve lai chi la duyet danh sach (khong con can bac hai moi diem)
static uint16_t* haloP = nullptr;      // vi tri = sy * SW + sx
static uint16_t* haloC = nullptr;      // mau RGB565
static int haloN = 0;
static void haloBuild() {
  if (haloP) return;
  int n = 0;
  uint16_t c;
  for (int sy = 0; sy < SH; sy++) for (int sx = 0; sx < SW; sx++) if (haloPix(sx, sy, c)) n++;
  if (n == 0) return;
  haloP = (uint16_t*)malloc((size_t)n * 2);
  haloC = (uint16_t*)malloc((size_t)n * 2);
  if (!haloP || !haloC) { free(haloP); free(haloC); haloP = haloC = nullptr; return; }
  int i = 0;
  for (int sy = 0; sy < SH; sy++) for (int sx = 0; sx < SW; sx++) {
    if (i < n && haloPix(sx, sy, c)) { haloP[i] = (uint16_t)(sy * SW + sx); haloC[i] = c; i++; }
  }
  haloN = i;
}
// Ve vien sang NGOAI container len g (oy = lech doc). Diem nao chi co mau nen thi bo qua de thay duoc anh nen.
void drawHaloG(GFX* g, int oy) {
  const int gh = g->height();
  if (haloN > 0) {
    for (int i = 0; i < haloN; i++) {
      int p = haloP[i];
      int sy = p / SW;
      int yy = SPR_Y + sy + oy;
      if (yy < 0 || yy >= gh) continue;
      g->drawPixel(SPR_X + (p - sy * SW), yy, haloC[i]);
    }
    return;
  }
  for (int sy = 0; sy < SH; sy++) {                    // du phong neu khong du RAM cho danh sach
    int yy = SPR_Y + sy + oy;
    if (yy < 0 || yy >= gh) continue;
    for (int sx = 0; sx < SW; sx++) {
      uint16_t c;
      if (haloPix(sx, sy, c)) g->drawPixel(SPR_X + sx, yy, c);
    }
  }
}

// ---- Duong cong cubic-bezier(x1,y1,x2,y2) giong CSS: tra ve y theo thoi gian t (0..1) ----
static float cubicBezier(float t, float x1, float y1, float x2, float y2) {
  if (t <= 0.f) return 0.f;
  if (t >= 1.f) return 1.f;
  float lo = 0.f, hi = 1.f, s = t;
  for (int i = 0; i < 18; i++) {
    s = 0.5f * (lo + hi);
    float inv = 1.f - s;
    float xs = 3.f * inv * inv * s * x1 + 3.f * inv * s * s * x2 + s * s * s;
    if (xs < t) lo = s; else hi = s;
  }
  float inv = 1.f - s;
  return 3.f * inv * inv * s * y1 + 3.f * inv * s * s * y2 + s * s * s;
}

// Mau "trong suot" cua sprite nut (goc ngoai vien thuoc) de thay anh nen phia sau
static const uint16_t PILL_KEY = 0xF81F;
static inline uint16_t px565(float r, float g, float b) {
  uint16_t c = pack565f(r, g, b);
  return c == PILL_KEY ? (uint16_t)(c ^ 0x0020) : c;
}
// Ghi 1 diem (step=1) hoac 1 khoi step x step (step=2) vao sprite
static inline void putPx(int x, int y, int step, uint16_t c) {
  if (step <= 1) { pill.drawPixel(x, y, c); return; }
  int w = (CW - x < step) ? (CW - x) : step;
  int h = (CH - y < step) ? (CH - y) : step;
  pill.fillRect(x, y, w, h, c);
}

// Ve 1 khung cua nut.  step : 1 = day du chi tiet; 2 = khoi 2x2 (nhanh ~4 lan, dung luc dang chuyen dong)
//  n5 : 0 = ban ngay ... 1 = ban dem   (nen, mat trang, may, sao)  - co the vuot nhe 0..1 do duong cong nay
//  nk : 0 = nut o ben trai ... 1 = nut o ben phai
void renderFrame(float n5, float nk, int step, bool push = true) {
  const float kx       = 50.f + 125.f * nk;               // tam nut
  const float shift    = 85.f * (1.f - n5);               // mat trang: translateX(100%) -> 0
  const float cloudOff = CLOUD_DROP * n5;                 // may truot xuong
  const float starT    = STAR_TOP_DAY + (STAR_TOP_NIGHT - STAR_TOP_DAY) * n5;   // sao roi tu tren xuong
  const float n5c      = clampf(n5, 0.f, 1.f);
  const float bgR = clampf(C_DAY_BG.r + (C_NIGHT_BG.r - C_DAY_BG.r) * n5c, 0.f, 255.f);
  const float bgG = clampf(C_DAY_BG.g + (C_NIGHT_BG.g - C_DAY_BG.g) * n5c, 0.f, 255.f);
  const float bgB = clampf(C_DAY_BG.b + (C_NIGHT_BG.b - C_DAY_BG.b) * n5c, 0.f, 255.f);
  const bool showClouds = cloudOff < 128.f;
  const bool showStars  = (starT + 43.f > 0.f) && (starT < 100.f);
  const float half = 0.5f * (float)step;

  for (int y = 0; y < CH; y += step) {
    const float py  = (float)y + half;
    const float dyk = py - 50.f;                 // so voi tam nut
    const float dyk2 = dyk * dyk;
    const float dsh2 = (py - 55.f) * (py - 55.f);   // tam bong do cua nut thap hon 5px
    const float dsi  = dyk - 2.f;                // bong toi trong container dich xuong 2px

    for (int x = 0; x < CW; x += step) {
      const float px = (float)x + half;
      const float qx = px < 50.f ? 50.f : (px > 175.f ? 175.f : px);
      const float ex = px - qx;                  // 0 o doan giua cua vien thuoc
      const float inside = (ex == 0.f) ? (50.f - fabsf(dyk)) : (50.f - fsqrt(ex * ex + dyk2));
      const float cov = clampf(inside + 0.5f, 0.f, 1.f);

      float r, g, b;
      if (cov <= 0.f) {                          // goc ngoai vien thuoc -> chi con vien sang
        glowColor(px, py, r, g, b);
        if (fabsf(r - BG_R) < 0.6f && fabsf(g - BG_G) < 0.6f && fabsf(b - BG_B) < 0.6f) putPx(x, y, step, PILL_KEY);
        else putPx(x, y, step, px565(r, g, b));
        continue;
      }

      r = bgR; g = bgG; b = bgB;

      // may (chi khi con nhin thay)
      if (showClouds) {
        for (int i = 0; i < NCL; i++) {
          float cy = CLOUDS[i].y + cloudOff;
          float dx = px - CLOUDS[i].x, dy = py - cy, cr = CLOUDS[i].r;
          if (fabsf(dx) > cr + 1.f || fabsf(dy) > cr + 1.f) continue;
          float cc = circCov(fsqrt(dx * dx + dy * dy), cr);
          if (cc > 0.f) mixTo(r, g, b, CLOUDS[i].back ? C_BACKCL : C_CLOUD, cc);
        }
      }
      // sao
      if (showStars) {
        float sc = starCov(px, py, starT);
        if (sc > 0.f) mixTo(r, g, b, C_WHITE, sc);
      }

      // vong tron mo quanh nut (circle-container): ban kinh 67.5, +25, +50
      const float dxk = px - kx;
      const float d2  = dxk * dxk + dyk2;
      float dk = 1e9f;
      if (d2 < 13806.25f) {
        dk = fsqrt(d2);
        float ra = 0.1f * circCov(dk, 117.5f) + 0.09f * circCov(dk, 92.5f) + 0.081f * circCov(dk, 67.5f);
        if (ra > 0.f) mixTo(r, g, b, C_WHITE, ra);
      }

      // bong toi ben trong container (2 lop inset 0 .05em .187em rgba(0,0,0,.25))
      {
        float sd = (ex == 0.f) ? fabsf(dsi) : fsqrt(ex * ex + dsi * dsi);
        float d = 50.f - sd;
        if (d < 3.75f) {
          float a = 0.25f * clampf(0.5f - d / 7.5f, 0.f, 1.f);
          float keep = (1.f - a) * (1.f - a);
          r *= keep; g *= keep; b *= keep;
        }
      }

      // bong do cua nut (drop-shadow)
      {
        float ds2 = dxk * dxk + dsh2;
        if (ds2 < 2352.25f) {
          float sa = 0.3f * clampf(0.5f + (42.5f - fsqrt(ds2)) / 5.f, 0.f, 1.f);
          if (sa > 0.f) { r *= (1.f - sa); g *= (1.f - sa); b *= (1.f - sa); }
        }
      }

      // mat troi / mat trang
      if (dk < 43.5f) {
        float covK = circCov(dk, 42.5f);
        if (covK > 0.f) {
          float kr, kg, kb;
          knobRGB(dxk, dyk, shift, covK, kr, kg, kb);
          r += (kr - r) * covK; g += (kg - g) * covK; b += (kb - b) * covK;
        }
      }

      // mep vien thuoc: tron voi vien sang ben ngoai
      if (cov < 1.f) {
        float gr, gg, gb;
        glowColor(px, py, gr, gg, gb);
        r = gr + (r - gr) * cov; g = gg + (g - gg) * cov; b = gb + (b - gb) * cov;
      }

      putPx(x, y, step, px565(r, g, b));
    }
  }
  if (push) pill.pushSprite(PILL_X, PILL_Y, PILL_KEY);
}



// ================= TRANG THAI CHUNG =================
enum { PG_MAIN = 0, PG_INFO, PG_GRAPH, PG_CYD, PG_WALL, PG_SET, PG_COUNT };
static int  page = PG_MAIN;
static bool locked = false;              // dang o man hinh cho (ke ca luc dang truot)
static bool lkSliding = false;           // dang truot len
static int  lkOff = 0;
static uint32_t lastAct = 0;
static volatile bool lockReq = false;
static int  pgNext = SCR_H;              // vi tri dai ke tiep cua lan ve lai trang dang chay nen (>= SCR_H: roi)

int lamp = -1;                    // -1 chua biet, 0 tat, 1 sang
int lampBeforeCmd = -1;

// ---- Thong so relay doc tu /status (task mang ghi, loop() doc ban sao) ----
struct RelayInfo {
  bool valid;
  int  lamp;
  long heap, ram, sketch, flash, vcc, rssi, up, cpu, idle, left, tleft, tset;
  bool se, sleeping, synced, ap, mqttcfg, mqtt;
  char ip[20], time[12], on[10], off[10];
};
static RelayInfo ri;
static volatile uint32_t riSeq = 0;

// ---- Giao tiep loop() <-> task mang ----
volatile int  cmdWant    = -1;    // loop -> task: 0/1 = lenh can gui, -1 = khong co
volatile bool cmdPending = false; // dang cho ket qua lenh
volatile int  evtType    = 0;     // task -> loop: 0 khong, 1 lenh OK, 2 lenh loi, 3 /status OK, 4 /status loi
volatile int  evtLamp    = 0;
volatile int  netFail    = 0;
IPAddress relayIP;

// ---- Cai dat (luu Preferences) ----
static const uint16_t IDLE_OPTS[] = {0, 30, 60, 120, 300, 600, 1800};
#define N_IDLE (int)(sizeof(IDLE_OPTS) / sizeof(IDLE_OPTS[0]))
static uint16_t cfgIdleSec = 60;         // 0 = khong bao gio vao man hinh cho
static bool     cfgRot     = false;      // xoay vong anh nen trong /wall moi lan vao man hinh cho
static uint8_t  cfgMask    = 0x0F;       // o thong so hien tren man hinh cho (toi da 5 o)
static uint8_t  cfgLockBr  = 35;         // do sang (%) khi o man hinh cho
static bool     cfgVidAll  = true;       // nen dong (video/GIF) chay o MOI man hinh, khong chi man hinh cho
static char     cfgWp[40]  = "flash";    // "flash" hoac "sd:<ten>"

static void loadCfg() {
  cfgIdleSec = prefs.getUShort("idle2", 60);
  cfgRot     = prefs.getBool("rot", false);
  cfgMask    = prefs.getUChar("mask", 0x0F);
  cfgLockBr  = prefs.getUChar("lbr", 35);
  cfgVidAll  = prefs.getBool("vall", true);
  String w = prefs.getString("wp", "flash");
  strlcpy(cfgWp, w.c_str(), sizeof(cfgWp));
  cfgMask &= 0x7F;                         // bo muc "anh sang"
  if (cfgLockBr < 5) cfgLockBr = 5;
  if (cfgLockBr > 100) cfgLockBr = 100;
}
static void saveCfg() {
  prefs.putUShort("idle2", cfgIdleSec);
  prefs.putBool("rot", cfgRot);
  prefs.putUChar("mask", cfgMask);
  prefs.putUChar("lbr", cfgLockBr);
  prefs.putBool("vall", cfgVidAll);
  prefs.putString("wp", cfgWp);
}

// ---- Den nen man hinh (PWM) ----
static void blInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL_PIN, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL_PIN, 0);
#endif
}
static void blSet(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  uint32_t duty = (uint32_t)pct * 255 / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// ================= HIEU UNG CHUYEN DONG NUT =================
static float    curN5 = 1.f, curNk = 1.f;       // gia tri hien tai (1 = dem / ben phai)
static bool     targetNight = true;
static bool     animOn = false;
static uint32_t animT0 = 0;
static float    a5From = 1.f, a5To = 1.f, akFrom = 1.f, akTo = 1.f;

void visualSet(bool night) {
  if (night == targetNight) return;
  targetNight = night;
  a5From = curN5; akFrom = curNk;
  a5To = night ? 1.f : 0.f; akTo = a5To;
  animT0 = millis();
  animOn = true;
}

// Dua nut ve dung trang thai cua den ngay lap tuc (khong chuyen dong, khong day ra man hinh)
static void visualSnapToLamp() {
  targetNight = (lamp != 1);
  curN5 = curNk = targetNight ? 1.f : 0.f;
  a5From = a5To = akFrom = akTo = curN5;
  animOn = false;
  if (pillOk) renderFrame(curN5, curNk, 1, false);
}

void animStep() {
  if (!animOn || page != PG_MAIN || locked || !pillOk) return;
  uint32_t el = millis() - animT0;
  bool last = false;
  if (el >= ANIM_MS_MAIN) {
    curN5 = a5To; curNk = akTo;
    animOn = false;
    last = true;
  } else {
    float t5 = (float)el / (float)ANIM_MS_MAIN;
    float tk = (float)el / (float)ANIM_MS_KNOB;
    curN5 = a5From + (a5To - a5From) * cubicBezier(t5, 0.f, -0.02f, 0.4f, 1.25f);
    curNk = (tk >= 1.f) ? akTo
                        : akFrom + (akTo - akFrom) * cubicBezier(tk, 0.f, -0.02f, 0.35f, 1.17f);
  }
  // dang chay: khoi stepCur x stepCur (tu dieu chinh theo toc do thuc: cham -> khoi to hon); khung cuoi: day du
  static int stepCur = ANIM_STEP;
  uint32_t tf = millis();
  renderFrame(curN5, curNk, last ? 1 : stepCur);
  uint32_t dt = millis() - tf;
  if (!last) {
    if (dt > 55 && stepCur < 4) stepCur++;
    else if (dt < 25 && stepCur > ANIM_STEP) stepCur--;
  }
}

// ================= CHU / DINH DANG =================
static const FNT F0 = &fonts::Font0;
static const FNT F2 = &fonts::Font2;
static const FNT F4 = &fonts::Font4;
static const FNT F7 = &fonts::Font7;

static void T(GFX* g, const char* s, int x, int y, FNT f, uint16_t fg,
              lgfx::textdatum_t d = top_left, int bg = -1, int pad = 0) {
  g->setFont(f);
  g->setTextDatum(d);
  if (bg >= 0) g->setTextColor(fg, (uint16_t)bg); else g->setTextColor(fg);
  g->setTextPadding(pad);
  g->drawString(s, x, y);
  g->setTextPadding(0);
}

// "0.38" (khong don vi). Tong < 0.5MB -> 3 so le, < 10MB -> 2, < 100MB -> 1, con lai 0. Tong >= 10GB tinh bang GB.
static void fmtNum(char* out, size_t n, uint64_t bytes, uint64_t total) {
  bool gb = total >= 10240ULL * 1048576ULL;
  double div = gb ? 1073741824.0 : 1048576.0;
  double t = (double)total / div;
  int dec = gb ? 2 : (t < 0.5 ? 3 : (t < 10 ? 2 : (t < 100 ? 1 : 0)));
  snprintf(out, n, "%.*f", dec, (double)bytes / div);
}
static const char* unitOf(uint64_t total) { return total >= 10240ULL * 1048576ULL ? "GB" : "MB"; }
static void fmtPair(char* out, size_t n, uint64_t used, uint64_t total) {
  char a[16], b[16];
  fmtNum(a, sizeof(a), used, total);
  fmtNum(b, sizeof(b), total, total);
  snprintf(out, n, "%s / %s %s", a, b, unitOf(total));
}
static int pctOf(uint64_t used, uint64_t total) {
  if (!total) return 0;
  int p = (int)(used * 100ULL / total);
  return p > 100 ? 100 : p;
}

// ================= TRANG CHINH (ve duoc len man hinh HOAC len dai ghep) =================
static void pageRange(int y0, int y1);       // ve lai cac dai [y0, y1) cua trang dang xem
static void pageRender();                    // ve lai ca trang dang xem

static char     statusMsg[48] = "Dang tim relay...";
static uint16_t statusCol = COL_GREY;
static uint16_t linkCol   = COL_YELLOW;

static void drawDots(GFX* g, int oy) {
  for (int i = 0; i < PG_COUNT; i++) {
    int y = 96 + i * 12 + oy;
    if (i == page) g->fillCircle(314, y, 3, COL_WHITE);
    else           g->fillCircle(314, y, 2, COL_DIM);
  }
}
static void drawLinkDot(GFX* g, int oy) { g->fillCircle(300, 18 + oy, 7, linkCol); }

static void drawLabelG(GFX* g, int oy) {
  if (lamp == 1)      T(g, "DEN SANG", 160, 184 + oy, F4, COL_GOLD, middle_center);
  else if (lamp == 0) T(g, "DEN TAT", 160, 184 + oy, F4, COL_MUTED, middle_center);
  else                T(g, "...", 160, 184 + oy, F4, COL_MUTED, middle_center);
}
static void drawStatusG(GFX* g, int oy) {
  T(g, statusMsg, 160, 222 + oy, F2, statusCol, middle_center);
}

// oy = do lech doc cua trang chinh so voi toa do g (nen da duoc ve san ben duoi)
static void mainDraw(GFX* g, int oy) {
  T(g, "Den thong minh", 12, 10 + oy, F4, COL_TXT);
  drawLinkDot(g, oy);
  drawHaloG(g, oy);                                   // vien sang ngoai nut
  if (pillOk) pill.pushSprite(g, PILL_X, PILL_Y + oy, PILL_KEY);
  drawLabelG(g, oy);
  drawStatusG(g, oy);
  drawDots(g, oy);
}

static void drawMainFull() {
  visualSnapToLamp();
  pageRender();
}

// ---- cac ham ve "an toan": chi ve khi dang thay trang tuong ung ----
static void drawLabel() { if (!locked && page == PG_MAIN) pageRange(168, 200); }
static void drawStatus(const char* msg, uint16_t col) {
  if (!strcmp(statusMsg, msg) && statusCol == col) return;
  strlcpy(statusMsg, msg, sizeof(statusMsg));
  statusCol = col;
  if (!locked && page == PG_MAIN) pageRange(208, SCR_H);
}
static void drawLink(uint16_t col) {
  if (linkCol == col) return;
  linkCol = col;
  if (!locked) pageRange(0, 32);
}
// Den doi trang thai: cap nhat nut / nhan theo trang dang xem
static void applyLampVisual() {
  if (locked || page != PG_MAIN) { visualSnapToLamp(); return; }
  visualSet(lamp != 1);
  pageRange(168, 200);
}

static void gotoPage(int p);
static void enterLock();
static void startSlide();


// ================= BO NHO: SD + LittleFS + ANH NEN =================
static SemaphoreHandle_t fsMux;
#define FS_LOCK()   xSemaphoreTake(fsMux, portMAX_DELAY)
#define FS_UNLOCK() xSemaphoreGive(fsMux)

static bool sdOk = false;
static bool fsOk = false;

static volatile uint64_t sdTotalC = 0, sdUsedC = 0;
static volatile uint32_t sdStamp = 0;            // 0 = can tinh lai
static bool sdMount() {
  FS_LOCK();
  SD.end();
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  sdOk = SD.begin(SD_CS_PIN, sdSPI, SD_FREQ);
  if (sdOk) { if (!SD.exists("/wall")) SD.mkdir("/wall"); }
  FS_UNLOCK();
  sdStamp = 0;
  Serial.printf("SD: %s\n", sdOk ? "OK" : "khong co the");
  return sdOk;
}

// Dung luong SD: tinh bang task web (de cham, co the mat vai giay), cac noi khac chi doc ban luu
static void sdRefresh() {
  if (!sdOk) { sdTotalC = 0; sdUsedC = 0; return; }
  if (sdStamp != 0 && millis() - sdStamp < 30000) return;
  FS_LOCK(); uint64_t t = SD.totalBytes(), u = SD.usedBytes(); FS_UNLOCK();
  sdTotalC = t; sdUsedC = u; sdStamp = millis() | 1;
}

// Doc header .cya, tra ve true neu file hop le
// kind: 0 = CYA1 (tho 320x240) | 1 = CYA2 (tho 160x120) | 2 = CYJ1 (moi khung la 1 anh JPEG 160x120, co bang chi muc)
static bool cyaInfoK(fs::File& f, uint16_t& frames, uint16_t& fps, uint8_t& kind) {
  uint8_t h[CYA_HDR];
  if (!f || f.size() < CYA_HDR) return false;
  f.seek(0);
  if (f.read(h, CYA_HDR) != CYA_HDR) return false;
  if (memcmp(h, "CYA1", 4) == 0) kind = 0;
  else if (memcmp(h, "CYA2", 4) == 0) kind = 1;
  else if (memcmp(h, "CYJ1", 4) == 0) kind = 2;
  else return false;
  frames = h[4] | (h[5] << 8);
  fps    = h[6] | (h[7] << 8);
  if (frames == 0) return false;                          // khong gioi han thoi luong (toi da 65535 khung)
  if (fps == 0 || fps > 30) fps = 8;
  uint64_t sz = f.size();
  if (kind == 2) {
    uint64_t idxEnd = (uint64_t)CYA_HDR + 4ULL * ((uint64_t)frames + 1);
    if (sz < idxEnd) return false;
    uint8_t e[4];
    if (!f.seek(CYA_HDR + 4UL * (uint32_t)frames) || f.read(e, 4) != 4) return false;
    uint64_t last = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
    return last >= idxEnd && last <= sz;                  // chi muc cuoi = tong dung luong -> file day du
  }
  uint64_t need = (uint64_t)CYA_HDR + (uint64_t)frames * (kind == 1 ? WALL_FB_HALF : WALL_FB);
  return sz >= need;
}
static bool cyaInfo(fs::File& f, uint16_t& frames, uint16_t& fps) { uint8_t k; return cyaInfoK(f, frames, fps, k); }

struct Wall { fs::File f; bool ok = false, half = false, jpg = false; uint16_t frames = 0, fps = 8, cur = 0; };
static Wall wall;
static volatile bool wallDirty = true;     // can mo lai anh nen
static uint16_t* hframe = nullptr;          // dem 1 khung CYA2 (38.4 KB): doc ca khung 1 lan thay vi tung dai
static int       hfCur = -1;               // khung dang nam trong hframe / jspr (-1 = chua co)
static uint8_t*  jbuf = nullptr;           // 1 khung JPEG doc tu the nho
static uint32_t  jbufCap = 0;
static bool      jReady = false, jswap = true;
static int       jbad = -1;                // khung JPEG loi (bo qua, khong thu lai moi dai)

static void hframeEnsure() {               // cap phat dem khung CYA2 neu con du RAM (khong dung khi dang phat JPEG)
  if (hframe || (wall.ok && wall.jpg)) return;
  if (ESP.getMaxAllocHeap() > WALL_FB_HALF + 60000UL && ESP.getFreeHeap() > WALL_FB_HALF + 70000UL) {
    hframe = (uint16_t*)malloc(WALL_FB_HALF);
    hfCur = -1;
  }
}
static void jpgRelease() {
  if (jReady) jspr.deleteSprite();
  if (jbuf) { free(jbuf); jbuf = nullptr; }
  jbufCap = 0; jReady = false; hfCur = -1; jbad = -1;
}
// Goi khi dang giu FS_LOCK va wall.f da mo (CYJ1): nhuong RAM cua dem CYA2 cho sprite giai ma + bo dem JPEG
static bool jpgPrepare() {
  jpgRelease();
  uint8_t h[4];
  if (!(wall.f.seek(12) && wall.f.read(h, 4) == 4)) return false;
  uint32_t mx = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
  if (mx < 64 || mx > JPG_MAX) return false;
  if (hframe) { free(hframe); hframe = nullptr; }
  jbuf = (uint8_t*)malloc(mx);
  if (!jbuf) return false;
  jbufCap = mx;
  jspr.setColorDepth(16);
  if (!jspr.createSprite(160, 120)) { free(jbuf); jbuf = nullptr; jbufCap = 0; return false; }
  jspr.drawPixel(0, 0, (uint16_t)0x07E0);                       // tu do thu tu byte cua dem sprite
  jswap = (((uint16_t*)jspr.getBuffer())[0] != 0x07E0);
  jReady = true;
  return true;
}

// ---- Dem anh nen trong RAM (chi dung luc keo man hinh cho len): moi 2 dong anh giu 1 dong ----
static uint16_t* wcache = nullptr;
static uint8_t   wcOk[WC_ROWS];
static int       wcFrame = -1;
static inline void wcReset() { memset(wcOk, 0, sizeof(wcOk)); wcFrame = -1; }
static void wcStore(int row, int n, const uint16_t* src) {
  if (!wcache) return;
  if (wcFrame != (int)wall.cur) { memset(wcOk, 0, sizeof(wcOk)); wcFrame = (int)wall.cur; }
  for (int i = 0; i < n; i++) {
    int r = row + i;
    if (r & 1) continue;
    memcpy(wcache + (size_t)(r >> 1) * SCR_W, src + (size_t)i * SCR_W, SCR_W * 2);
    wcOk[r >> 1] = 1;
  }
}
static bool wcLoad(int row, int n, uint16_t* dst) {
  if (!wcache || wcFrame != (int)wall.cur) return false;
  for (int i = 0; i < n; i++) if (!wcOk[(row + i) >> 1]) return false;
  for (int i = 0; i < n; i++) memcpy(dst + (size_t)i * SCR_W, wcache + (size_t)((row + i) >> 1) * SCR_W, SCR_W * 2);
  return true;
}

static void wallClose() {
  FS_LOCK();
  if (wall.f) wall.f.close();
  wall.ok = false;
  wcFrame = -1;
  hfCur = -1;
  FS_UNLOCK();
}
// goi tu luong web: nha file dang mo de co the xoa / doi ten
static void wallDetach() { wallClose(); wallDirty = true; }

static bool wallTry(bool sd, const char* path) {
  FS_LOCK();
  fs::File f;
  if (sd) { if (sdOk) f = SD.open(path, "r"); }
  else if (fsOk) f = LittleFS.open(path, "r");
  uint16_t fr = 0, fp = 8;
  uint8_t kind = 0;
  bool good = f && cyaInfoK(f, fr, fp, kind);
  if (good) {
    wall.f = f; wall.frames = fr; wall.fps = fp; wall.half = (kind != 0); wall.jpg = (kind == 2);
    wall.cur = 0; wall.ok = true; hfCur = -1;
    if (kind == 2) { if (!jpgPrepare()) { wall.f.close(); wall.ok = false; good = false; } }
    else { jpgRelease(); hframeEnsure(); }
  }
  else if (f) f.close();
  FS_UNLOCK();
  return good;
}
static void wallOpen() {
  wallClose();
  wallDirty = false;
  if (strncmp(cfgWp, "sd:", 3) == 0) {
    char p[64];
    snprintf(p, sizeof(p), "/wall/%s.cya", cfgWp + 3);
    if (wallTry(true, p)) return;
  }
  wallTry(false, "/bg.cya");
}
// ---- Anh nen CYA2 (160x120): phong to 2x, noi suy nhe theo ca hai chieu cho do vo vuong ----
static inline uint16_t avg565(uint16_t a, uint16_t b) { return ((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1); }
static uint16_t hbuf[160 * 10];                  // du phong khi khong du RAM cho ca khung (toi da 10 dong nguon / dai)
static void halfExpand(const uint16_t* src, int r0, int cnt, int row, int n, uint16_t* dst) {
  for (int i = 0; i < n; i++) {
    int r = row + i;
    int sidx = (r >> 1) - r0;
    const uint16_t* a = src + sidx * 160;
    const uint16_t* b = a;
    bool odd = (r & 1) != 0;
    if (odd) b = src + (sidx + 1 < cnt ? sidx + 1 : sidx) * 160;
    uint16_t* o = dst + i * SCR_W;
    uint16_t pv = odd ? avg565(a[0], b[0]) : a[0];
    for (int x = 0; x < 160; x++) {
      uint16_t q = (x < 159) ? (odd ? avg565(a[x + 1], b[x + 1]) : a[x + 1]) : pv;
      o[2 * x] = pv;
      o[2 * x + 1] = avg565(pv, q);
      pv = q;
    }
  }
}
static bool wallReadHalf(int row, int n, uint16_t* dst) {
  if (n < 1 || n > BAND_H || row < 0 || row + n > SCR_H) return false;
  const uint16_t* full = nullptr;                 // ca khung 160x120 dang nam trong RAM (neu co)
  bool ok = false, dec = false;
  int r0 = row >> 1, cnt = 0;
  uint32_t jlen = 0;
  FS_LOCK();
  if (wall.ok && wall.half) {
    if (wall.jpg) {                               // CYJ1: doc 1 khung JPEG theo bang chi muc, giai ma 1 lan cho ca khung
      if (jReady) {
        if (hfCur == (int)wall.cur) ok = true;
        else if (jbad != (int)wall.cur) {
          hfCur = -1;
          uint8_t ix[8];
          if (wall.f.seek(CYA_HDR + 4UL * (uint32_t)wall.cur) && wall.f.read(ix, 8) == 8) {
            uint32_t o0 = (uint32_t)ix[0] | ((uint32_t)ix[1] << 8) | ((uint32_t)ix[2] << 16) | ((uint32_t)ix[3] << 24);
            uint32_t o1 = (uint32_t)ix[4] | ((uint32_t)ix[5] << 8) | ((uint32_t)ix[6] << 16) | ((uint32_t)ix[7] << 24);
            if (o1 > o0 && (o1 - o0) <= jbufCap && wall.f.seek(o0) && wall.f.read(jbuf, o1 - o0) == (o1 - o0) &&
                jbuf[0] == 0xFF && jbuf[1] == 0xD8) { jlen = o1 - o0; dec = true; }
          }
          if (!dec) jbad = (int)wall.cur;
        }
        if (ok || dec) full = (const uint16_t*)jspr.getBuffer();
      }
    } else if (hframe) {                          // CYA2 co dem khung
      if (hfCur != (int)wall.cur) {
        hfCur = -1;
        if (wall.f.seek(CYA_HDR + (uint32_t)wall.cur * WALL_FB_HALF) &&
            wall.f.read((uint8_t*)hframe, WALL_FB_HALF) == WALL_FB_HALF) hfCur = (int)wall.cur;
      }
      ok = (hfCur == (int)wall.cur);
      if (ok) full = hframe;
    } else {                                      // CYA2 thieu RAM: doc tung dai
      int r1 = (row + n - 1) >> 1;
      int rr = (r1 + 1 > 119) ? 119 : r1 + 1;
      cnt = rr - r0 + 1;
      size_t want = (size_t)cnt * 320;
      ok = wall.f.seek(CYA_HDR + (uint32_t)wall.cur * WALL_FB_HALF + (uint32_t)r0 * 320UL) &&
           wall.f.read((uint8_t*)hbuf, want) == want;
    }
  }
  FS_UNLOCK();
  if (dec) {                                      // giai ma ngoai khoa the nho (khong chan web)
    jspr.drawJpg(jbuf, jlen, 0, 0);
    if (jswap) {
      uint16_t* q = (uint16_t*)jspr.getBuffer();
      for (int i = 0; i < 160 * 120; i++) q[i] = (uint16_t)((q[i] << 8) | (q[i] >> 8));
    }
    hfCur = (int)wall.cur;
    ok = true;
  }
  if (!ok) return false;
  if (full) halfExpand(full, 0, 120, row, n, dst);
  else      halfExpand(hbuf, r0, cnt, row, n, dst);
  return true;
}
static bool wallReadRows(int row, int n, uint16_t* dst) {
  if (wall.ok && wall.half) return wallReadHalf(row, n, dst);
  if (lkSliding && wcLoad(row, n, dst)) return true;      // luc keo man hinh: lay tu RAM, khong cham SD
  bool ok = false;
  FS_LOCK();
  if (wall.ok) {
    uint32_t pos = CYA_HDR + (uint32_t)wall.cur * WALL_FB + (uint32_t)row * 640UL;
    size_t want = (size_t)n * 640;
    if (wall.f.seek(pos)) ok = (wall.f.read((uint8_t*)dst, want) == want);
  }
  FS_UNLOCK();
  if (ok) wcStore(row, n, dst);
  return ok;
}

// Xoay vong: chon file ke tiep trong /wall
static void wallRotate() {
  if (!sdOk) return;
  String names[24];
  int n = 0, curIdx = -1;
  FS_LOCK();
  fs::File d = SD.open("/wall");
  if (d) {
    for (;;) {
      fs::File e = d.openNextFile();
      if (!e) break;
      String nm = e.name();
      int sl = nm.lastIndexOf('/');
      if (sl >= 0) nm = nm.substring(sl + 1);
      if (!e.isDirectory() && nm.endsWith(".cya") && n < 24) names[n++] = nm.substring(0, nm.length() - 4);
      e.close();
    }
    d.close();
  }
  FS_UNLOCK();
  if (n == 0) return;
  for (int i = 0; i < n; i++) if (strncmp(cfgWp, "sd:", 3) == 0 && names[i] == (cfgWp + 3)) curIdx = i;
  int nx = (curIdx + 1) % n;
  snprintf(cfgWp, sizeof(cfgWp), "sd:%s", names[nx].c_str());
  prefs.putString("wp", cfgWp);
}

// ================= MAN HINH CHO =================
static uint16_t rowbuf[SCR_W * BAND_H];
static inline uint16_t dimPx(uint16_t c) { return ((c >> 2) & 0x39E7) + ((c >> 3) & 0x18E3); }   // toi con ~37%
static int  eShift = 0;
static int  digW = 32;
static uint32_t lkNextEarth = 0, lkNextFrame = 0, lkNextChk = 0, lkSig = 0;

static char lkHH[3] = "--", lkMM[3] = "--", lkSS[3] = "--", lkDate[28] = "";
static bool lkColon = true;
static char lkLab[5][14], lkVal[5][40];
static int  lkNChip = 0;
static char lkHint[56] = "";

static const PanelR PNL_A = {8, 16, 200, 84};
static const PanelR PNL_B = {8, 106, 304, 102};
static const PanelR PNL_C = {8, 212, 304, 22};

static void getClock() {
  static const char* DOW[] = {"CN", "T2", "T3", "T4", "T5", "T6", "T7"};
  struct tm t;
  if (getLocalTime(&t, 0)) {
    snprintf(lkHH, 3, "%02d", t.tm_hour); snprintf(lkMM, 3, "%02d", t.tm_min); snprintf(lkSS, 3, "%02d", t.tm_sec);
    snprintf(lkDate, sizeof(lkDate), "%s  %02d/%02d/%04d", DOW[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    lkColon = (t.tm_sec % 2) == 0;
  } else if (ri.valid && ri.synced && strlen(ri.time) >= 8) {
    lkHH[0] = ri.time[0]; lkHH[1] = ri.time[1]; lkHH[2] = 0;
    lkMM[0] = ri.time[3]; lkMM[1] = ri.time[4]; lkMM[2] = 0;
    lkSS[0] = ri.time[6]; lkSS[1] = ri.time[7]; lkSS[2] = 0;
    lkDate[0] = 0;
    lkColon = (atoi(lkSS) % 2) == 0;
  } else {
    strcpy(lkHH, "--"); strcpy(lkMM, "--"); strcpy(lkSS, "--"); lkDate[0] = 0; lkColon = true;
  }
}

static void addChip(const char* lab, const char* val) {
  if (lkNChip >= 5) return;
  strlcpy(lkLab[lkNChip], lab, sizeof(lkLab[0]));
  strlcpy(lkVal[lkNChip], val, sizeof(lkVal[0]));
  lkNChip++;
}
static void addBarChip(const char* lab, uint64_t used, uint64_t total) {
  char p[24], v[40];
  fmtPair(p, sizeof(p), used, total);
  snprintf(v, sizeof(v), "%s  %d%%", p, pctOf(used, total));
  addChip(lab, v);
}

static void lockPrepare() {
  getClock();
  lkNChip = 0;
  char b[40];
  for (int bit = 0; bit < 7 && lkNChip < 5; bit++) {
    if (!(cfgMask & (1 << bit))) continue;
    switch (bit) {
      case 0:
        snprintf(b, sizeof(b), "%s", lamp == 1 ? "SANG" : (lamp == 0 ? "TAT" : "..."));
        addChip("DEN", b); break;
      case 1:
        if (ri.valid) addBarChip("01S ROM", ri.sketch, ri.flash); else addChip("01S ROM", "--");
        break;
      case 2:
        if (ri.valid) addBarChip("01S RAM", ri.ram > ri.heap ? ri.ram - ri.heap : 0, ri.ram); else addChip("01S RAM", "--");
        break;
      case 3:
        if (sdOk) {
          addBarChip("SD", sdUsedC, sdTotalC);
        } else addChip("SD", "khong co the");
        break;
      case 4: { uint64_t t = ESP.getHeapSize(), f = ESP.getFreeHeap(); addBarChip("CYD RAM", t - f, t); } break;
      case 5:
        if (fsOk) { uint64_t t = LittleFS.totalBytes(), u = LittleFS.usedBytes(); addBarChip("CHIP", u, t); }
        break;
      case 6:
        if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof(b), "%d dBm", (int)WiFi.RSSI()); else strcpy(b, "mat WiFi");
        addChip("WIFI", b); break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) snprintf(lkHint, sizeof(lkHint), "Giu de mo   |   http://%s", WiFi.localIP().toString().c_str());
  else strcpy(lkHint, "Giu man hinh hoac BOOT de mo");
}
static uint32_t lockSigNow() {
  uint32_t h = 5381;
  for (int i = 0; i < lkNChip; i++) {
    for (const char* p = lkLab[i]; *p; p++) h = h * 33 + (uint8_t)*p;
    for (const char* p = lkVal[i]; *p; p++) h = h * 33 + (uint8_t)*p;
  }
  for (const char* p = lkHint; *p; p++) h = h * 33 + (uint8_t)*p;
  return h ^ linkCol;
}

static inline uint16_t gradColor(int y) {
  int t = y * 255 / (SCR_H - 1);
  return RGB565(10 + t / 22, 16 + t / 14, 36 + t / 6);
}
static void dimRows(uint16_t* row, int L, const PanelR& p) {
  if (L < p.y || L >= p.y + p.h) return;
  int d = L - p.y, e = p.h - 1 - d, m = d < e ? d : e;
  static const uint8_t CI[6] = {5, 3, 2, 1, 1, 0};
  int inset = m < 6 ? CI[m] : 0;
  for (int x = p.x + inset; x < p.x + p.w - inset; x++) {
    uint16_t c = row[x];
    row[x] = ((c >> 2) & 0x39E7) + ((c >> 3) & 0x18E3);     // toi con ~37%
  }
}
static void lockOverlays(uint16_t* buf, int L0, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t* row = buf + i * SCR_W;
    int L = L0 + i;
    dimRows(row, L, PNL_A); dimRows(row, L, PNL_B); dimRows(row, L, PNL_C);
    if (L >= LK_EY && L < LK_EY + EARTH_B_D) {
      uint16_t er[EARTH_B_D];
      earthRow2D(L - LK_EY, eShift, er);
      for (int x = 0; x < EARTH_B_D; x++) if (er[x] != EARTH_KEY) row[LK_EX + x] = er[x];
    }
  }
}

// chu tren man hinh khoa; oy = lech doc (am khi da truot len)
static void lockDraw(GFX* g, int oy) {
  const int x0 = 14, ty = 22 + oy, cw = 14;
  // ve tung chu so de can deu
  char c[2] = {0, 0};
  c[0] = lkHH[0]; T(g, c, x0, ty, F7, COL_WHITE);
  c[0] = lkHH[1]; T(g, c, x0 + digW, ty, F7, COL_WHITE);
  if (lkColon) {
    int cx = x0 + 2 * digW + cw / 2 - 3;
    g->fillRect(cx, ty + 14, 6, 6, COL_WHITE);
    g->fillRect(cx, ty + 32, 6, 6, COL_WHITE);
  }
  c[0] = lkMM[0]; T(g, c, x0 + 2 * digW + cw, ty, F7, COL_WHITE);
  c[0] = lkMM[1]; T(g, c, x0 + 3 * digW + cw, ty, F7, COL_WHITE);
  T(g, lkSS, x0 + 4 * digW + cw + 8, ty + 22, F4, COL_GOLD);
  if (lkDate[0]) T(g, lkDate, 16, 78 + oy, F2, COL_WHITE);
  for (int i = 0; i < lkNChip; i++) {
    int y = 112 + i * 19 + oy;
    T(g, lkLab[i], 18, y, F2, COL_GOLD);
    T(g, lkVal[i], 302, y, F2, COL_WHITE, top_right);
  }
  T(g, lkHint, 160, 216 + oy, F2, COL_TXT, top_center);
  g->fillRoundRect(140, 236 + oy, 40, 3, 1, COL_WHITE);
  g->fillCircle(304, 8 + oy, 4, linkCol);
}

// Ghep 1 dai 16 dong (tren man hinh y = yTop..yTop+15) khi man hinh khoa da truot len `off` px
// Phan khong con la man hinh khoa = trang chinh (cung anh nen, toi 37%) + noi dung trang chinh.
static void composeBand(int yTop, int off) {
  if (!bandOk) return;
  int nLockRaw = SCR_H - off - yTop;
  int nLock = nLockRaw > BAND_H ? BAND_H : (nLockRaw < 0 ? 0 : nLockRaw);
  if (nLock < BAND_H) {
    int cnt = BAND_H - nLock;
    int mrow0 = nLockRaw < 0 ? -nLockRaw : 0;          // dong cua trang chinh ung voi dong dau tien cua phan nay
    uint16_t* r0 = rowbuf + nLock * SCR_W;
    bool okw = (mrow0 + cnt <= SCR_H) && wallReadRows(mrow0, cnt, r0);
    for (int i = 0; i < cnt * SCR_W; i++) r0[i] = okw ? dimPx(r0[i]) : COL_BG;
  }
  if (nLock > 0) {
    int L0 = yTop + off;
    if (!wallReadRows(L0, nLock, rowbuf)) {
      for (int i = 0; i < nLock; i++) {
        uint16_t gc = gradColor(L0 + i);
        uint16_t* r = rowbuf + i * SCR_W;
        for (int x = 0; x < SCR_W; x++) r[x] = gc;
      }
    }
    lockOverlays(rowbuf, L0, nLock);
  }
  band.pushImage(0, 0, SCR_W, BAND_H, (const lgfx::rgb565_t*)rowbuf);
  if (nLock > 0) {
    band.setClipRect(0, 0, SCR_W, nLock);
    lockDraw(&band, -(off + yTop));
  }
  if (nLock < BAND_H) {
    band.setClipRect(0, nLock, SCR_W, BAND_H - nLock);
    mainDraw(&band, (SCR_H - off) - yTop);
  }
  band.clearClipRect();
  band.pushSprite(0, yTop);
}
static void composeRange(int y0, int y1, int off) {      // cac dai cham vao [y0, y1)
  int a = (y0 / BAND_H) * BAND_H;
  for (int y = a; y < y1 && y < SCR_H; y += BAND_H) composeBand(y, off);
}
static void composeAll(int off) { composeRange(0, SCR_H, off); }

static void enterLock() {
  if (locked) return;
  page = PG_MAIN;
  visualSnapToLamp();
  locked = true; lkSliding = false; lkOff = 0; pgNext = SCR_H;
  if (cfgRot) { wallRotate(); wallDirty = true; }
  if (wallDirty || !wall.ok) wallOpen();
  uint32_t now = millis();
  eShift = earthShift(now, EARTH_REV_MS);
  lockPrepare();
  lkSig = lockSigNow();
  composeAll(0);
  blSet(cfgLockBr);
  lkNextEarth = now + 90; lkNextFrame = now + 1000UL / (wall.fps ? wall.fps : 8); lkNextChk = now + 500;
}

static uint32_t slideT0 = 0;
static void startSlide() {
  if (!locked || lkSliding) return;
  lkSliding = true; page = PG_MAIN; lkOff = 0; slideT0 = millis();
  blSet(100);
}
// Keo theo THOI GIAN (ease-out): may cham thi bo bot khung hinh, nhung luon xong trong SLIDE_MS
static void slideStep() {
  uint32_t el = millis() - slideT0;
  bool end = el >= SLIDE_MS;
  if (!end) {
    float inv = 1.f - (float)el / (float)SLIDE_MS;
    int off = (int)((1.f - inv * inv * inv) * SCR_H + 0.5f);
    if (off > SCR_H - 1) off = SCR_H - 1;
    if (off > lkOff) { lkOff = off; composeAll(lkOff); }
    return;
  }
  locked = false; lkSliding = false; lkOff = 0;
  wall.cur = 0;
  drawMainFull();
  lastAct = millis();
}

// Chon khung ke tiep theo thoi gian that: neu ve cham hon nhip thi nhay qua vai khung de video luon dung toc do
static void wallAdvance(uint32_t now, uint32_t& next) {
  uint32_t per = 1000UL / (wall.fps ? wall.fps : 8);
  if (per < 1) per = 1;
  uint32_t late = (uint32_t)(now - next), adv = 1;
  if (late > 1000) next = now; else adv += late / per;
  next += per * adv;
  wall.cur = (wall.cur + adv) % wall.frames;
}

static void lockTick(uint32_t now) {
  if (wallDirty) {
    wallOpen();
    lockPrepare(); composeAll(0);
    return;
  }
  if (wall.ok && wall.frames > 1) {                       // anh dong: moi khung ghep lai ca man hinh
    if (now >= lkNextFrame) {
      wallAdvance(now, lkNextFrame);                      // theo THOI GIAN THUC: ve cham thi bo bot khung, khong bi chay cham
      eShift = earthShift(now, EARTH_REV_MS);
      static uint32_t lkPrep = 0;
      if (now - lkPrep >= 250) { lkPrep = now; lockPrepare(); lkSig = lockSigNow(); }
      composeAll(0);
    }
    return;
  }
  if (now >= lkNextEarth) {                               // anh tinh: chi ghep lai vung dong ho + trai dat
    eShift = earthShift(now, EARTH_REV_MS);
    lockPrepare();
    composeRange(16, 96, 0);
    lkNextEarth = millis() + 70;
  }
  if (now >= lkNextChk) {
    lkNextChk = now + 500;
    uint32_t s = lockSigNow();
    if (s != lkSig) { lkSig = s; composeRange(96, SCR_H, 0); }
  }
}


// ================= CAM UNG XPT2046 (bit-bang) =================
static uint16_t xptXfer(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_DIN, (cmd >> i) & 1);
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(TP_CLK, LOW);
    delayMicroseconds(1);
  }
  uint16_t r = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(1);
    r = (r << 1) | (digitalRead(TP_OUT) ? 1 : 0);
    digitalWrite(TP_CLK, LOW);
    delayMicroseconds(1);
  }
  return (r >> 3) & 0x0FFF;
}
static int medianOf(int* a, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (a[j] < a[i]) { int t = a[i]; a[i] = a[j]; a[j] = t; }
  return a[n / 2];
}
static bool touchRaw(int& rx, int& ry) {
  if (digitalRead(TP_IRQ) != LOW) return false;
  int xs[5], ys[5];
  digitalWrite(TP_CS, LOW);
  xptXfer(0x90); xptXfer(0xD0);                       // doc bo 1 lan cho on dinh
  for (int i = 0; i < 5; i++) { ys[i] = xptXfer(0x90); xs[i] = xptXfer(0xD0); }
  digitalWrite(TP_CS, HIGH);
  medianOf(xs, 5); medianOf(ys, 5);                   // (sap xep tai cho) roi lay trung binh 3 gia tri giua: it nhieu hon
  rx = (xs[1] + xs[2] + xs[3]) / 3; ry = (ys[1] + ys[2] + ys[3]) / 3;
  return rx > 100 && rx < 4000 && ry > 100 && ry < 4000;
}

struct TCal { int16_t magic, swp, uL, uR, vT, vB; };
static TCal tcal = {0, 0, 200, 3900, 200, 3900};
#define TCAL_MAGIC 0x7C21

static void touchMap(int rx, int ry, int& x, int& y) {
  int u = tcal.swp ? ry : rx, v = tcal.swp ? rx : ry;
  const int M = 20;
  long dx = (long)tcal.uR - tcal.uL, dy = (long)tcal.vB - tcal.vT;
  if (dx == 0) dx = 1;
  if (dy == 0) dy = 1;
  x = M + (int)(((long)(u - tcal.uL) * (SCR_W - 2 * M)) / dx);
  y = M + (int)(((long)(v - tcal.vT) * (SCR_H - 2 * M)) / dy);
  x = x < 0 ? 0 : (x > SCR_W - 1 ? SCR_W - 1 : x);
  y = y < 0 ? 0 : (y > SCR_H - 1 ? SCR_H - 1 : y);
}

// ---- TASK CAM UNG: doc moi TP_POLL_MS, phat su kien qua hang doi cho loop() ----
enum { TEV_DOWN = 1, TEV_HOLD, TEV_UP, TEV_SWIPE_UP, TEV_SWIPE_L, TEV_SWIPE_R };
struct TEv { uint8_t type; int16_t x, y, x0, y0; uint32_t t; };
static QueueHandle_t   touchQ = nullptr;
static volatile bool   tpPause = false;      // true: task nghi (dang hieu chinh cam ung)
static volatile bool   tpBusy  = false;

// Cham: IRQ thap (nhu cu). Dang giu ma IRQ chap chon thi dung ap luc Z de giu cham them toi da TP_BRIDGE_MS.
static bool touchPoll(int& x, int& y, bool wasDown) {
  static uint32_t lastReal = 0;
  static int lastX = 0, lastY = 0;
  int rx, ry;
  if (touchRaw(rx, ry)) {
    touchMap(rx, ry, x, y);
    lastX = x; lastY = y; lastReal = millis();
    return true;
  }
  if (wasDown && millis() - lastReal < TP_BRIDGE_MS) {
    digitalWrite(TP_CS, LOW);
    xptXfer(0xB0);
    int z1 = xptXfer(0xB0);
    xptXfer(0xC0);
    int z2 = xptXfer(0xC0);
    digitalWrite(TP_CS, HIGH);
    if (z1 + 4095 - z2 > TP_ZOFF) { x = lastX; y = lastY; return true; }
  }
  return false;
}

static void touchEmit(uint8_t type, int x, int y, int x0, int y0) {
  if (!touchQ) return;
  TEv e;
  e.type = type; e.x = (int16_t)x; e.y = (int16_t)y; e.x0 = (int16_t)x0; e.y0 = (int16_t)y0; e.t = millis();
  xQueueSend(touchQ, &e, 0);
}

static void touchTask(void*) {
  bool down = false, holdSent = false, swipeSent = false;
  int x0 = 0, y0 = 0, lx = 0, ly = 0, miss = 0, outl = 0;
  uint32_t t0 = 0, lastSeen = 0;
  for (;;) {
    if (tpPause) { tpBusy = false; down = false; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    tpBusy = true;
    uint32_t now = millis();
    int x = 0, y = 0;
    if (touchPoll(x, y, down)) {
      if (down && (abs(x - lx) > 70 || abs(y - ly) > 70) && outl < 2) { outl++; x = lx; y = ly; }   // bo diem nhay vot
      else outl = 0;
      miss = 0; lastSeen = now; lastAct = now;
      if (!down) {
        down = true; holdSent = false; swipeSent = false; x0 = x; y0 = y; t0 = now;
        touchEmit(TEV_DOWN, x, y, x, y);
      }
      lx = x; ly = y;
      int dx = lx - x0, dy = ly - y0, adx = abs(dx), ady = abs(dy);
      if (!swipeSent) {
        if (ady >= SWIPE_MIN && ady * 2 > adx * 3) { swipeSent = true; if (dy < 0) touchEmit(TEV_SWIPE_UP, lx, ly, x0, y0); }
        else if (adx >= SWIPE_MIN + 10 && adx * 2 > ady * 3) { swipeSent = true; touchEmit(dx < 0 ? TEV_SWIPE_L : TEV_SWIPE_R, lx, ly, x0, y0); }
      }
      if (!holdSent && !swipeSent && now - t0 >= HOLD_MS && adx < SWIPE_MIN && ady < SWIPE_MIN) {
        holdSent = true; touchEmit(TEV_HOLD, lx, ly, x0, y0);
      }
    } else if (down) {
      if (++miss >= 3 && now - lastSeen >= 45) {          // nha tay: mat cham ca ve so lan lan ve thoi gian
        down = false; miss = 0;
        touchEmit(TEV_UP, lx, ly, x0, y0);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(TP_POLL_MS));
  }
}

static void touchCalibrate() {
  tpPause = true;                                       // dung task cam ung de doc truc tiep
  for (uint32_t tw = millis(); tpBusy && millis() - tw < 150; ) delay(2);
  const int M = 20;
  const int px[4] = {M, SCR_W - M, SCR_W - M, M};
  const int py[4] = {M, M, SCR_H - M, SCR_H - M};
  int rxs[4], rys[4];
  int rx, ry;
  for (;;) {
    for (int k = 0; k < 4; k++) {
      tft.fillScreen(COL_BG);
      T(&tft, "Cham giu vao dau + de hieu chinh", 160, 110, F2, COL_WHITE, middle_center);
      char b[12]; snprintf(b, sizeof(b), "%d / 4", k + 1);
      T(&tft, b, 160, 132, F2, COL_MUTED, middle_center);
      tft.drawLine(px[k] - 10, py[k], px[k] + 10, py[k], COL_GREEN);
      tft.drawLine(px[k], py[k] - 10, px[k], py[k] + 10, COL_GREEN);
      tft.drawCircle(px[k], py[k], 6, COL_GREEN);
      while (digitalRead(TP_IRQ) == LOW) delay(10);
      delay(250);
      for (;;) {
        if (touchRaw(rx, ry)) {
          long ax = 0, ay = 0; int n = 0;
          uint32_t t = millis();
          while (millis() - t < 350) { if (touchRaw(rx, ry)) { ax += rx; ay += ry; n++; } delay(5); }
          if (n > 0) { rxs[k] = ax / n; rys[k] = ay / n; break; }
        }
        delay(10);
      }
    }
    bool swp = abs(rys[1] - rys[0]) > abs(rxs[1] - rxs[0]);
    int u[4], v[4];
    for (int k = 0; k < 4; k++) { u[k] = swp ? rys[k] : rxs[k]; v[k] = swp ? rxs[k] : rys[k]; }
    TCal c;
    c.magic = TCAL_MAGIC; c.swp = swp ? 1 : 0;
    c.uL = (u[0] + u[3]) / 2; c.uR = (u[1] + u[2]) / 2;
    c.vT = (v[0] + v[1]) / 2; c.vB = (v[2] + v[3]) / 2;
    if (abs(c.uR - c.uL) > 800 && abs(c.vB - c.vT) > 800) {
      tcal = c;
      prefs.putBytes("tcal", &tcal, sizeof(tcal));
      Serial.printf("TCAL swp=%d u=%d..%d v=%d..%d\n", c.swp, c.uL, c.uR, c.vT, c.vB);
      break;
    }
    tft.fillScreen(COL_BG);
    T(&tft, "Hieu chinh loi, thu lai...", 160, 120, F2, COL_RED, middle_center);
    delay(1200);
  }
  while (digitalRead(TP_IRQ) == LOW) delay(10);
  if (touchQ) xQueueReset(touchQ);
  tpPause = false;
}

static void touchSetup() {
  TCal c;
  bool have = (prefs.getBytes("tcal", &c, sizeof(c)) == sizeof(c)) && c.magic == TCAL_MAGIC;
  if (have) tcal = c;
  tft.fillScreen(COL_BG);
  T(&tft, have ? "Giu tay de hieu chinh lai" : "Chua co hieu chinh cam ung", 160, 120, F2, COL_WHITE, middle_center);
  bool recal = !have;
  if (have) {
    uint32_t t0 = millis();
    int rx, ry;
    while (millis() - t0 < 1800) { if (touchRaw(rx, ry)) { recal = true; break; } delay(20); }
  }
  if (recal) touchCalibrate();
}

// ================= CAC TRANG (ve len dai ghep: nen = anh nen da lam toi, hoac mau phang) =================
static inline bool hit(const R& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void pageHeader(GFX* g, int oy, const char* title) {
  T(g, title, 12, 6 + oy, F4, COL_TXT);
  drawLinkDot(g, oy);
  drawDots(g, oy);
}

// y = hang chu. Hang chu: [nhan] [da dung / tong MB] [xx free] ........ [NN%]  (NN% cung y, o CUOI thanh)
static void barBlock(GFX* g, int oy, int y, const char* lab, uint64_t used, uint64_t total, uint16_t col) {
  char a[40], f[16], p[8], fr[32];
  fmtPair(a, sizeof(a), used, total);
  fmtNum(f, sizeof(f), total > used ? total - used : 0, total);
  snprintf(fr, sizeof(fr), "%s free", f);
  snprintf(p, sizeof(p), "%d%%", pctOf(used, total));
  y += oy;
  T(g, lab, 12, y, F2, COL_GOLD);
  T(g, a, 48, y, F2, COL_WHITE);
  T(g, fr, 196, y, F2, COL_MUTED);
  T(g, p, 308, y, F2, COL_WHITE, top_right);
  const int bx = 12, bw = 296, by = y + 20, bh = 10;
  int f2 = total ? (int)((uint64_t)(bw - 2) * used / total) : 0;
  if (f2 > bw - 2) f2 = bw - 2;
  g->drawRect(bx, by, bw, bh, COL_DIM);
  if (f2 > 0) g->fillRect(bx + 1, by + 1, f2, bh - 2, col);
  if (f2 < bw - 2) g->fillRect(bx + 1 + f2, by + 1, bw - 2 - f2, bh - 2, COL_TRACK);
}
static void barNone(GFX* g, int oy, int y, const char* lab, const char* msg) {
  y += oy;
  T(g, lab, 12, y, F2, COL_GOLD);
  T(g, msg, 48, y, F2, COL_MUTED);
  g->drawRect(12, y + 20, 296, 10, COL_DIM);
  g->fillRect(13, y + 21, 294, 8, COL_TRACK);
}
static void kv1(GFX* g, int oy, int y, const char* l, const char* v) {
  y += oy;
  T(g, l, 12, y, F2, COL_MUTED);
  T(g, v, 64, y, F2, COL_WHITE);
}
static void kv2(GFX* g, int oy, int y, const char* l1, const char* v1, const char* l2, const char* v2) {
  y += oy;
  T(g, l1, 12, y, F2, COL_MUTED);
  T(g, v1, 56, y, F2, COL_WHITE);
  T(g, l2, 168, y, F2, COL_MUTED);
  T(g, v2, 212, y, F2, COL_WHITE);
}

static void pageInfo(GFX* g, int oy) {
  RelayInfo r = ri;
  pageHeader(g, oy, "ESP-01S RELAY");
  if (!r.valid) {
    T(g, "Chua co du lieu tu relay", 160, 130 + oy, F2, COL_RED, middle_center);
    T(g, statusMsg, 160, 152 + oy, F2, COL_MUTED, middle_center);
    return;
  }
  barBlock(g, oy, 40, "ROM", (uint64_t)r.sketch, (uint64_t)r.flash, COL_BLUE);
  barBlock(g, oy, 80, "RAM", (uint64_t)(r.ram > r.heap ? r.ram - r.heap : 0), (uint64_t)r.ram, COL_GREEN);
  char a[40], b[40];
  int y = 124;
  snprintf(a, sizeof(a), "%.2f V", r.vcc / 1000.0);
  if (r.ap) strcpy(b, "AP"); else snprintf(b, sizeof(b), "%ld dBm", r.rssi);
  kv2(g, oy, y, "Vcc", a, "WiFi", b); y += 18;
  snprintf(a, sizeof(a), "%ld MHz", r.cpu);
  strcpy(b, !r.mqttcfg ? "chua cau hinh" : (r.mqtt ? "ket noi" : "mat"));
  kv2(g, oy, y, "CPU", a, "MQTT", b); y += 18;
  snprintf(a, sizeof(a), "%ldd %02ld:%02ld:%02ld", r.up / 86400, (r.up % 86400) / 3600, (r.up % 3600) / 60, r.up % 60);
  kv1(g, oy, y, "Uptime", a); y += 18;
  snprintf(a, sizeof(a), "%s%s", r.ip, r.ap ? "  (tu phat WiFi)" : "");
  kv1(g, oy, y, "IP", a); y += 18;
  if (r.sleeping) strcpy(b, "dang ngu");
  else if (r.idle == 0) strcpy(b, "khong ngu");
  else snprintf(b, sizeof(b), "ngu sau %ld:%02ld", r.left / 60, r.left % 60);
  kv2(g, oy, y, "Gio", r.synced ? r.time : "--:--:--", "Ngu", b); y += 18;
  if (r.se) snprintf(a, sizeof(a), "BAT %.5s - TAT %.5s", r.on, r.off); else strcpy(a, "tat");
  if (r.tleft > 0) { size_t l = strlen(a); snprintf(a + l, sizeof(a) - l, "   dem %ld:%02ld", r.tleft / 60, r.tleft % 60); }
  kv1(g, oy, y, "Hen gio", a);
}

static void pageCyd(GFX* g, int oy) {
  pageHeader(g, oy, "CYD ESP32");
  barBlock(g, oy, 40, "ROM", ESP.getSketchSize(), ESP.getFlashChipSize(), COL_BLUE);
  uint64_t ht = ESP.getHeapSize(), hf = ESP.getFreeHeap();
  barBlock(g, oy, 80, "RAM", ht - hf, ht, COL_GREEN);
  if (sdOk) barBlock(g, oy, 120, "SD", sdUsedC, sdTotalC, RGB565(200, 120, 255));
  else      barNone(g, oy, 120, "SD", "chua co the nho");
  if (fsOk) barBlock(g, oy, 160, "CHIP", LittleFS.usedBytes(), LittleFS.totalBytes(), COL_GOLD);
  else      barNone(g, oy, 160, "CHIP", "LittleFS loi");
  char b[64];
  if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  else strcpy(b, "chua co WiFi");
  T(g, b, 12, 202 + oy, F2, COL_WHITE);
  snprintf(b, sizeof(b), "http://%s.local   (anh nen, cai dat)", CYD_HOSTNAME);
  T(g, b, 12, 220 + oy, F2, COL_MUTED);
}

// ================= TRANG: DO THI MOI GIAY =================
#define GN 150
static int16_t gRssi[GN], gVcc[GN], gHeap[GN];      // Vcc: mV ; Heap: don vi 64 byte
static int gCnt = 0;
static bool graphDirty = false;

static void graphPush(long rssi, long vccmV, long heapB) {
  if (gCnt == GN) {
    memmove(gRssi, gRssi + 1, (GN - 1) * sizeof(int16_t));
    memmove(gVcc, gVcc + 1, (GN - 1) * sizeof(int16_t));
    memmove(gHeap, gHeap + 1, (GN - 1) * sizeof(int16_t));
    gCnt--;
  }
  gRssi[gCnt] = (int16_t)(rssi == 0 ? -100 : rssi);
  gVcc[gCnt]  = (int16_t)vccmV;
  gHeap[gCnt] = (int16_t)(heapB / 64);
  gCnt++;
  graphDirty = true;
}

static void chart(GFX* g, int oy, int y, const char* title, const int16_t* a, float scale, float minSpan, uint16_t col, int dec, const char* unit) {
  const int ox = 12, y0 = y + oy;
  T(g, title, ox, y0, F2, COL_MUTED);
  const int px = ox, py = y0 + 18, pw = 296, ph = 34;
  g->fillRect(px, py, pw, ph, COL_BG);                 // the toi sau bieu do cho de nhin tren anh nen
  g->drawRect(px, py, pw, ph, COL_DIM);
  if (gCnt > 0) {
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < gCnt; i++) { float v = a[i] * scale; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi - lo < minSpan) { float m = (hi + lo) / 2; lo = m - minSpan / 2; hi = m + minSpan / 2; }
    float step = (pw - 4) / (float)(GN - 1);
    int lx = 0, ly = 0;
    for (int i = 0; i < gCnt; i++) {
      float v = a[i] * scale;
      int x = px + pw - 2 - (int)((gCnt - 1 - i) * step);
      int yy = py + 2 + (int)((hi - v) / (hi - lo) * (ph - 5));
      if (i) g->drawLine(lx, ly, x, yy, col);
      lx = x; ly = yy;
    }
    g->fillCircle(lx, ly, 2, col);
    char b[24];
    snprintf(b, sizeof(b), "%.*f %s", dec, a[gCnt - 1] * scale, unit);
    T(g, b, ox + 296, y0, F2, col, top_right);
    snprintf(b, sizeof(b), "%.*f", dec, hi);
    T(g, b, px + 4, py + 3, F0, COL_MUTED);
    snprintf(b, sizeof(b), "%.*f", dec, lo);
    T(g, b, px + 4, py + ph - 11, F0, COL_MUTED);
  } else {
    T(g, "dang thu thap...", px + pw / 2, py + ph / 2, F2, COL_MUTED, middle_center);
  }
}
static void pageGraph(GFX* g, int oy) {
  pageHeader(g, oy, "DO THI ESP-01S");
  chart(g, oy, 36,  "WiFi RSSI", gRssi, 1.f, 6.f, COL_BLUE, 0, "dBm");
  chart(g, oy, 94,  "Vcc nguon", gVcc, 0.001f, 0.06f, COL_GOLD, 2, "V");
  chart(g, oy, 152, "RAM trong", gHeap, 64.f / 1024.f, 2.f, COL_GREEN, 1, "KB");
  T(g, "ESP-01S - 1 diem / giay - 150 giay gan nhat", 160, 216 + oy, F2, COL_MUTED, top_center);
}

// ================= TRANG: CAI DAT =================
static const R B_IDLM = {170, 44, 44, 32};
static const R B_IDLP = {264, 44, 44, 32};
static const R B_LOCK = {12, 100, 144, 36};
static const R B_TCAL = {164, 100, 144, 36};
static const R B_VID  = {12, 144, 296, 30};

static void btn(GFX* g, int oy, const R& r, const char* s) {
  g->fillRoundRect(r.x, r.y + oy, r.w, r.h, 8, COL_BTN);
  g->drawRoundRect(r.x, r.y + oy, r.w, r.h, 8, COL_BTNLN);
  T(g, s, r.x + r.w / 2, r.y + r.h / 2 + oy, F2, COL_WHITE, middle_center);
}
static void idleText(char* b, size_t n, int s) {
  if (s == 0) snprintf(b, n, "Khong");
  else if (s < 60) snprintf(b, n, "%d giay", s);
  else snprintf(b, n, "%d phut", s / 60);
}
static void pageSet(GFX* g, int oy) {
  pageHeader(g, oy, "CAI DAT");
  T(g, "Cho roi khoa sau", 12, 52 + oy, F2, COL_WHITE);
  btn(g, oy, B_IDLM, "-");
  btn(g, oy, B_IDLP, "+");
  char b[48]; idleText(b, sizeof(b), cfgIdleSec);
  T(g, b, 239, 60 + oy, F2, COL_WHITE, middle_center);
  btn(g, oy, B_LOCK, "Khoa ngay");
  btn(g, oy, B_TCAL, "Chinh cam ung");
  snprintf(b, sizeof(b), "Nen dong moi man hinh: %s", cfgVidAll ? "BAT" : "tat");
  btn(g, oy, B_VID, b);
  T(g, "Giu man hinh: doi trang", 12, 180 + oy, F2, COL_MUTED);
  T(g, "Man den: cham 1 lan = bat/tat den", 12, 198 + oy, F2, COL_MUTED);
  if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof(b), "Web: http://%s", WiFi.localIP().toString().c_str());
  else strcpy(b, "Chua co WiFi");
  T(g, b, 12, 220 + oy, F2, COL_MUTED);
}

// ================= TRANG: ANH NEN (chon anh / video da luu trong the SD hoac chip) =================
#define WL_MAX 24
#define WROWS  4
struct WlItem { char key[40]; char label[22]; uint16_t frames, fps; bool ok; };
static WlItem wl[WL_MAX];
static int wlN = 0, wlTop = 0;
static volatile bool wlStale = false;        // web vua them / xoa anh -> quet lai danh sach

static void wlScan() {
  wlStale = false;
  wlN = 0;
  FS_LOCK();
  if (fsOk) {
    fs::File f = LittleFS.open("/bg.cya", "r");
    uint16_t fr = 0, fp = 8;
    if (f) {
      if (cyaInfo(f, fr, fp)) {
        strcpy(wl[wlN].key, "flash"); strcpy(wl[wlN].label, "Chip (co ban)");
        wl[wlN].frames = fr; wl[wlN].fps = fp; wl[wlN].ok = true; wlN++;
      }
      f.close();
    }
  }
  if (sdOk) {
    fs::File d = SD.open("/wall");
    if (d) {
      for (;;) {
        fs::File e = d.openNextFile();
        if (!e) break;
        String nm = e.name();
        int sl = nm.lastIndexOf('/');
        if (sl >= 0) nm = nm.substring(sl + 1);
        if (!e.isDirectory() && nm.endsWith(".cya") && wlN < WL_MAX) {
          nm = nm.substring(0, nm.length() - 4);
          uint16_t fr = 0, fp = 8;
          bool ok = cyaInfo(e, fr, fp);
          snprintf(wl[wlN].key, sizeof(wl[wlN].key), "sd:%s", nm.c_str());
          strlcpy(wl[wlN].label, nm.c_str(), sizeof(wl[wlN].label));
          wl[wlN].frames = fr; wl[wlN].fps = fp; wl[wlN].ok = ok; wlN++;
        }
        e.close();
      }
      d.close();
    }
  }
  FS_UNLOCK();
  int s0 = (wlN > 0 && !strcmp(wl[0].key, "flash")) ? 1 : 0;     // xep theo ten (anh trong chip luon dau)
  for (int i = s0 + 1; i < wlN; i++)
    for (int j = i; j > s0 && strcmp(wl[j].label, wl[j - 1].label) < 0; j--) { WlItem t = wl[j]; wl[j] = wl[j - 1]; wl[j - 1] = t; }
  if (wlTop >= wlN) wlTop = 0;
}

static R wallRow(int i) { R r = {12, 36 + i * 38, 296, 34}; return r; }
static const R B_WPREV = {12, 196, 56, 36};
static const R B_WNEXT = {72, 196, 56, 36};
static const R B_WROT  = {132, 196, 112, 36};
static const R B_WLCK  = {248, 196, 60, 36};

static void pageWall(GFX* g, int oy) {
  pageHeader(g, oy, "ANH NEN");
  char b[48];
  if (wlN == 0) {
    T(g, "Chua co anh nen", 160, 90 + oy, F2, COL_WHITE, middle_center);
    T(g, "Them anh / video tu web:", 160, 114 + oy, F2, COL_MUTED, middle_center);
    if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
    else strcpy(b, "chua co WiFi");
    T(g, b, 160, 134 + oy, F2, COL_WHITE, middle_center);
  } else {
    int last = wlTop + WROWS < wlN ? wlTop + WROWS : wlN;
    snprintf(b, sizeof(b), "%d-%d / %d", wlTop + 1, last, wlN);
    T(g, b, 150, 10 + oy, F2, COL_MUTED);
    for (int i = 0; i < WROWS; i++) {
      int k = wlTop + i;
      if (k >= wlN) break;
      R r = wallRow(i);
      bool act = !strcmp(wl[k].key, cfgWp);
      g->fillRoundRect(r.x, r.y + oy, r.w, r.h, 8, COL_BTN);
      g->drawRoundRect(r.x, r.y + oy, r.w, r.h, 8, act ? COL_GOLD : COL_BTNLN);
      T(g, wl[k].label, r.x + 12, r.y + r.h / 2 + oy, F2, act ? COL_GOLD : COL_WHITE, middle_left);
      if (!wl[k].ok) strcpy(b, "file loi");
      else if (wl[k].frames > 1) {
        unsigned sec = wl[k].frames / (wl[k].fps ? wl[k].fps : 1);
        if (sec < 60) snprintf(b, sizeof(b), "%us  %u fps", sec, (unsigned)wl[k].fps);
        else snprintf(b, sizeof(b), "%um%02us  %u fps", sec / 60, sec % 60, (unsigned)wl[k].fps);
      }
      else strcpy(b, "anh tinh");
      T(g, b, r.x + r.w - 10, r.y + r.h / 2 + oy, F2, COL_MUTED, middle_right);
    }
  }
  btn(g, oy, B_WPREV, "<");
  btn(g, oy, B_WNEXT, ">");
  snprintf(b, sizeof(b), "Xoay vong: %s", cfgRot ? "BAT" : "tat");
  btn(g, oy, B_WROT, b);
  btn(g, oy, B_WLCK, "Xem");
}

// ================= DIEU HUONG TRANG + GHEP NEN =================
static bool pageDirty = false;

static void pageContent(GFX* g, int oy) {
  switch (page) {
    case PG_MAIN:  mainDraw(g, oy);  break;
    case PG_INFO:  pageInfo(g, oy);  break;
    case PG_GRAPH: pageGraph(g, oy); break;
    case PG_CYD:   pageCyd(g, oy);   break;
    case PG_WALL:  pageWall(g, oy);  break;
    case PG_SET:   pageSet(g, oy);   break;
  }
}

// 1 dai 16 dong: anh nen (toi 37%) hoac mau phang, roi ve noi dung trang len tren
static void pageBand(int yTop) {
  bool w = wallReadRows(yTop, BAND_H, rowbuf);
  for (int i = 0; i < SCR_W * BAND_H; i++) rowbuf[i] = w ? dimPx(rowbuf[i]) : COL_BG;
  band.pushImage(0, 0, SCR_W, BAND_H, (const lgfx::rgb565_t*)rowbuf);
  pageContent(&band, -yTop);
  band.pushSprite(0, yTop);
}
static void pageRange(int y0, int y1) {
  if (locked) return;
  if (y0 < 0) y0 = 0;
  if (y1 > SCR_H) y1 = SCR_H;
  if (y1 <= y0) return;
  if (!bandOk) {                                       // khong co dai ghep: ve thang, nen phang
    tft.setClipRect(0, y0, SCR_W, y1 - y0);
    tft.fillRect(0, y0, SCR_W, y1 - y0, COL_BG);
    pageContent(&tft, 0);
    tft.clearClipRect();
    return;
  }
  for (int y = (y0 / BAND_H) * BAND_H; y < y1 && y < SCR_H; y += BAND_H) pageBand(y);
}
static void pageRender() { pageRange(0, SCR_H); }
// Ve lai trang theo tung dai nho (~18 ms moi luot), xen ke voi doc cam ung: khong con dung hinh ~0.3 s
static void pageRenderAsync() {
  if (!bandOk) { pageRender(); return; }
  pgNext = 0;
}
static void pageAsyncStep() {
  if (pgNext >= SCR_H) return;
  if (locked || page == PG_MAIN) { pgNext = SCR_H; return; }
  uint32_t t = millis();
  do { pageBand(pgNext); pgNext += BAND_H; } while (pgNext < SCR_H && millis() - t < 18);
}

static uint32_t vidNext = 0;
// Nen dong o cac trang thuong: moi nhip ve lai ca trang voi khung nen ke tiep (bo qua khi dang chay hieu ung nut / ve lai trang)
static void vidStep(uint32_t now) {
  if (!cfgVidAll || locked || !wall.ok || wall.frames < 2 || !bandOk) return;
  if (animOn || pgNext < SCR_H) { vidNext = now; return; }
  if ((int32_t)(now - vidNext) < 0) return;
  wallAdvance(now, vidNext);
  pageRender();
}

static void gotoPage(int p) {
  if (p < 0 || p >= PG_COUNT || p == page) return;
  page = p;
  pgNext = SCR_H;
  pageDirty = false;
  graphDirty = false;
  if (p == PG_MAIN) visualSnapToLamp();
  if (p == PG_WALL) wlScan();
  pageRender();
}

static void pageTick(uint32_t now) {
  static uint32_t next = 0;
  if (page == PG_MAIN) return;
  if (page == PG_WALL && wlStale) { wlScan(); pageDirty = true; }      // web vua them / xoa anh
  bool due = pageDirty || graphDirty || (page != PG_SET && page != PG_WALL && now >= next);
  if (!due) return;
  next = now + 1000;
  pageDirty = false;
  graphDirty = false;
  pageRenderAsync();
}

// ================= GUI LENH DEN =================
static bool sendLamp(int want) {
  if (cmdPending) return false;
  lampBeforeCmd = lamp;
  lamp = want;
  applyLampVisual();
  drawStatus("Dang gui...", COL_YELLOW);
  cmdPending = true;
  cmdWant = want;
  return true;
}
static void pressButton() { sendLamp(lamp == 1 ? 0 : 1); }

// ================= CHAM: CHAM 1 LAN = THAO TAC / GIU = DOI TRANG (hoac THOAT MAN CHO) =================
static inline bool inPillArea(int x, int y) {
  return x >= PILL_X - HIT_MARGIN && x < PILL_X + PILL_W + HIT_MARGIN && y >= PILL_Y - HIT_MARGIN && y < PILL_Y + PILL_H + HIT_MARGIN;
}

// Tra ve true neu cham trung 1 nut dieu khien (khong tinh la "cho trong")
static bool onTap(int x, int y) {
  if (page == PG_WALL) {
    for (int i = 0; i < WROWS; i++) {
      int k = wlTop + i;
      if (k < wlN && hit(wallRow(i), x, y)) {                // chon anh nen nay (wallDirty -> loop() mo lai + ve lai trang)
        strlcpy(cfgWp, wl[k].key, sizeof(cfgWp));
        prefs.putString("wp", cfgWp);
        wallDetach();
        return true;
      }
    }
    if (hit(B_WPREV, x, y)) { wlTop = wlTop - WROWS < 0 ? 0 : wlTop - WROWS; pageRender(); return true; }
    if (hit(B_WNEXT, x, y)) { if (wlTop + WROWS < wlN) wlTop += WROWS; pageRender(); return true; }
    if (hit(B_WROT, x, y))  { cfgRot = !cfgRot; prefs.putBool("rot", cfgRot); pageRender(); return true; }
    if (hit(B_WLCK, x, y))  { lockReq = true; return true; }
  }
  if (page == PG_SET) {
    if (hit(B_IDLM, x, y) || hit(B_IDLP, x, y)) {
      int idx = 0;
      for (int i = 0; i < N_IDLE; i++) if (IDLE_OPTS[i] <= cfgIdleSec) idx = i;
      idx += hit(B_IDLP, x, y) ? 1 : -1;
      if (idx < 0) idx = 0;
      if (idx >= N_IDLE) idx = N_IDLE - 1;
      cfgIdleSec = IDLE_OPTS[idx];
      prefs.putUShort("idle2", cfgIdleSec);
      pageRender();
      return true;
    }
    if (hit(B_LOCK, x, y)) { lockReq = true; return true; }
    if (hit(B_TCAL, x, y)) { touchCalibrate(); pageRender(); return true; }
    if (hit(B_VID, x, y))  { cfgVidAll = !cfgVidAll; prefs.putBool("vall", cfgVidAll); pageRender(); return true; }
  }
  return false;
}

// Cach dieu khien:
//   - GIU man hinh (~HOLD_MS)  : doi sang trang ke tiep  (dang o man hinh cho thi la mo khoa)
//   - CHAM 1 LAN o man den     : bat / tat den
//   - CHAM 1 LAN o trang khac  : bam nut tren trang do (vd Cai dat)
//   - VUOT TRAI/PHAI           : van doi trang nhu cu
// Bat/tat den chay luc NHA TAY (TEV_UP) de phan biet voi giu man hinh.
static void touchLoop(uint32_t) {
  static bool ignore = false, holdArm = false;
  static uint32_t lastPress = 0;
  if (!touchQ) return;
  TEv e;
  while (xQueueReceive(touchQ, &e, 0) == pdTRUE) {
    switch (e.type) {
      case TEV_DOWN:
        holdArm = locked;                                   // chi tinh \"giu de mo khoa\" neu cham bat dau khi dang o man hinh cho
        ignore  = locked;                                   // lan cham mo dau o man hinh cho thi khong thao tac gi khac
        break;
      case TEV_HOLD:
        if (locked) {                                       // giu man hinh cho -> mo khoa
          if (holdArm && !lkSliding) startSlide();
        } else if (!ignore) {                               // giu man hinh -> trang ke tiep
          ignore = true;
          gotoPage((page + 1) % PG_COUNT);
        }
        break;
      case TEV_SWIPE_UP:                                    // vuot len -> mo khoa
        if (locked && !lkSliding) startSlide();
        break;
      case TEV_SWIPE_L:
      case TEV_SWIPE_R:                                     // vuot ngang -> doi trang
        if (!locked && !ignore) {
          ignore = true;
          gotoPage(e.type == TEV_SWIPE_L ? (page + 1) % PG_COUNT : (page + PG_COUNT - 1) % PG_COUNT);
        }
        break;
      case TEV_UP:
        if (ignore) break;                                  // da la giu / vuot / mo khoa
        if (abs(e.x - e.x0) >= 14 || abs(e.y - e.y0) >= 14) break;
        if (onTap(e.x0, e.y0)) break;                       // nut tren trang Cai dat
        if (page == PG_MAIN && (MAIN_TAP_ANYWHERE || inPillArea(e.x0, e.y0)) && e.t - lastPress > PRESS_GAP_MS) {
          lastPress = e.t;                                  // man den: cham 1 lan = bat/tat den
          pressButton();
        }
        break;
    }
  }
}

static void bootLoop(uint32_t now) {
  static bool dn = false, fired = false;
  static uint32_t t0 = 0;
  bool d = (digitalRead(BOOT_BTN_PIN) == LOW);
  if (d && !dn) { dn = true; fired = false; t0 = now; lastAct = now; }
  else if (!d && dn) dn = false;
  if (dn && !fired && now - t0 >= 600) {
    fired = true; lastAct = now;
    if (locked) startSlide(); else lockReq = true;
  }
}

// ================= RELAY (HTTP) - CHI TASK MANG GOI =================
static const char* jfind(const char* b, const char* key) {
  char k[24];
  snprintf(k, sizeof(k), "\"%s\":", key);
  const char* p = strstr(b, k);
  return p ? p + strlen(k) : nullptr;
}
static long jl(const char* b, const char* k, long d = 0) { const char* p = jfind(b, k); return p ? atol(p) : d; }
static bool jb(const char* b, const char* k) { const char* p = jfind(b, k); return p && strncmp(p, "true", 4) == 0; }
static void js(const char* b, const char* k, char* out, size_t n) {
  const char* p = jfind(b, k);
  out[0] = 0;
  if (!p || *p != '"') return;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
  out[i] = 0;
}
static bool parseRelay(const char* b) {
  if (!jfind(b, "lamp")) return false;
  RelayInfo t;
  memset(&t, 0, sizeof(t));
  t.valid = true;
  t.lamp = (int)jl(b, "lamp");
  t.heap = jl(b, "heap"); t.ram = jl(b, "ram", 81920); t.sketch = jl(b, "sketch"); t.flash = jl(b, "flash", 1048576);
  t.vcc = jl(b, "vcc"); t.rssi = jl(b, "rssi"); t.up = jl(b, "up"); t.cpu = jl(b, "cpu");
  t.idle = jl(b, "idle"); t.left = jl(b, "left"); t.tleft = jl(b, "tleft"); t.tset = jl(b, "tset");
  t.se = jb(b, "se"); t.sleeping = jb(b, "sleeping"); t.synced = jb(b, "synced"); t.ap = jb(b, "ap");
  t.mqttcfg = jb(b, "mqttcfg"); t.mqtt = jb(b, "mqtt");
  js(b, "ip", t.ip, sizeof(t.ip)); js(b, "time", t.time, sizeof(t.time));
  js(b, "on", t.on, sizeof(t.on)); js(b, "off", t.off, sizeof(t.off));
  ri = t;
  riSeq = riSeq + 1;
  return true;
}

bool resolveRelay() {
  if ((uint32_t)relayIP != 0) return true;
  IPAddress ip = MDNS.queryHost(RELAY_HOST, 2000);
  if ((uint32_t)ip == 0) {
    IPAddress ip2;
    if (WiFi.hostByName("esp-relay.local", ip2) == 1) ip = ip2;
  }
  if ((uint32_t)ip == 0 && RELAY_FALLBACK_IP[0]) ip.fromString(RELAY_FALLBACK_IP);
  if ((uint32_t)ip == 0) return false;
  relayIP = ip;
  Serial.print("Relay IP: ");
  Serial.println(relayIP);
  return true;
}

bool relayCall(const char* path, int& lampOut, uint16_t timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!resolveRelay()) return false;
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + relayIP.toString() + path;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return false;
  bool ok = false;
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    if (parseRelay(body.c_str())) { lampOut = ri.lamp ? 1 : 0; ok = true; }
  }
  http.end();
  if (!ok) relayIP = IPAddress((uint32_t)0);
  return ok;
}

static void postEvt(int type, int lampVal) {
  while (evtType != 0) vTaskDelay(pdMS_TO_TICKS(10));
  evtLamp = lampVal;
  evtType = type;
}

// Task mang (core 0): tim relay, gui lenh, hoi /status MOI GIAY, noi lai WiFi
static void netTask(void*) {
  uint32_t lastPoll = 0, lastWifi = 0;
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastWifi > 10000) { lastWifi = now; WiFi.reconnect(); }
    }
    int want = cmdWant;
    if (want >= 0) {
      cmdWant = -1;
      int res = 0;
      bool ok = relayCall(want ? "/on" : "/off", res, 3000);
      if (!ok) ok = relayCall(want ? "/on" : "/off", res, 3000);
      netFail = ok ? 0 : netFail + 1;
      postEvt(ok ? 1 : 2, res);
      lastPoll = millis();
    } else {
      uint32_t period = (netFail >= 2) ? 10000 : 1000;
      if (now - lastPoll >= period) {
        lastPoll = now;
        int res = 0;
        bool ok = relayCall("/status", res, 1500);
        netFail = ok ? 0 : netFail + 1;
        postEvt(ok ? 3 : 4, res);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= SU KIEN TU TASK MANG (loop) =================
static void handleEvent() {
  int e = evtType;
  if (e == 0) return;
  int l = evtLamp;
  evtType = 0;
  switch (e) {
    case 1:
      cmdPending = false;
      if (l != lamp) { lamp = l; applyLampVisual(); }
      drawStatus("Da gui lenh", COL_GREEN);
      drawLink(COL_GREEN);
      break;
    case 2:
      cmdPending = false;
      lamp = lampBeforeCmd;
      applyLampVisual();
      drawStatus("LOI: khong ket noi relay", COL_RED);
      drawLink(COL_RED);
      break;
    case 3:
      if (ri.valid) graphPush(ri.ap ? 0 : ri.rssi, ri.vcc, ri.heap);
      pageDirty = true;
      drawLink(COL_GREEN);
      if (cmdPending) break;
      if (l != lamp) { lamp = l; applyLampVisual(); }
      {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s.local  %s", RELAY_HOST, relayIP.toString().c_str());
        drawStatus(buf, COL_GREY);
      }
      break;
    case 4:
      if (netFail >= 2) ri.valid = false;
      pageDirty = true;
      if (cmdPending) break;
      if (netFail >= 2) {
        if (WiFi.status() != WL_CONNECTED) { drawLink(COL_YELLOW); drawStatus("Mat WiFi - dang noi lai...", COL_YELLOW); }
        else { drawLink(COL_RED); drawStatus("Relay offline", COL_RED); }
      }
      break;
  }
}


// ================= WEB RIENG CUA CYD =================
static const char PAGE[] PROGMEM = R"CYD(<!doctype html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD · Màn hình chờ</title>
<style>
:root{--bg:#0c1019;--sf:#131a28;--ln:#26324a;--tx:#e8eef7;--mu:#8f9db3;--gold:#ffd43b;--ok:#34d399;--bad:#f87171;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.5 ui-rounded,"Segoe UI",system-ui,sans-serif}
main{max-width:660px;margin:0 auto;padding:20px 16px 60px}
h1{font-size:22px;margin:0 0 2px}
h2{font-size:16px;margin:28px 0 10px;padding-bottom:6px;border-bottom:1px solid var(--ln)}
.mu{color:var(--mu);font-size:13px}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:8px 0}
.row>label{display:flex;flex-direction:column;gap:3px;font-size:13px;color:var(--mu)}
select,input[type=text]{background:var(--sf);color:var(--tx);border:1px solid var(--ln);border-radius:8px;padding:8px 10px;font:inherit}
input[type=text]{flex:1;min-width:140px}
button{background:var(--sf);color:var(--tx);border:1px solid var(--ln);border-radius:8px;padding:9px 14px;font:inherit;cursor:pointer}
button.p{background:var(--gold);color:#17130a;border-color:var(--gold);font-weight:600}
button.d{color:var(--bad)}
button:disabled{opacity:.45;cursor:default}
.prev{display:flex;gap:14px;flex-wrap:wrap}
canvas{width:320px;max-width:100%;aspect-ratio:4/3;background:#000;border:6px solid #1b2438;border-radius:10px}
.ctl{flex:1;min-width:230px}
.bar{height:6px;background:var(--sf);border-radius:3px;overflow:hidden;margin:10px 0 4px}
.bar i{display:block;height:100%;width:0;background:var(--gold)}
.it{display:flex;gap:10px;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid var(--ln)}
.it b{font-weight:600}
.tag{font-size:12px;color:#17130a;background:var(--ok);border-radius:99px;padding:1px 8px;margin-left:6px}
.chips{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:4px 12px}
.chips label{display:flex;gap:8px;align-items:center}
#st{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px;margin-top:14px}
#st div{background:var(--sf);border:1px solid var(--ln);border-radius:8px;padding:8px 10px}
#st span{display:block;color:var(--mu);font-size:12px}
</style></head><body><main>
<h1>Màn hình chờ CYD</h1>
<div class="mu" id="sub">Đang kết nối…</div>
<div id="st"></div>

<h2>Ảnh nền</h2>
<div class="prev"><canvas id="pv" width="320" height="240"></canvas>
<div class="ctl">
<div class="row"><input type="file" id="fi" accept="image/*,video/*,.mp4,.webm,.mov,.mkv,.m4v,.avi,.gif,.webp,.avif,.apng" multiple></div>
<div class="mu">Trình duyệt giải mã MP4/WebM/GIF/WebP/AVIF (H.264/VP9/AV1 nếu máy hỗ trợ), nén JPEG 160×120 (CYJ1) rồi ghi thẳng thẻ SD. CYD chỉ đọc file sẵn — không giải mã video. ~10–15 khung/giây.</div>
<div class="row">
<label>Khung hình<select id="fit"><option value="cover">Phủ đầy</option><option value="contain">Vừa khung</option></select></label>
<label>Khung/giây (video)<select id="fps"><option>6</option><option>8</option><option>10</option><option selected>12</option><option>15</option></select></label>
<label>Định dạng<select id="ql"><option value="jpg" selected>JPEG 160×120 → thẻ SD (khuyên dùng, CYD chỉ đọc)</option><option value="half">Thô 160×120 (nặng ~8×)</option><option value="full">Nét 320×240 (ảnh tĩnh)</option></select></label>
</div>
<div class="mu" id="fmsg">Chưa chọn ảnh.</div>
</div></div>
<div class="row"><button class="p" id="bF" disabled>Lưu ảnh tĩnh vào chip</button></div>
<div class="row"><input type="text" id="nm" placeholder="tên ảnh, ví dụ: bien-xanh" maxlength="24"><button id="bS" disabled>Nén &amp; lưu thẻ SD (CYD chỉ đọc)</button></div>
<div class="row"><label style="flex-direction:row;gap:8px;align-items:center"><input type="checkbox" id="ua" checked> Dùng làm ảnh nền ngay sau khi lưu</label></div>
<div class="bar"><i id="pg"></i></div><div class="mu" id="pm"></div>

<h2>Đã lưu</h2>
<div id="list" class="mu">…</div>

<h2>Màn hình chờ</h2>
<div class="row">
<label>Vào màn hình chờ sau<select id="idle"><option value="0">Không bao giờ</option><option value="30">30 giây</option><option value="60">1 phút</option><option value="120">2 phút</option><option value="300">5 phút</option><option value="600">10 phút</option><option value="1800">30 phút</option></select></label>
<label>Độ sáng khi chờ<select id="lbr"><option>10</option><option>20</option><option>35</option><option>50</option><option>75</option><option>100</option></select></label>
</div>
<div class="row"><label style="flex-direction:row;gap:8px;align-items:center"><input type="checkbox" id="rot"> Xoay vòng ảnh trong thẻ nhớ mỗi lần vào màn hình chờ</label></div>
<div class="row"><label style="flex-direction:row;gap:8px;align-items:center"><input type="checkbox" id="vall"> Nền động (video/GIF) chạy ở mọi màn hình, không chỉ màn hình chờ</label></div>
<div class="mu" style="margin-top:10px">Thông số hiện trên màn hình chờ (tối đa 5 mục, lấy theo thứ tự)</div>
<div class="chips" id="chips"></div>
<div class="row"><button class="p" id="bSet">Lưu cài đặt</button><button id="bLock">Khóa màn hình ngay</button></div>

</main>
<script>
const $=id=>document.getElementById(id),W=320,H=240;
const api=async p=>(await fetch(p)).json();
const kb=k=>k>=1048576?(k/1048576).toFixed(2)+' GB':(k/1024).toFixed(k<10240?2:0)+' MB';
const CH=['Đèn','ROM ESP-01S','RAM ESP-01S','Thẻ SD','RAM CYD','Bộ nhớ chip','WiFi'];
let frames=[],info=null,busy=false;
$('chips').innerHTML=CH.map((n,i)=>`<label><input type="checkbox" class="cb" data-b="${i}"> ${n}</label>`).join('');

async function refresh(){
 try{info=await api('/api/info')}catch(e){$('sub').textContent='Mất kết nối với CYD';return}
 const d=info;
 $('sub').textContent='http://'+d.ip+'  ·  '+d.host;
 $('st').innerHTML=[
  ['WiFi',d.rssi+' dBm'],['RAM CYD',kb((d.heapTotal-d.heap)/1024)+' / '+kb(d.heapTotal/1024)],
  ['Thẻ SD',d.sdOk?kb(d.sdUsed)+' / '+kb(d.sdTotal):'chưa có thẻ'],
  ['Bộ nhớ chip',kb(d.fsUsed)+' / '+kb(d.fsTotal)],['Đèn',d.lamp==1?'sáng':d.lamp==0?'tắt':'?']
 ].map(x=>`<div><span>${x[0]}</span>${x[1]}</div>`).join('');
 const act=d.active;let h='';
 const row=(t,sub,on,u,del)=>`<div class="it"><div><b>${t}</b>${on?'<span class="tag">đang dùng</span>':''}<div class="mu">${sub}</div></div><div class="row" style="margin:0">${on?'':`<button onclick="${u}">Dùng</button>`}<button class="d" onclick="${del}">Xóa</button></div></div>`;
 if(d.flash.ok)h+=row('Ảnh cơ bản (chip)',d.flash.f+' khung · '+kb(d.flash.s),act==='flash',"use('flash','')","del('flash','')");
 d.files.forEach(f=>{h+=row(f.n,(f.ok?(f.f>1?fmtT(f.f/f.fps)+' · '+f.f+' khung · '+f.fps+' fps'+(f.k==2?' · JPEG 160×120':f.k==1?' · 160×120':''):'ảnh tĩnh'):'file lỗi')+' · '+kb(f.s),act==='sd:'+f.n,`use('sd','${f.n}')`,`del('sd','${f.n}')`)});
 $('list').innerHTML=h||'Chưa có ảnh nào. Gradient mặc định đang được dùng.';
 if(!busy){
  $('idle').value=d.idle;$('lbr').value=d.lbr;$('rot').checked=!!d.rot;$('vall').checked=!!d.vall;
  document.querySelectorAll('.cb').forEach(c=>c.checked=!!(d.mask&(1<<c.dataset.b)));
 }
}
async function use(s,n){await api('/api/use?src='+s+'&name='+encodeURIComponent(n));refresh()}
async function del(s,n){if(confirm('Xóa ảnh này?')){await api('/api/del?src='+s+'&name='+encodeURIComponent(n));refresh()}}

// ----- trình duyệt giải mã mọi định dạng → CYJ1/CYA; CYD chỉ đọc file sẵn trên SD -----
const cv=$('pv'),cx=cv.getContext('2d',{willReadFrequently:true});
let SRC=null,FILES=null;const MAXF=65000;
const fmtT=t=>t<60?t.toFixed(t<10?1:0)+' giây':Math.floor(t/60)+' phút '+Math.round(t%60)+' giây';
function fitTo(c,w,h,src){
 const m=$('fit').value;
 const sw=src.videoWidth||src.displayWidth||src.codedWidth||src.width||1;
 const sh=src.videoHeight||src.displayHeight||src.codedHeight||src.height||1;
 c.fillStyle='#000';c.fillRect(0,0,w,h);
 const k=m==='cover'?Math.max(w/sw,h/sh):Math.min(w/sw,h/sh),dw=sw*k,dh=sh*k;
 c.drawImage(src,(w-dw)/2,(h-dh)/2,dw,dh);
}
const once=(el,ev)=>new Promise(r=>el.addEventListener(ev,r,{once:true}));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function guessType(f){
 let t=(f.type||'').toLowerCase(); if(t) return t;
 const n=(f.name||'').toLowerCase();
 if(/\.gif$/.test(n))return 'image/gif';
 if(/\.webp$/.test(n))return 'image/webp';
 if(/\.apng$|\.png$/.test(n))return 'image/png';
 if(/\.jpe?g$/.test(n))return 'image/jpeg';
 if(/\.avif$/.test(n))return 'image/avif';
 if(/\.mp4$|\.m4v$/.test(n))return 'video/mp4';
 if(/\.webm$/.test(n))return 'video/webm';
 if(/\.mov$/.test(n))return 'video/quicktime';
 if(/\.mkv$/.test(n))return 'video/x-matroska';
 return '';
}
function isVideoType(t){return t.startsWith('video/')||t==='application/mp4';}
function isAnimImage(t){return t==='image/gif'||t==='image/webp'||t==='image/apng'||t==='image/avif';}

async function loadViaImageDecoder(file,type,fpsUser){
 if(!('ImageDecoder' in window)) throw new Error('no ImageDecoder');
 const dec=new ImageDecoder({data:await file.arrayBuffer(),type:type||file.type||'image/gif'});
 await dec.tracks.ready;
 const track=dec.tracks.selectedTrack; if(!track) throw new Error('no track');
 const n=Math.min(MAXF,track.frameCount|0); if(n<=0) throw new Error('0 frames');
 const sample=Math.min(n,24); let durUs=0,got=0;
 for(let i=0;i<sample;i++){try{const r=await dec.decode({frameIndex:i});durUs+=(r.image.duration||0);if(r.image.duration)got++;r.image.close()}catch(e){break}}
 let fps=fpsUser;
 if(got>0&&durUs>0) fps=Math.max(1,Math.min(20,Math.round(1e6/(durUs/got))));
 else if(n>1) fps=Math.max(1,Math.min(20,fpsUser||10)); else fps=1;
 return {n,fps,note:(type||'image').replace('image/','')+' '+n+' khung',
  get:async i=>{const r=await dec.decode({frameIndex:Math.min(i,n-1)});const b=await createImageBitmap(r.image);r.image.close();return b},
  rel:b=>{if(b&&b.close)b.close()}, done:()=>{try{dec.close()}catch(e){}}};
}

async function loadViaVideo(file,fpsUser){
 const url=URL.createObjectURL(file);
 const v=document.createElement('video'); v.muted=true;v.playsInline=true;v.preload='auto';v.src=url;
 await Promise.race([once(v,'loadedmetadata'),sleep(12000).then(()=>{throw new Error('Timeout metadata video')})]);
 if(!isFinite(v.duration)||v.duration<=0){try{await v.play();v.pause()}catch(e){} await sleep(200)}
 if(!isFinite(v.duration)||v.duration<=0){URL.revokeObjectURL(url);throw new Error('Không đọc được thời lượng (codec không hỗ trợ?)')}
 const fps=Math.max(1,Math.min(20,fpsUser||12));
 const n=Math.max(1,Math.min(MAXF,Math.floor(v.duration*fps)));
 try{const p=once(v,'seeked');v.currentTime=0;await Promise.race([p,sleep(3000)])}catch(e){}
 return {n,fps,note:'video '+fmtT(v.duration)+(v.videoWidth?' · '+v.videoWidth+'×'+v.videoHeight:''),
  get:async i=>{const t=Math.min(Math.max(0,v.duration-0.04),i/fps);
   if(Math.abs(v.currentTime-t)>0.02){const p=once(v,'seeked');try{v.currentTime=t}catch(e){}await Promise.race([p,sleep(2500)])}
   if(v.readyState<2)await sleep(30); return v},
  rel:()=>{}, done:()=>{try{v.removeAttribute('src');v.load()}catch(e){}URL.revokeObjectURL(url)}};
}

async function loadViaBitmaps(files,fpsUser){
 const a=files.slice(0,MAXF); const fps=a.length>1?Math.max(1,Math.min(20,fpsUser||10)):1;
 return {n:a.length,fps,note:a.length>1?('chuỗi '+a.length+' ảnh'):'ảnh tĩnh',
  get:i=>createImageBitmap(a[i]), rel:b=>{if(b&&b.close)b.close()}};
}

async function load(files){
 const f=[...files]; if(!f.length) throw new Error('Chưa chọn file');
 let fpsUser=+$('fps').value||12;
 if(SRC&&SRC.done) try{SRC.done()}catch(e){}
 SRC=null;
 const t0=guessType(f[0]); let s=null, lastErr=null;
 if(f.length===1&&isAnimImage(t0)){try{s=await loadViaImageDecoder(f[0],t0,fpsUser)}catch(e){lastErr=e}}
 if(!s&&f.length===1&&(isVideoType(t0)||!t0||/\.(mp4|webm|mov|mkv|m4v|avi)$/i.test(f[0].name||''))){
  try{s=await loadViaVideo(f[0],fpsUser)}catch(e){lastErr=e}}
 if(!s&&f.length===1&&t0.startsWith('image/')){try{s=await loadViaImageDecoder(f[0],t0,fpsUser)}catch(e){lastErr=e}}
 if(!s){try{s=await loadViaBitmaps(f,fpsUser)}catch(e){lastErr=e}}
 if(!s||!s.n) throw lastErr||new Error('Không đọc được file');
 if(f.length===1&&t0==='image/gif'&&s.n===1&&!('ImageDecoder' in window))
  s.note=(s.note||'')+' · trình duyệt không tách GIF';
 SRC=s; window._fps=s.fps;
 const b0=await s.get(0); fitTo(cx,W,H,b0); s.rel(b0);
 $('fmsg').textContent=(s.n>1?s.n+' khung · '+fmtT(s.n/s.fps)+' · '+s.fps+' khung/giây':'ảnh tĩnh')+(s.note?' · '+s.note:'');
 $('bF').disabled=$('bS').disabled=!s.n;
 if(s.n>30&&$('ql').value==='full') $('ql').value='jpg';
}
$('fi').onchange=async e=>{
 if(!e.target.files.length)return; FILES=[...e.target.files];
 $('fmsg').textContent='Đang giải mã…'; $('bF').disabled=$('bS').disabled=true;
 try{await load(FILES)}catch(x){$('fmsg').textContent='Không đọc được: '+(x&&x.message?x.message:x)}
};
$('fps').onchange=async()=>{if(FILES&&!busy)try{await load(FILES)}catch(x){$('fmsg').textContent='Lỗi: '+x.message}};
$('fit').onchange=async()=>{if(SRC&&!busy){try{const b=await SRC.get(0);fitTo(cx,W,H,b);SRC.rel(b)}catch(e){}}};

const JMAX=20000;
const toJpg=(c,q)=>new Promise((res,rej)=>{c.toBlob(b=>{if(!b)return rej(new Error('toBlob fail'));res(b)},'image/jpeg',q)});
async function build(onp){
 const s=SRC,n=s.n,mode=n>1?($('ql').value||'jpg'):'full';
 const half=mode!=='full',w=half?160:W,h=half?120:H;
 const c=document.createElement('canvas');c.width=w;c.height=h;
 const g=c.getContext('2d',{willReadFrequently:true}),parts=[];
 if(mode==='jpg'){
  const sizes=[];let mx=0;
  for(let i=0;i<n;i++){
   const src=await s.get(i);fitTo(g,w,h,src);s.rel(src);
   let q=0.82,u;
   for(;;){const blob=await toJpg(c,q);u=new Uint8Array(await blob.arrayBuffer());if(u.length<=JMAX||q<=0.28)break;q-=0.1}
   if(u.length>JMAX)throw new Error('Khung '+(i+1)+' quá nặng');
   parts.push(u);sizes.push(u.length);if(u.length>mx)mx=u.length;
   if(i%4===0||i===n-1)onp(i+1,n);
  }
  const hd=new Uint8Array(16),idx=new Uint32Array(n+1),dv=new DataView(hd.buffer);
  hd.set([67,89,74,49]);dv.setUint16(4,n,true);dv.setUint16(6,s.fps,true);dv.setUint16(8,160,true);dv.setUint16(10,120,true);dv.setUint32(12,mx,true);
  let o=16+4*(n+1);for(let i=0;i<n;i++){idx[i]=o;o+=sizes[i]}idx[n]=o;
  return new Blob([hd,idx,...parts]);
 }
 const hd=new Uint8Array(16);hd.set([67,89,65,half?50:49]);
 const dv=new DataView(hd.buffer);dv.setUint16(4,n,true);dv.setUint16(6,s.fps,true);parts.push(hd);
 for(let i=0;i<n;i++){
  const src=await s.get(i);fitTo(g,w,h,src);s.rel(src);
  const d=g.getImageData(0,0,w,h).data,u=new Uint16Array(w*h);
  for(let k=0,j=0;k<u.length;k++,j+=4)u[k]=((d[j]&0xF8)<<8)|((d[j+1]&0xFC)<<3)|(d[j+2]>>3);
  parts.push(u); if(i%4===0||i===n-1)onp(i+1,n);
 }
 return new Blob(parts);
}
function send(blob,dst,name){return new Promise((ok,no)=>{const x=new XMLHttpRequest(),fd=new FormData();
 fd.append('f',blob,'w.cya');x.open('POST','/api/up?dst='+dst+'&name='+encodeURIComponent(name)+'&size='+blob.size);x.timeout=0;
 x.upload.onprogress=e=>{if(e.lengthComputable){$('pg').style.width=(e.loaded/e.total*100)+'%';$('pm').textContent='Đang tải lên SD '+Math.round(e.loaded/e.total*100)+'% · '+kb(e.loaded/1024)+' / '+kb(e.total/1024)}};
 x.onload=()=>{if(x.status===200)return ok();let m='';try{const r=JSON.parse(x.responseText);m=r.err+' (nhận '+r.got+' / '+blob.size+' byte)'}catch(e){m='không rõ'}no(new Error('CYD từ chối ('+x.status+'): '+m))};
 x.onerror=()=>no(new Error('Mất kết nối'));x.send(fd)})}
function sendFrame(blob){return new Promise((ok,no)=>{
 const x=new XMLHttpRequest(),fd=new FormData(); fd.append('f',blob,'f.jpg');
 x.open('POST','/api/upframe'); x.timeout=120000;
 x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(e){}
  if(x.status===200&&r.ok)return ok(r); no(new Error(r.err||('frame HTTP '+x.status)))};
 x.onerror=()=>no(new Error('Mất kết nối khi gửi khung')); x.send(fd)})}
async function saveProgressive(dst,name){
 const s=SRC,n=s.n,fps=s.fps||12;
 if(dst==='flash'&&n>30) throw new Error('Video dài phải lưu vào thẻ SD');
 if(dst==='sd'&&info&&!info.sdOk) throw new Error('CYD chưa có thẻ SD');
 const free=info?(dst==='flash'?info.fsTotal-info.fsUsed:info.sdTotal-info.sdUsed):-1;
 if(free>=0&&(n*6)>free-32) throw new Error('Có thể không đủ chỗ: còn ~'+kb(free));
 $('pm').textContent='Khởi tạo file trên CYD…';
 const st=await fetch('/api/upstart?dst='+encodeURIComponent(dst)+'&name='+encodeURIComponent(name)+'&frames='+n+'&fps='+fps,{method:'POST'});
 const sj=await st.json(); if(!st.ok||!sj.ok) throw new Error(sj.err||'upstart thất bại');
 const c=document.createElement('canvas');c.width=160;c.height=120;
 const g=c.getContext('2d',{willReadFrequently:true}); let bytes=0;
 try{
  for(let i=0;i<n;i++){
   const src=await s.get(i); fitTo(g,160,120,src); s.rel(src);
   let q=0.82,blob; for(;;){blob=await toJpg(c,q); if(blob.size<=JMAX||q<=0.28)break; q-=0.1}
   if(blob.size>JMAX) throw new Error('Khung '+(i+1)+' quá nặng');
   await sendFrame(blob); bytes+=blob.size;
   const pct=Math.round((i+1)/n*100);
   $('pg').style.width=pct+'%';
   $('pm').textContent='Nén + ghi SD '+pct+'% · khung '+(i+1)+'/'+n+' · '+kb(bytes/1024);
  }
  $('pm').textContent='Đóng file trên thẻ SD…';
  const fin=await fetch('/api/upfinish',{method:'POST'}); const fj=await fin.json();
  if(!fin.ok||!fj.ok) throw new Error(fj.err||'upfinish thất bại');
 }catch(e){try{await fetch('/api/upabort',{method:'POST'})}catch(x){} throw e}
 return bytes;
}
async function save(dst){
 if(!SRC)return;busy=true;$('bF').disabled=$('bS').disabled=true;
 const name=($('nm').value||'anh').replace(/[^A-Za-z0-9_-]/g,'').slice(0,24)||'anh';
 try{
  if(dst==='sd'&&info&&!info.sdOk)throw new Error('CYD chưa có thẻ SD');
  const multi=SRC.n>1, mode=multi?($('ql').value||'jpg'):'full';
  if(multi&&mode==='jpg'){
   if(dst==='flash'){ if(info&&info.sdOk){dst='sd';$('pm').textContent='Video → tự chuyển lưu thẻ SD…'}
    else throw new Error('Video/GIF phải lưu thẻ SD')}
   const bytes=await saveProgressive(dst,name);
   if($('ua').checked){await api('/api/use?src=sd&name='+encodeURIComponent(name));
    $('pm').textContent='Xong. Đã ghi '+kb(bytes/1024)+' lên thẻ SD và đặt làm nền. CYD chỉ đọc file sẵn.'}
   else $('pm').textContent='Xong. Đã ghi '+kb(bytes/1024)+' lên thẻ SD (CYD chỉ đọc). Bấm "Dùng" để đặt nền.';
  }else{
   const b=await build((i,n)=>{$('pm').textContent='Đang chuyển khung '+i+' / '+n+'…';$('pg').style.width=(i/n*100)+'%'});
   const free=info?(dst==='flash'?info.fsTotal-info.fsUsed:info.sdTotal-info.sdUsed):-1;
   if(free>=0&&b.size/1024>free-8)throw new Error('Không đủ chỗ: cần '+kb(b.size/1024)+', còn '+kb(free));
   $('pg').style.width='0';$('pm').textContent='Đang tải lên '+kb(b.size/1024)+'…';
   await send(b,dst,name);
   if($('ua').checked){await api('/api/use?src='+(dst==='flash'?'flash&name=':'sd&name='+encodeURIComponent(name)));
    $('pm').textContent='Xong. Đã đặt làm ảnh nền (CYD chỉ đọc file sẵn).'}
   else $('pm').textContent='Xong. Bấm "Dùng" trong danh sách hoặc chọn trên CYD.';
  }
 }catch(e){$('pm').textContent='Lỗi: '+e.message}
 busy=false;$('bF').disabled=$('bS').disabled=false;refresh()}
$('bF').onclick=()=>save('flash');$('bS').onclick=()=>save('sd');

$('bSet').onclick=async()=>{let m=0;document.querySelectorAll('.cb:checked').forEach(c=>m|=1<<c.dataset.b);
 await api('/api/set?idle='+$('idle').value+'&lbr='+$('lbr').value+'&rot='+(+$('rot').checked)+'&vall='+(+$('vall').checked)+'&mask='+m);
 $('bSet').textContent='Đã lưu ✓';setTimeout(()=>$('bSet').textContent='Lưu cài đặt',1500);refresh()};
$('bLock').onclick=()=>api('/api/lock');
refresh();setInterval(()=>{if(!busy)refresh()},4000);
</script></body></html>)CYD";

static String cleanName(const String& n) {
  String o;
  for (unsigned i = 0; i < n.length() && o.length() < 24; i++) {
    char c = n[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-') o += c;
  }
  if (o.length() == 0) o = "anh";
  return o;
}

static void hRoot() { web.sendHeader("Cache-Control", "no-store"); web.send_P(200, "text/html; charset=utf-8", PAGE); }

static void jFileInfo(String& j, fs::File& f, const String& name, bool first) {
  uint16_t fr = 0, fp = 8;
  uint8_t kd = 0;
  bool ok = cyaInfoK(f, fr, fp, kd);
  if (!first) j += ',';
  j += "{\"n\":\""; j += name; j += "\",\"s\":"; j += (unsigned long)(f.size() / 1024);
  j += ",\"f\":"; j += fr; j += ",\"fps\":"; j += fp; j += ",\"ok\":"; j += ok ? "true" : "false"; j += ",\"k\":"; j += (int)kd; j += "}";
}

static void hInfo() {
  String j;
  j.reserve(1800);
  j += "{\"ip\":\""; j += WiFi.localIP().toString(); j += "\"";
  j += ",\"host\":\""; j += CYD_HOSTNAME; j += ".local\"";
  j += ",\"rssi\":"; j += WiFi.RSSI();
  j += ",\"heap\":"; j += ESP.getFreeHeap();
  j += ",\"heapTotal\":"; j += ESP.getHeapSize();
  j += ",\"sdOk\":"; j += sdOk ? "true" : "false";
  FS_LOCK();
  unsigned long sdT = (unsigned long)(sdTotalC / 1024), sdU = (unsigned long)(sdUsedC / 1024);
  unsigned long fsT = fsOk ? (unsigned long)(LittleFS.totalBytes() / 1024) : 0, fsU = fsOk ? (unsigned long)(LittleFS.usedBytes() / 1024) : 0;
  j += ",\"sdTotal\":"; j += sdT; j += ",\"sdUsed\":"; j += sdU;
  j += ",\"fsTotal\":"; j += fsT; j += ",\"fsUsed\":"; j += fsU;
  j += ",\"flash\":{";
  {
    fs::File f = fsOk ? LittleFS.open("/bg.cya", "r") : fs::File();
    uint16_t fr = 0, fp = 8;
    bool ok = f && cyaInfo(f, fr, fp);
    j += "\"ok\":"; j += ok ? "true" : "false";
    j += ",\"f\":"; j += fr; j += ",\"s\":"; j += f ? (unsigned long)(f.size() / 1024) : 0UL;
    if (f) f.close();
  }
  j += "},\"files\":[";
  if (sdOk) {
    fs::File d = SD.open("/wall");
    bool first = true;
    if (d) {
      for (;;) {
        fs::File e = d.openNextFile();
        if (!e) break;
        String nm = e.name();
        int sl = nm.lastIndexOf('/');
        if (sl >= 0) nm = nm.substring(sl + 1);
        if (!e.isDirectory() && nm.endsWith(".cya")) { jFileInfo(j, e, nm.substring(0, nm.length() - 4), first); first = false; }
        e.close();
      }
      d.close();
    }
  }
  FS_UNLOCK();
  j += "],\"active\":\""; j += cfgWp; j += "\"";
  j += ",\"idle\":"; j += cfgIdleSec;
  j += ",\"rot\":"; j += cfgRot ? 1 : 0;
  j += ",\"vall\":"; j += cfgVidAll ? 1 : 0;
  j += ",\"mask\":"; j += cfgMask;
  j += ",\"lbr\":"; j += cfgLockBr;
  j += ",\"lamp\":"; j += lamp;
  j += "}";
  web.send(200, "application/json", j);
}

static void hUse() {
  String src = web.arg("src"), name = cleanName(web.arg("name"));
  if (src == "flash") strcpy(cfgWp, "flash");
  else if (src == "sd") snprintf(cfgWp, sizeof(cfgWp), "sd:%s", name.c_str());
  else { web.send(400, "application/json", "{\"ok\":0}"); return; }
  prefs.putString("wp", cfgWp);
  wallDetach();
  pageDirty = true;
  web.send(200, "application/json", "{\"ok\":1}");
}

static void hDel() {
  String src = web.arg("src"), name = cleanName(web.arg("name"));
  wallDetach();
  FS_LOCK();
  if (src == "flash") { if (fsOk) LittleFS.remove("/bg.cya"); }
  else if (sdOk) { String p = "/wall/" + name + ".cya"; SD.remove(p.c_str()); }
  FS_UNLOCK();
  sdStamp = 0;
  wlStale = true; pageDirty = true;
  if ((src == "flash" && !strcmp(cfgWp, "flash")) || (src == "sd" && !strcmp(cfgWp, ("sd:" + name).c_str()))) {
    strcpy(cfgWp, "flash"); prefs.putString("wp", cfgWp);
  }
  web.send(200, "application/json", "{\"ok\":1}");
}

static void hSet() {
  if (web.hasArg("idle")) cfgIdleSec = (uint16_t)web.arg("idle").toInt();
  if (web.hasArg("rot"))  cfgRot = web.arg("rot").toInt() != 0;
  if (web.hasArg("vall")) cfgVidAll = web.arg("vall").toInt() != 0;
  if (web.hasArg("mask")) cfgMask = (uint8_t)web.arg("mask").toInt();
  if (web.hasArg("lbr"))  { int v = web.arg("lbr").toInt(); cfgLockBr = (uint8_t)(v < 5 ? 5 : (v > 100 ? 100 : v)); }
  saveCfg();
  pageDirty = true;
  web.send(200, "application/json", "{\"ok\":1}");
}
static void hLock() { lockReq = true; web.send(200, "application/json", "{\"ok\":1}"); }

// ---- tai anh len (multipart) ----
static fs::File upF;
static bool upOk = false, upSD = false;
static String upTmp, upFin;
static const char* upErr = "";
static uint32_t upGot = 0;
static uint8_t upHdr[CYA_HDR];          // 16 byte dau cua file, giu trong RAM de kiem tra khong phai doc lai tu the SD
static uint8_t upHdrN = 0;

static void hUpload() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    upOk = false; upErr = ""; upGot = 0; upHdrN = 0;
    upSD = (web.arg("dst") == "sd");
    if (upSD) { String n = cleanName(web.arg("name")); upFin = "/wall/" + n + ".cya"; upTmp = "/wall/" + n + ".tmp"; }
    else { upFin = "/bg.cya"; upTmp = "/bg.tmp"; }
    if ((upSD && !sdOk) || (!upSD && !fsOk)) { upErr = upSD ? "the SD chua mount" : "LittleFS chua mount"; return; }
    FS_LOCK();
    if (upSD) {
      SD.remove(upTmp.c_str());
      if (!SD.exists("/wall")) SD.mkdir("/wall");
      upF = SD.open(upTmp.c_str(), "w");
    } else { LittleFS.remove(upTmp.c_str()); upF = LittleFS.open(upTmp.c_str(), "w"); }
    FS_UNLOCK();
    upOk = (bool)upF;
    if (!upOk) upErr = "khong mo duoc file de ghi";
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upOk && upF) {
      FS_LOCK();
      size_t w = upF.write(u.buf, u.currentSize);
      if (w != u.currentSize) {                 // ghi loi: thu 1 lan nua truoc khi bo cuoc
        size_t done = w;
        for (int t = 0; t < 3 && done < u.currentSize; t++) {
          delay(5);
          done += upF.write(u.buf + done, u.currentSize - done);
        }
        w = done;
      }
      FS_UNLOCK();
      for (size_t i = 0; i < u.currentSize && upHdrN < CYA_HDR; i++) upHdr[upHdrN++] = u.buf[i];
      upGot += w;
      if (w != u.currentSize) { upOk = false; upErr = "ghi the SD that bai giua chung"; }
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (upF) { FS_LOCK(); upF.close(); FS_UNLOCK(); }
    if (upOk) {
      // Kiem tra 1: du dung luong nhu trinh duyet bao (tham so size) va header hop le (doc tu RAM)
      uint32_t want = (uint32_t)web.arg("size").toInt();
      bool good = (upHdrN == CYA_HDR) && (want == 0 || want == upGot) &&
                  (!memcmp(upHdr, "CYA1", 4) || !memcmp(upHdr, "CYA2", 4) || !memcmp(upHdr, "CYJ1", 4)) &&
                  (upHdr[4] | (upHdr[5] << 8)) != 0;
      // Kiem tra 2: doc lai file tu the nho; neu loi thi thu lai vai lan (the SD doi khi doc truot ngay sau khi ghi)
      uint32_t onDisk = 0; bool reopened = false;
      if (good) {
        bool ok2 = false;
        for (int t = 0; t < 3 && !ok2; t++) {
          if (t) delay(30);
          FS_LOCK();
          fs::File v = upSD ? SD.open(upTmp.c_str(), "r") : LittleFS.open(upTmp.c_str(), "r");
          uint16_t fr, fp;
          reopened = (bool)v;
          if (v) onDisk = (uint32_t)v.size();
          ok2 = v && cyaInfo(v, fr, fp);
          if (v) v.close();
          FS_UNLOCK();
        }
        if (!ok2) {
          Serial.printf("UPLOAD verify FAIL: reopened=%d onDisk=%u got=%u want=%u hdr=%02X%02X%02X%02X frames=%u\n",
                        (int)reopened, (unsigned)onDisk, (unsigned)upGot, (unsigned)want,
                        upHdr[0], upHdr[1], upHdr[2], upHdr[3], (unsigned)(upHdr[4] | (upHdr[5] << 8)));
          // Neu dung luong tren the dung bang so byte da nhan thi tin file la dung, khong bo file
          good = (reopened && onDisk == upGot);
        }
      }
      if (good) {
        wallDetach();
        FS_LOCK();
        if (upSD) { SD.remove(upFin.c_str()); upOk = SD.rename(upTmp.c_str(), upFin.c_str()); }
        else      { LittleFS.remove(upFin.c_str()); upOk = LittleFS.rename(upTmp.c_str(), upFin.c_str()); }
        FS_UNLOCK();
        sdStamp = 0;
        wlStale = true; pageDirty = true;
        if (!upOk) upErr = "doi ten file that bai";
      } else { upOk = false; upErr = "file khong hop le (header sai hoac thieu du lieu)"; }
    }
    if (!upOk) {
      FS_LOCK();
      if (upSD) { if (sdOk) SD.remove(upTmp.c_str()); } else if (fsOk) LittleFS.remove(upTmp.c_str());
      FS_UNLOCK();
    }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (upF) { FS_LOCK(); upF.close(); FS_UNLOCK(); }
    FS_LOCK();
    if (upSD) { if (sdOk) SD.remove(upTmp.c_str()); } else if (fsOk) LittleFS.remove(upTmp.c_str());
    FS_UNLOCK();
    upOk = false;
  }
}

// ---- upload tung khung CYJ1: trinh duyet da giai ma+nen; CYD chi ghi SD + doc lai ----
static bool     upProg = false;
static uint16_t upFrames = 0, upFps = 8, upFrameI = 0;
static uint32_t upMaxJpeg = 0, upDataBase = 0, upFrameGot = 0;
static uint32_t* upOff = nullptr;

static void upProgCleanup(bool removeTmp) {
  if (upF) { FS_LOCK(); upF.close(); upF = fs::File(); FS_UNLOCK(); }
  if (upOff) { free(upOff); upOff = nullptr; }
  if (removeTmp && upTmp.length()) {
    FS_LOCK();
    if (upSD) { if (sdOk) SD.remove(upTmp.c_str()); }
    else if (fsOk) LittleFS.remove(upTmp.c_str());
    FS_UNLOCK();
  }
  upProg = false; upFrames = 0; upFrameI = 0; upMaxJpeg = 0; upDataBase = 0; upGot = 0; upFrameGot = 0;
}

static void hUpStart() {
  upProgCleanup(true);
  upOk = false; upErr = ""; upGot = 0; upHdrN = 0;
  upSD = (web.arg("dst") == "sd");
  upFrames = (uint16_t)web.arg("frames").toInt();
  upFps = (uint16_t)web.arg("fps").toInt();
  if (upFps == 0 || upFps > 30) upFps = 8;
  if (upFrames == 0 || upFrames > 65000) {
    web.send(400, "application/json", "{\"ok\":0,\"err\":\"frames khong hop le\"}"); return;
  }
  if (upSD) {
    String n = cleanName(web.arg("name"));
    upFin = "/wall/" + n + ".cya"; upTmp = "/wall/" + n + ".tmp";
  } else {
    if (upFrames > 30) { web.send(400, "application/json", "{\"ok\":0,\"err\":\"video dai phai luu the SD\"}"); return; }
    upFin = "/bg.cya"; upTmp = "/bg.tmp";
  }
  if ((upSD && !sdOk) || (!upSD && !fsOk)) {
    web.send(500, "application/json", upSD ? "{\"ok\":0,\"err\":\"the SD chua mount\"}" : "{\"ok\":0,\"err\":\"LittleFS chua mount\"}"); return;
  }
  size_t idxBytes = (size_t)(upFrames + 1) * 4;
  upOff = (uint32_t*)malloc(idxBytes);
  if (!upOff) { web.send(500, "application/json", "{\"ok\":0,\"err\":\"het RAM index\"}"); return; }
  memset(upOff, 0, idxBytes);
  upDataBase = CYA_HDR + (uint32_t)idxBytes;

  FS_LOCK();
  if (upSD) {
    SD.remove(upTmp.c_str());
    if (!SD.exists("/wall")) SD.mkdir("/wall");
    upF = SD.open(upTmp.c_str(), "w");
  } else {
    LittleFS.remove(upTmp.c_str());
    upF = LittleFS.open(upTmp.c_str(), "w");
  }
  bool opened = (bool)upF;
  if (opened) {
    uint8_t hd[CYA_HDR]; memset(hd, 0, sizeof(hd)); memcpy(hd, "CYJ1", 4);
    hd[4] = upFrames & 0xFF; hd[5] = upFrames >> 8;
    hd[6] = upFps & 0xFF; hd[7] = upFps >> 8;
    hd[8] = 160; hd[10] = 120;
    upF.write(hd, CYA_HDR);
    uint8_t z[256]; memset(z, 0, sizeof(z));
    size_t left = idxBytes;
    while (left) { size_t n = left > sizeof(z) ? sizeof(z) : left; upF.write(z, n); left -= n; }
    upOff[0] = upDataBase; upFrameI = 0; upMaxJpeg = 0; upGot = upDataBase;
    upProg = true; upOk = true;
  }
  FS_UNLOCK();
  if (!opened) { free(upOff); upOff = nullptr; web.send(500, "application/json", "{\"ok\":0,\"err\":\"khong mo duoc file\"}"); return; }
  web.send(200, "application/json", "{\"ok\":1}");
}

static void hUpFrameUpload() {
  HTTPUpload& u = web.upload();
  if (!upProg || !upOk) return;
  if (u.status == UPLOAD_FILE_START) { upFrameGot = 0; }
  else if (u.status == UPLOAD_FILE_WRITE) {
    if (!upF) return;
    FS_LOCK();
    size_t w = upF.write(u.buf, u.currentSize);
    if (w != u.currentSize) {
      size_t done = w;
      for (int t = 0; t < 3 && done < u.currentSize; t++) { delay(5); done += upF.write(u.buf + done, u.currentSize - done); }
      w = done;
    }
    FS_UNLOCK();
    upFrameGot += w; upGot += w;
    if (w != u.currentSize) { upOk = false; upErr = "ghi SD loi"; }
  } else if (u.status == UPLOAD_FILE_END) {
    if (upFrameGot > upMaxJpeg) upMaxJpeg = upFrameGot;
    upFrameI++;
    if (upFrameI <= upFrames && upOff) upOff[upFrameI] = upOff[upFrameI - 1] + upFrameGot;
  } else if (u.status == UPLOAD_FILE_ABORTED) { upOk = false; upErr = "frame aborted"; }
}
static void hUpFrameDone() {
  if (!upProg) { web.send(400, "application/json", "{\"ok\":0,\"err\":\"chua upstart\"}"); return; }
  if (!upOk) {
    String j = "{\"ok\":0,\"err\":\""; j += (upErr[0] ? upErr : "loi"); j += "\",\"i\":"; j += upFrameI; j += "}";
    web.send(500, "application/json", j); return;
  }
  String j = "{\"ok\":1,\"i\":"; j += upFrameI; j += ",\"n\":"; j += upFrames; j += "}";
  web.send(200, "application/json", j);
}

static void hUpFinish() {
  if (!upProg) { web.send(400, "application/json", "{\"ok\":0,\"err\":\"chua upstart\"}"); return; }
  if (!upOk) { upProgCleanup(true); web.send(500, "application/json", "{\"ok\":0,\"err\":\"upload loi truoc do\"}"); return; }
  if (upFrameI != upFrames) {
    String j = "{\"ok\":0,\"err\":\"thieu khung\",\"got\":"; j += upFrameI; j += ",\"need\":"; j += upFrames; j += "}";
    upProgCleanup(true); web.send(400, "application/json", j); return;
  }
  FS_LOCK();
  if (upF) { upF.close(); upF = fs::File(); }
  upF = upSD ? SD.open(upTmp.c_str(), "r+") : LittleFS.open(upTmp.c_str(), "r+");
  bool ok = (bool)upF;
  if (ok) {
    uint8_t hd[CYA_HDR]; memset(hd, 0, sizeof(hd)); memcpy(hd, "CYJ1", 4);
    hd[4] = upFrames & 0xFF; hd[5] = upFrames >> 8;
    hd[6] = upFps & 0xFF; hd[7] = upFps >> 8;
    hd[8] = 160; hd[10] = 120;
    hd[12] = upMaxJpeg & 0xFF; hd[13] = (upMaxJpeg >> 8) & 0xFF;
    hd[14] = (upMaxJpeg >> 16) & 0xFF; hd[15] = (upMaxJpeg >> 24) & 0xFF;
    upF.seek(0); upF.write(hd, CYA_HDR);
    for (uint16_t i = 0; i <= upFrames; i++) {
      uint32_t o = upOff[i];
      uint8_t b[4] = { (uint8_t)o, (uint8_t)(o >> 8), (uint8_t)(o >> 16), (uint8_t)(o >> 24) };
      upF.write(b, 4);
    }
    upF.close(); upF = fs::File();
  }
  FS_UNLOCK();
  if (ok) {
    wallDetach();
    FS_LOCK();
    if (upSD) { SD.remove(upFin.c_str()); ok = SD.rename(upTmp.c_str(), upFin.c_str()); }
    else { LittleFS.remove(upFin.c_str()); ok = LittleFS.rename(upTmp.c_str(), upFin.c_str()); }
    FS_UNLOCK();
    sdStamp = 0; wlStale = true; pageDirty = true;
  }
  if (upOff) { free(upOff); upOff = nullptr; }
  upProg = false;
  if (!ok) { upProgCleanup(true); web.send(500, "application/json", "{\"ok\":0,\"err\":\"finalize that bai\"}"); return; }
  upOk = true;
  web.send(200, "application/json", "{\"ok\":1}");
}

static void hUpAbort() {
  upProgCleanup(true);
  web.send(200, "application/json", "{\"ok\":1}");
}

static void hUploadDone() {
  Serial.printf("UPLOAD %s: err=%s got=%u dst=%s tmp=%s sdOk=%d\n", upOk ? "OK" : "FAIL", upErr, (unsigned)upGot, upSD ? "sd" : "flash", upTmp.c_str(), (int)sdOk);
  if (upOk) { web.send(200, "application/json", "{\"ok\":1}"); return; }
  String j = "{\"ok\":0,\"err\":\""; j += (upErr[0] ? upErr : "khong ro"); j += "\",\"got\":"; j += upGot; j += "}";
  web.send(500, "application/json", j);
}

static void webTask(void*) {
  for (;;) {
    sdRefresh();
    web.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ================= ARDUINO =================
void setup() {
  Serial.begin(115200);
  delay(300);
  fsMux = xSemaphoreCreateMutex();

  pinMode(TP_CLK, OUTPUT); pinMode(TP_CS, OUTPUT); pinMode(TP_DIN, OUTPUT);
  pinMode(TP_OUT, INPUT);  pinMode(TP_IRQ, INPUT);
  digitalWrite(TP_CS, HIGH); digitalWrite(TP_CLK, LOW);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // Den nen: bat bang digitalWrite nhu ban .ino don (chay duoc), PWM chi bat SAU khi man hinh da len
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  Serial.println("[boot] init TFT...");

  tft.init();
  tft.setRotation(ROTATION);
  tft.fillScreen(COL_BG);
  digitalWrite(TFT_BL_PIN, HIGH);
  Serial.println("[boot] TFT OK");
  tft.setFont(F7);
  digW = tft.textWidth("8");
  if (digW < 16 || digW > 60) digW = 32;

  // cap phat cac vung nho lon luc con trong
#if WALL_CACHE
  // dem anh nen (76.8 KB) cap phat TRUOC cac sprite; chi lay khi con du RAM cho WiFi/web
  if (ESP.getFreeHeap() > (uint32_t)WC_BYTES + 190000UL && ESP.getMaxAllocHeap() > (uint32_t)WC_BYTES + 8000UL) {
    wcache = (uint16_t*)malloc(WC_BYTES);
    wcReset();
  }
#endif
  pill.setColorDepth(16);
  pillOk = (pill.createSprite(CW, CH) != nullptr);
  if (!pillOk && wcache) {                        // thieu RAM cho nut -> nha dem anh nen
    free(wcache); wcache = nullptr;
    pillOk = (pill.createSprite(CW, CH) != nullptr);
  }
  band.setColorDepth(16);
  bandOk = (band.createSprite(SCR_W, BAND_H) != nullptr);
  if (!pillOk || !bandOk) Serial.println("Canh bao: khong tao duoc sprite!");
  haloBuild();
  Serial.printf("[boot] dem anh nen: %s | halo: %d diem | free=%u maxAlloc=%u\n", wcache ? "BAT" : "tat", haloN,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  prefs.begin("cydrelay", false);
  loadCfg();
  touchQ = xQueueCreate(16, sizeof(TEv));
  touchSetup();

  tft.fillScreen(COL_BG);
  T(&tft, "Dang ket noi WiFi...", 160, 110, F2, COL_WHITE, middle_center);
  Serial.println("[boot] bat PWM den nen");
  blInit();
  blSet(100);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(CYD_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
    MDNS.begin(CYD_HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Chua vao duoc WiFi (task mang se thu noi lai)");
  }
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");

  fsOk = LittleFS.begin(true);
  sdMount();
  wallOpen();                                   // anh nen dung cho moi man hinh

  web.on("/", hRoot);
  web.on("/api/info", hInfo);
  web.on("/api/use", hUse);
  web.on("/api/del", hDel);
  web.on("/api/set", hSet);
  web.on("/api/lock", hLock);
  web.on("/api/up", HTTP_POST, hUploadDone, hUpload);
  web.on("/api/upstart", HTTP_POST, hUpStart);
  web.on("/api/upframe", HTTP_POST, hUpFrameDone, hUpFrameUpload);
  web.on("/api/upfinish", HTTP_POST, hUpFinish);
  web.on("/api/upabort", HTTP_POST, hUpAbort);
  web.begin();

  // Dem 1 khung video CYA2 (38.4 KB): doc ca khung 1 lan -> video muot hon nhieu. Chi cap phat khi con du RAM cho WiFi/web.
  hframeEnsure();
  Serial.printf("[boot] dem khung video: %s | free=%u\n", jReady ? "JPEG" : (hframe ? "BAT" : "tat (thieu RAM)"), (unsigned)ESP.getFreeHeap());

  lastAct = millis();
  tft.fillScreen(COL_BG);
  page = PG_MAIN;
  drawMainFull();

  xTaskCreatePinnedToCore(netTask, "net", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(webTask, "web", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(touchTask, "touch", 3072, NULL, 3, NULL, 0);
}

void loop() {
  uint32_t now = millis();
  handleEvent();
  touchLoop(now);                       // cam ung da duoc task rieng doc; day chi xu ly su kien
  bootLoop(now);

  if (lockReq) { lockReq = false; if (!locked) enterLock(); }
  // (int32_t): lastAct do task cam ung ghi co the moi hon 'now' vai ms
  if (!locked && cfgIdleSec > 0 && (int32_t)(now - lastAct) > (int32_t)(cfgIdleSec * 1000UL)) enterLock();

  if (locked) {
    if (lkSliding) slideStep(); else lockTick(now);
  } else {
    if (wallDirty) { wallOpen(); pageRender(); }          // anh nen vua doi (web) -> ve lai trang
    animStep();
    pageTick(now);
    pageAsyncStep();
    vidStep(now);
  }
  if (!lkSliding && !animOn && pgNext >= SCR_H) delay(2);
}
