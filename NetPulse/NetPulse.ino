/*****************************************************************************************
 *
 *  NetPulse  -  network health logger for CYD 2.8" / Guition 4.3"  (Pulse Systems)
 *  Based on Internet Connection Monitor 24H
 *  ----------------------------------------------------------------------------
 *  Original design : Francesco Landi (v1.0, (c) 2026)
 *                    Free for personal and educational use. Commercial
 *                    redistribution or sale of modified versions requires
 *                    permission from the original author.
 *  This port       : CYD 2.8" (ESP32-2432S028R) and Guition JC4827W543C / R 4.3"
 *                    portrait + landscape, web-flasher setup, 24h / 7d / 14d history
 *
 *  Pages (tap anywhere to cycle):
 *    LIVE      health gauge, status, ping / Wi-Fi / uptime tiles, 10-minute trend,
 *              router > ISP > DNS path
 *    24 HOURS  1-minute history       7 DAYS / 14 DAYS  15-minute history
 *    EVENTS    outages, public IP changes, Wi-Fi drops, access point changes, restarts
 *    DEVICE    network / device / storage / test stats - hold here for SETTINGS
 *  History is saved to flash (LittleFS) and survives reboots and firmware updates.
 *
 *  Settings: config.h     Pins / drivers: board.h     Fonts: fonts.h (Chakra Petch, OFL)
 *  Library : "GFX Library for Arduino" by Moon On Our Nation (Arduino_GFX)
 *
 *****************************************************************************************/

#include "config.h"
#include "board.h"
#include "fonts.h"
#include "types.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <SPI.h>
#include <SD.h>
#include "netping.h"
#include "ota.h"
#if defined(TOUCH_GT911)
  #include <Wire.h>
#endif

#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// The HTTPS update check / download runs in loop(), and TLS needs far more stack than the
// default 8 KB loop task. Overflowing it silently corrupted nearby memory (the history file
// system) and crashed the board a few seconds later. 16 KB leaves plenty of room.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Firmware version = the GitHub release tag: "v2.1.0" official, "b2.1.0" beta (pre-release)
#define FW_VERSION "v2.3.1"

// =======================================================================================
// SETTINGS (stored in flash, written by the web flasher over USB serial)
// =======================================================================================
String cfgSsid, cfgPass, cfgTz;
uint8_t cfgRot = DEFAULT_ORIENTATION;   // 0 portrait, 1 landscape, 2 portrait flipped, 3 landscape flipped

// Device settings (SETTINGS menu on the DEVICE page; saved in flash). Defaults come from config.h.
static const uint8_t BRI_LEVEL[4] = { 60, 120, 180, 240 };            // 25 / 50 / 75 / 100 %
static const uint8_t NIGHT_PRESET[4][2] = { { 0, 0 }, { 22, 6 }, { 23, 7 }, { 21, 7 } };   // [0] = off
static const char *PING_SET1[3] = { PING_HOST, "8.8.8.8", "9.9.9.9" };
static const char *PING_SET2[3] = { PING_HOST2, "1.1.1.1", "1.1.1.1" };
uint8_t setBri = 3, setNight = 1, setPing = 0;
bool setLed = true, setSdLog = true, setUpdChk = true;
const char *pingHost1() { return PING_SET1[setPing % 3]; }
const char *pingHost2() { return PING_SET2[setPing % 3]; }
uint8_t dayBrightness() { return BRI_LEVEL[setBri & 3]; }

uint8_t defaultBri() {
  uint8_t best = 3;
  for (uint8_t i = 0; i < 4; i++) if (abs((int)BRI_LEVEL[i] - BRIGHT_DAY) < abs((int)BRI_LEVEL[best] - BRIGHT_DAY)) best = i;
  return best;
}
uint8_t defaultNight() {
  for (uint8_t i = 1; i < 4; i++) if (NIGHT_PRESET[i][0] == NIGHT_START_HOUR && NIGHT_PRESET[i][1] == NIGHT_END_HOUR) return i;
  return 1;
}

void saveSetting(const char *key, uint8_t v) {
  Preferences prefs;
  prefs.begin("imon", false);
  prefs.putUChar(key, v);
  prefs.end();
}

void loadSettings() {
  Preferences prefs;
  prefs.begin("imon", false);        // read-write: creates the namespace on first boot
  cfgSsid = prefs.getString("ssid", "").c_str();
  cfgPass = prefs.getString("pass", "").c_str();
  cfgTz   = prefs.getString("tz", MY_TIMEZONE).c_str();
  cfgRot  = prefs.getUChar("rot", DEFAULT_ORIENTATION) & 3;
  setBri    = prefs.getUChar("bri", defaultBri()) & 3;
  setNight  = prefs.getUChar("ngt", defaultNight()) & 3;
  setPing   = prefs.getUChar("png", 0) % 3;
  setLed    = prefs.getUChar("led", 1) != 0;
  setSdLog  = prefs.getUChar("sdl", SD_LOG ? 1 : 0) != 0;
  setUpdChk = prefs.getUChar("upd", UPDATE_CHECK ? 1 : 0) != 0;
  prefs.end();
  if (cfgSsid.length() == 0 && strcmp(WIFI_SSID, "YOUR WIFI NAME") != 0) {
    cfgSsid = WIFI_SSID;
    cfgPass = WIFI_PASS;
  }
}

// =======================================================================================
// THEME
// =======================================================================================
Arduino_GFX *gfx = nullptr;

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
// Cyberpunk palette: near-black violet panels, neon cyan / mint / yellow / magenta
#define C_BG      RGB(5, 4, 12)
#define C_HEADER  RGB(11, 8, 24)
#define C_CARD    RGB(13, 11, 28)
#define C_CARD2   RGB(30, 24, 58)
#define C_LINE    RGB(52, 40, 100)
#define C_TRACK   RGB(34, 26, 64)
#define C_WHITE   RGB(236, 238, 255)
#define C_GREY    RGB(146, 136, 200)
#define C_DIM     RGB(92, 82, 148)
#define C_CYAN    RGB(0, 240, 255)
#define C_GREEN   RGB(5, 255, 161)
#define C_YELLOW  RGB(252, 238, 10)
#define C_ORANGE  RGB(255, 150, 40)
#define C_RED     RGB(255, 42, 109)
#define C_PINK    RGB(255, 0, 200)
#define C_GREEN_D RGB(2, 58, 42)
#define C_YELL_D  RGB(62, 58, 4)
#define C_RED_D   RGB(72, 8, 36)
#define C_CYAN_D  RGB(0, 46, 60)
#define C_ROUTER  RGB(150, 110, 255)
#define C_ISP     RGB(0, 200, 255)

// =======================================================================================
// STATE
// =======================================================================================
bool wifiOK = false, dnsOK = false, internetOK = false, captivePortal = false;
int  wifiRSSI = -100;
long latencyMs = -1;             // real ping to PING_HOST: average of this test's burst, -1 = no reply
long pingMaxMs = -1;             // worst single ping in this test's burst
long jitterMs = -1;              // average change between consecutive pings, -1 = not enough data
long gwMs = -1;                  // ping to your router (gateway), -1 = no reply
long webMs = -1;                 // time to load TEST_URL (the web check), for the log
long ping2Ms = -1;               // second internet target (PING_HOST2)
long ispMs = -1;                 // ISP's first router, -1 = not found / no answer
long dnsMs = -1;                 // DNS lookup straight to your DNS server
int  pingSent = 0, pingLost = 0; // this test's pings (both targets)
int  ping1Sent = 0, ping1Lost = 0, ping2Sent = 0, ping2Lost = 0;
uint32_t ispIp = 0;              // ISP hop address (network order), 0 = not found yet
int  ispTtl = 0;
int  ispFails = 0;
unsigned long ispLastSearch = 0;
String wanIp = "";               // public IP as the internet sees it
unsigned long wanLastCheck = 0;
String apKey = "";               // Wi-Fi access point (BSSID + channel) we're on

// ---- event log -------------------------------------------------------------------------
const int EV_MAX = 48;
Event evRing[EV_MAX];
int evCount = 0, evHead = 0;               // evHead = next slot to write
bool evDirty = false;                      // needs saving to flash
static const char *EV_NAME[EV_TYPES] = { "RESTART", "OUTAGE", "BACK ONLINE", "WI-FI DROP", "IP CHANGE", "AP CHANGE", "ISP ROUTER", "FIRMWARE" };
// outage tracking
bool inOutage = false, outageJustEnded = false;
uint32_t outStartT = 0, outStartMs = 0;
String outCause = "";
// Wi-Fi disconnect reason from the Wi-Fi task
volatile uint8_t wifiDiscReason = 0;
volatile bool wifiDiscFlag = false;
bool wifiWasUp = false;
float lossPct = -1;              // packet loss over the last LOSS_WINDOW tests, -1 = no data
int  health = 0;
int  httpCode = 0;
Level level = LV_BAD;

String statusText = "BOOT";
String ipText = "--";
String timeText = "--:--";
String wifiStateText = "Starting";

unsigned long lastScreenUpdate = 0, lastInternetTest = 0, lastWifiRetry = 0;

// 10-minute trend on Home (RAM only): one sample per test
const int TREND_SIZE = 60;
int8_t  trendHealth[TREND_SIZE];
uint8_t trendLevel[TREND_SIZE];
int trendIndex = 0, trendCount = 0;

// recent pings for the jitter figure
int16_t jitPing[JITTER_SAMPLES];
int jitCount = 0, jitIndex = 0;
// recent tests for the loss figure
uint8_t lossSentW[LOSS_WINDOW], lossLostW[LOSS_WINDOW];
int lossCount = 0, lossIndex = 0;

Page currentPage = PAGE_HOME;
bool pageDirty = false;          // full redraw of the current page requested

// touch
bool touchLatched = false;
unsigned long lastTouchMs = 0, lastTouchSeenMs = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 350;
const unsigned long TOUCH_RELEASE_MS  = 180;

// backlight / night
unsigned long touchWakeMillis = 0;
uint8_t currentBrightness = 0;
bool nightDimmed = false;

bool fsOK = false;
bool wdtStarted = false;        // watchdog running (only fed once it is)

// firmware updates from GitHub
OtaInfo updInfo;
bool updAvail = false;          // a newer release that hasn't been ignored
String updSkip = "";            // release the user chose to ignore
String updErr = "";
unsigned long updLastCheck = 0;
bool updChecked = false;
// update prompt (tap = switch choice, hold = confirm)
bool modalOpen = false;
uint8_t modalSel = 0;           // 0 = UPDATE, 1 = IGNORE
unsigned long modalOpenedMs = 0;
// SETTINGS menu (tap = next item, hold = change / run it)
bool settingsOpen = false;
uint8_t setSel = 0;
unsigned long settingsTouchedMs = 0, clearArmedMs = 0;
String updCheckMsg = "";        // result of the last update check, for the menu / DEVICE page
uint32_t updCheckedT = 0;
// counters since boot (DEVICE page)
uint32_t testsRun = 0, totPingSent = 0, totPingLost = 0;
bool overlayOpen() { return modalOpen || settingsOpen; }

// microSD log
SPIClass sdSPI(SD_SPI_HOST);
bool spiBusStarted = false;

// microSD bus (also the resistive Guition's touch bus)
void spiBusBegin() {
  if (spiBusStarted) return;
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  spiBusStarted = true;
}
enum SdState : uint8_t { SD_NONE, SD_OK, SD_ERR };   // no card / writing / card stopped working
SdState sdState = SD_NONE;
unsigned long sdLastTry = 0;
uint32_t sdRows = 0;

// =======================================================================================
// HISTORY LOGS  -  fixed time slots, saved to LittleFS
//   log1m : 1440 x 1-minute slots  = 24 hours
//   log15m: 1344 x 15-minute slots = 14 days (7-day view uses the newest half)
// Each slot keeps average / worst quality, % of tests that reached the web, avg / max ping.
// Slots with the board switched off are stored as "no data" so the time axis stays true.
// =======================================================================================
static LogRec buf1m[1440];
static LogRec buf15m[1344];
SlotLog log1m("/h1m", 60, 1440, buf1m);
SlotLog log15m("/h15m", 900, 1344, buf15m);
bool dirty1m = false, dirty15m = false;

bool timeValid() { return time(nullptr) > 1700000000; }

const RangeDef RANGES[3] = {
  { "24 HOURS", "24H", &log1m, 1440 },
  { "7 DAYS",   "7D",  &log15m, 672 },
  { "14 DAYS",  "14D", &log15m, 1344 },
};

RangeStats calcStats(const RangeDef &R) {
  RangeStats st = { 0, 100.0f, -1, -1, 0, -1, -1, -1 };
  uint64_t lSum = 0, jSum = 0, gSum = 0;
  uint32_t lN = 0, jN = 0, gN = 0;
  if (!timeValid()) return st;
  uint32_t nowSlot = (uint32_t)time(nullptr) / R.log->slotSec();
  uint64_t upSum = 0, pSum = 0;
  uint32_t pN = 0;
  LogRec r;
  for (uint32_t a = 0; a < R.span; a++) {
    if (!R.log->recAt(nowSlot - a, r)) continue;
    st.slots++;
    upSum += r.up;
    st.downSec += (uint32_t)(100 - r.up) * R.log->slotSec() / 100;
    if (r.pAvg != 0xFFFF) { pSum += r.pAvg; pN++; }
    if (r.pMax != 0xFFFF && (int)r.pMax > st.worstPing) st.worstPing = r.pMax;
    if (r.loss != 0xFFFF) { lSum += r.loss; lN++; }
    if (r.jit != 0xFFFF) { jSum += r.jit; jN++; }
    if (r.gw != 0xFFFF) { gSum += r.gw; gN++; }
  }
  if (lN) st.lossPct = lSum / 10.0f / lN;
  if (jN) st.avgJit = (int)(jSum / jN);
  if (gN) st.avgGw = (int)(gSum / gN);
  if (st.slots) st.upPct = (float)upSum / st.slots;
  if (pN) st.avgPing = (int)(pSum / pN);
  return st;
}

// =======================================================================================
// LAYOUT + FONTS (computed from the real screen size / orientation)
// =======================================================================================
Layout L;

void computeLayout() {
  L.W = gfx->width();
  L.H = gfx->height();
  L.land = L.W > L.H;
  L.big = max(L.W, L.H) >= 400;
  L.m = L.big ? 8 : 6;
  if (L.big) { L.fXS = &Cp10; L.fS = &Cp11; L.fM = &CpB16; L.fL = &CpB21; L.fXL = &CpB34; }
  else       { L.fXS = &Cp8; L.fS = &Cp10; L.fM = &CpB13; L.fL = &CpB17; L.fXL = &CpB25; }

  const int m = L.m;
  L.hdrH = L.big ? (L.land ? 36 : 42) : 30;
  L.footH = L.big ? 22 : 16;
  L.footY = L.H - L.footH;
  const int top = L.hdrH + m, bottom = L.footY - m / 2;

  if (!L.land) {
    int heroH = L.big ? 226 : 140;
    int tilesH = L.big ? 72 : 50;
    L.hero  = { (int16_t)m, (int16_t)top, (int16_t)(L.W - 2 * m), (int16_t)heroH };
    L.tiles = { (int16_t)m, (int16_t)(L.hero.b() + m), (int16_t)(L.W - 2 * m), (int16_t)tilesH };
    L.trend = { (int16_t)m, (int16_t)(L.tiles.b() + m), (int16_t)(L.W - 2 * m), (int16_t)(bottom - (L.tiles.b() + m)) };
    L.tilesVertical = false;
  } else if (!L.big) {                     // CYD landscape 320x240
    int heroW = 148, heroH = 128;
    L.hero  = { (int16_t)m, (int16_t)top, (int16_t)heroW, (int16_t)heroH };
    int rx = m + heroW + m;
    L.tiles = { (int16_t)rx, (int16_t)top, (int16_t)(L.W - rx - m), (int16_t)heroH };
    L.trend = { (int16_t)m, (int16_t)(L.hero.b() + m), (int16_t)(L.W - 2 * m), (int16_t)(bottom - (L.hero.b() + m)) };
    L.tilesVertical = true;
  } else {                                 // Guition landscape 480x272
    int heroW = 196;
    L.hero  = { (int16_t)m, (int16_t)top, (int16_t)heroW, (int16_t)(bottom - top) };
    int rx = m + heroW + m;
    L.tiles = { (int16_t)rx, (int16_t)top, (int16_t)(L.W - rx - m), 72 };
    L.trend = { (int16_t)rx, (int16_t)(L.tiles.b() + m), (int16_t)(L.W - rx - m), (int16_t)(bottom - (L.tiles.b() + m)) };
    L.tilesVertical = false;
  }

  // gauge inside the hero card, status text underneath
  int capL = -L.fL->glyph['H' - L.fL->first].yOffset;
  int statusH = capL + (L.big ? 22 : 14);
  int gaugeH = L.hero.h - statusH - (L.big ? 8 : 4);
  L.gr = min((int)L.hero.w - 16, gaugeH) / 2;
  L.gt = max(7, L.gr / 6);
  L.gx = L.hero.x + L.hero.w / 2;
  L.gy = L.hero.y + (L.big ? 8 : 4) + gaugeH / 2 + (L.big ? 6 : 4);
  L.statusY = L.hero.b() - statusH + (L.big ? 4 : 2);
}

// =======================================================================================
// PROTOTYPES
// =======================================================================================
void initBacklight();
void setBacklight(uint8_t v);
void updateBacklight();
bool isNightNow();
void initStatusLed();
void applyStatusLed();
void initTouch();
bool touchPressedRaw();
void handleTouch();
void showBoot();
void bootProgress(int pct);
void showConnecting();
void showNeedSetup();
void pollSerial();
void startWiFi();
void updateWiFiState();
void runInternetTest();
void calculateStatus();
void addTrendSample();
void logSample();
void updateTimeString();
void drawPage();
void drawHeader(const char *title);
void drawClock();
void drawHomeStatic();
void drawHomeDynamic();
void drawFooter();
void drawHistoryPage(const RangeDef &R);
void switchPage();
uint16_t levelColor(Level lv);
uint16_t healthColor(int v);
uint16_t healthDim(int v);
uint16_t pingColor(long ms);
uint16_t jitterColor(long ms);
void updateJitter();
void updateLoss();
uint16_t lossColor(float pct);
void startWatchdog();
void drawDevicePage();
void openSettings();
void closeSettings();
void settingsNext();
void settingsActivate();
void drawSettings();
void drawSettingsHold(int pct);
int runUpdateCheck(bool manual);
bool overlayOpen();
void updateCheckDue();
void openModal();
void closeModal();
void drawModal();
void drawModalHold(int pct);
void modalConfirm();
void drawUpdBadge();
int updBadgeW();
void updProgress(int pct);
String fmtDuration(uint32_t s);
void addEvent(EvType type, const char *detail, uint32_t dur, uint32_t whenT, uint32_t whenMs);
void addEvent(EvType type, const char *detail);
void trackOutage();
void flushEvents();
void sdEventRows();
void saveEvents();
void loadEvents();
void checkAccessPoint();
void checkWiFiEvents();
String resetReasonName();
String ipStr(uint32_t ip4);
String fmtLocal(uint32_t t);
void idleWork();
void drawEventsPage();
Event &evAt(int i);
uint32_t evTime(const Event &e);
void onWiFiEvent(WiFiEvent_t ev, WiFiEventInfo_t info);
uint16_t evColor(uint8_t t);
void sdLogTest();
bool sdReady();
bool sdAppend(const char *path, const char *header, const String &row);
void findIspHop(const IPAddress &target);
void checkPublicIp();
bool resolveHost(const char *h, IPAddress &ip);
void jitterPush(long us);
bool sdMount();
void sdLogTest();
void drawSdBadge();
void spiBusBegin();
uint16_t rssiColor(int rssi);
String wifiStatusName(wl_status_t st);
String qualityFromRSSI(int rssi);
String fmtLoss(float pct);

// =======================================================================================
// TEXT HELPERS (anti-aliased fonts, see fonts.h)
//   Each string is rendered into a small RAM buffer, blending the 4-bit glyph edges onto
//   the colour behind the text (textBg), then pushed to the screen in one go. Set textBg
//   to whatever the text sits on (card, header, page background) before drawing.
// =======================================================================================
uint16_t textBg = 0;

static inline const AAGlyph *glyphOf(const AAFont *f, char c) {
  if ((uint8_t)c < f->first || (uint8_t)c > f->last) c = f->first;   // unknown -> space
  return &f->glyph[(uint8_t)c - f->first];
}

int capH(const AAFont *f) {
  char c = ('H' <= f->last) ? 'H' : '0';
  return -glyphOf(f, c)->yOffset;
}

int textW(const String &t, const AAFont *f) {
  int w = 0;                                       // advances are in quarter pixels
  for (unsigned i = 0; i < t.length(); i++) w += glyphOf(f, t.c_str()[i])->xAdvance;
  return (w + 2) >> 2;
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a /*0-15*/) {
  if (a == 0) return bg;
  if (a >= 15) return fg;
  uint32_t a5 = a * 32 / 15;                       // 0-32
  uint32_t b = ((bg & 0xF81F) | ((uint32_t)(bg & 0x07E0) << 16));
  uint32_t f = ((fg & 0xF81F) | ((uint32_t)(fg & 0x07E0) << 16));
  uint32_t r = (b + (((f - b) * a5) >> 5)) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}

// y is the TOP of the capital letters (easier to lay out than the baseline)
int drawText(int x, int y, const String &t, const AAFont *f, uint16_t color, Align al) {
  const char *s = t.c_str();
  int n = t.length();
  int w = textW(t, f);
  if (al == AL_CENTER) x -= w / 2;
  else if (al == AL_RIGHT) x -= w;
  if (!n) return 0;
  const int base = y + capH(f);

  // ink bounding box of the whole string (pen in quarter pixels)
  int x0 = 32767, x1 = -32768, y0 = 32767, y1 = -32768, penQ = 0;
  for (int i = 0; i < n; i++) {
    const AAGlyph *g = glyphOf(f, s[i]);
    int px = x + ((penQ + 2) >> 2);
    if (g->width && g->height) {
      x0 = min(x0, px + g->xOffset);             x1 = max(x1, px + g->xOffset + g->width);
      y0 = min(y0, base + g->yOffset);           y1 = max(y1, base + g->yOffset + g->height);
    }
    penQ += g->xAdvance;
  }
  if (x1 <= x0 || y1 <= y0) return w;
  // clip to the screen
  x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, (int)gfx->width()); y1 = min(y1, (int)gfx->height());
  if (x1 <= x0 || y1 <= y0) return w;
  const int bw = x1 - x0, bh = y1 - y0;

  static uint16_t *buf = nullptr;
  static int bufCap = 0;
  if (bw * bh > bufCap) {
    free(buf);
    bufCap = bw * bh;
    buf = (uint16_t *)malloc(bufCap * 2);
    if (!buf) { bufCap = 0; return w; }
  }
  for (int i = 0; i < bw * bh; i++) buf[i] = textBg;

  penQ = 0;
  for (int i = 0; i < n; i++) {
    const AAGlyph *g = glyphOf(f, s[i]);
    const uint8_t *bm = f->bitmap + g->bitmapOffset;
    int gx = x + ((penQ + 2) >> 2) + g->xOffset, gy = base + g->yOffset;
    for (int r = 0; r < g->height; r++) {
      int yy = gy + r - y0;
      if (yy < 0 || yy >= bh) continue;
      for (int c = 0; c < g->width; c++) {
        int k = r * g->width + c;
        uint8_t a = (bm[k >> 1] >> ((k & 1) ? 0 : 4)) & 0x0F;
        if (!a) continue;
        int xx = gx + c - x0;
        if (xx < 0 || xx >= bw) continue;
        uint16_t &px = buf[yy * bw + xx];
        px = blend565(px, color, a);
      }
    }
    penQ += g->xAdvance;
  }
  gfx->draw16bitRGBBitmap(x0, y0, buf, bw, bh);
  return w;
}

// largest of the given fonts that fits maxW
const AAFont *fitFont(const String &t, int maxW, const AAFont *a, const AAFont *b, const AAFont *c) {
  if (textW(t, a) <= maxW) return a;
  if (!c || textW(t, b) <= maxW) return b;
  return c;
}

String fitText(String t, int maxW, const AAFont *f) {
  if (textW(t, f) <= maxW) return t;
  while (t.length() > 1 && textW(t + "..", f) > maxW) t = t.substring(0, t.length() - 1);
  return t + "..";
}

// Panel with two cut corners (top-left, bottom-right), a thin frame and neon on the cuts
void panel(int x, int y, int w, int h, uint16_t fill, uint16_t accent, uint16_t outside) {
  int c = min(L.big ? 9 : 7, min(w, h) / 3);
  gfx->fillRect(x, y, w, h, fill);
  gfx->fillTriangle(x, y, x + c, y, x, y + c, outside);
  gfx->fillTriangle(x + w - 1, y + h - 1, x + w - 1 - c, y + h - 1, x + w - 1, y + h - 1 - c, outside);
  gfx->drawFastHLine(x + c, y, w - c, C_LINE);
  gfx->drawFastVLine(x + w - 1, y, h - c, C_LINE);
  gfx->drawFastHLine(x, y + h - 1, w - c, C_LINE);
  gfx->drawFastVLine(x, y + c, h - c, C_LINE);
  gfx->drawLine(x, y + c, x + c, y, accent);
  gfx->drawLine(x + w - 1 - c, y + h - 1, x + w - 1, y + h - 1 - c, accent);
  gfx->drawFastHLine(x + c, y, min(w / 4, L.big ? 28 : 20), accent);         // neon tick on the top edge
}
void card(const Box &b, uint16_t fill, uint16_t accent) { panel(b.x, b.y, b.w, b.h, fill, accent, C_BG); }
void card(const Box &b, uint16_t fill) { card(b, fill, C_CYAN); }
void card(const Box &b) { card(b, C_CARD, C_CYAN); }
int drawText(int x, int y, const String &t, const AAFont *f, uint16_t color) { return drawText(x, y, t, f, color, AL_LEFT); }

// thick arc from angle a0 to a1 (degrees, 0 = 3 o'clock, clockwise)
void fillArc(int cx, int cy, int rOut, int thick, float a0, float a1, uint16_t col) {
  if (a1 <= a0) return;
  const float D = 0.01745329f;
  int rIn = rOut - thick;
  float step = rOut > 60 ? 2.0f : 3.0f;
  for (float a = a0; a < a1; a += step) {
    float b = min(a + step + 0.6f, a1);
    float ca = cosf(a * D), sa = sinf(a * D), cb = cosf(b * D), sb = sinf(b * D);
    int ax = cx + rOut * ca, ay = cy + rOut * sa, bx = cx + rOut * cb, by = cy + rOut * sb;
    int ix = cx + rIn * ca,  iy = cy + rIn * sa,  jx = cx + rIn * cb,  jy = cy + rIn * sb;
    gfx->fillTriangle(ax, ay, bx, by, ix, iy, col);
    gfx->fillTriangle(bx, by, jx, jy, ix, iy, col);
  }
}

void arcCap(int cx, int cy, int rOut, int thick, float a, uint16_t col) {
  const float D = 0.01745329f;
  float rm = rOut - thick / 2.0f;
  gfx->fillCircle(cx + rm * cosf(a * D), cy + rm * sinf(a * D), thick / 2, col);
}

// =======================================================================================
// PWM (works on ESP32 Arduino core 2.x and 3.x)
// =======================================================================================
#define PWM_FREQ 5000
#define PWM_RES  8
void pwmInit(uint8_t pin, uint8_t ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcAttach(pin, PWM_FREQ, PWM_RES);
#else
  ledcSetup(ch, PWM_FREQ, PWM_RES);
  ledcAttachPin(pin, ch);
#endif
}
void pwmWrite(uint8_t pin, uint8_t ch, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(ch, duty);
#endif
}

// =======================================================================================
// SETUP / LOOP
// =======================================================================================
void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);          // never stall when running from a USB charger with no computer
#endif
  delay(100);
  logMsg("\nNetPulse " FW_VERSION " - " BOARD_NAME "\n");

  pinMode(SD_CS_PIN, OUTPUT);       // keep the card deselected until we use it
  digitalWrite(SD_CS_PIN, HIGH);

  loadSettings();
  logMsg("IMON READY board=" BOARD_NAME " ver=" FW_VERSION "\n");

  gfx = createDisplay(cfgRot);
  if (!gfx->begin(DISPLAY_SPEED)) logMsg("Display init failed!\n");
  gfx->setTextWrap(false);
  gfx->fillScreen(C_BG);
  computeLayout();
  logMsg("Screen %d x %d\n", L.W, L.H);

  initBacklight();
  initTouch();
  initStatusLed();

  showBoot();
  bootProgress(15);
  fsOK = LittleFS.begin(true);       // formats the data partition on first boot
  if (!fsOK) logMsg("LittleFS failed - history kept in RAM only\n");
  bootProgress(45);
  // history files from before v2.3 (one big file each) - no longer used
  if (fsOK) for (const char *old : { "/log_1m.bin", "/log_15m.bin" }) if (LittleFS.exists(old)) LittleFS.remove(old);
  log1m.begin();
  log15m.begin();
  loadEvents();
  addEvent(EV_BOOT, resetReasonName().c_str());
  {
    Preferences prefs;
    prefs.begin("imon", false);
    String last = prefs.isKey("fwver") ? prefs.getString("fwver", "").c_str() : "";
    if (last != FW_VERSION) {
      if (last.length()) addEvent(EV_FIRMWARE, (last + " > " FW_VERSION).c_str());
      prefs.putString("fwver", FW_VERSION);
    }
    updSkip = prefs.isKey("skipver") ? prefs.getString("skipver", "").c_str() : "";
    prefs.end();
  }
  bootProgress(70);
  if (setSdLog) sdMount();
  bootProgress(100);
  delay(500);

  // No Wi-Fi saved yet: wait here for the web flasher to send settings
  if (cfgSsid.length() == 0) {
    showNeedSetup();
    while (true) { pollSerial(); delay(20); }
  }

  WiFi.onEvent(onWiFiEvent);
  startWiFi();
  showConnecting();
  for (int i = 0; i < 300 && WiFi.status() != WL_CONNECTED; i++) { pollSerial(); delay(50); }

  configTzTime(cfgTz.c_str(), NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  // give NTP a moment so the first samples are logged with the right time
  for (int i = 0; i < 40 && WiFi.status() == WL_CONNECTED && !timeValid(); i++) { pollSerial(); delay(50); }

  updateWiFiState();
  if (wifiOK) runInternetTest();
  else { calculateStatus(); addTrendSample(); }
  applyStatusLed();

  drawPage();
  lastScreenUpdate = lastInternetTest = lastWifiRetry = millis();
  startWatchdog();
}

// If the main loop ever stops for WATCHDOG_S seconds, restart the board instead of sitting
// frozen. History is on flash, so a restart only loses the current minute.
void startWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t c = { .timeout_ms = WATCHDOG_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&c);
#else
  esp_task_wdt_init(WATCHDOG_S, true);
#endif
  esp_task_wdt_add(NULL);
  wdtStarted = true;
}

void loop() {
  esp_task_wdt_reset();
  pollSerial();
  handleTouch();

  static unsigned long lastBrightCheck = 0;
  if (millis() - lastBrightCheck >= 1000) {
    lastBrightCheck = millis();
    updateBacklight();
  }

  if (!wifiOK && millis() - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    logMsg("Retry WiFi...\n");
    WiFi.disconnect(false);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  }

  // Internet test
  if (millis() - lastInternetTest >= TEST_INTERVAL_MS) {
    lastInternetTest = millis();
    runInternetTest();
    applyStatusLed();
    updateCheckDue();
    if (overlayOpen()) {}
    else if (currentPage == PAGE_HOME) drawHomeDynamic();
    else if (currentPage == PAGE_DEVICE) pageDirty = true;
    else if ((currentPage == PAGE_24H && dirty1m) || ((currentPage == PAGE_7D || currentPage == PAGE_14D) && dirty15m)) pageDirty = true;
  }

  // Every second: clock, Wi-Fi state change
  if (millis() - lastScreenUpdate >= 1000) {
    lastScreenUpdate = millis();
    checkWiFiEvents();
    bool wasOK = wifiOK;
    String prevTime = timeText;
    updateWiFiState();
    if (wasOK != wifiOK) {
      if (!wifiOK) calculateStatus();
      applyStatusLed();
      if (currentPage == PAGE_HOME && !overlayOpen()) drawHomeDynamic();
    }
    updateTimeString();
    if (timeText != prevTime) {
      if (!overlayOpen()) {
        drawClock();
        if (currentPage == PAGE_HOME) drawFooter();
      }
    }
  }

  if (modalOpen && millis() - modalOpenedMs > 60000UL) closeModal();   // no answer: ask again later
  if (settingsOpen && millis() - settingsTouchedMs > 60000UL) closeSettings();
  if (pageDirty && !overlayOpen()) drawPage();
  delay(5);
}

bool pageShown = false;          // a dashboard page (with header) is on screen

void drawPage() {
  pageDirty = false;
  pageShown = true;
  if (currentPage == PAGE_HOME) { drawHomeStatic(); drawHomeDynamic(); }
  else if (currentPage == PAGE_EVENTS) drawEventsPage();
  else if (currentPage == PAGE_DEVICE) drawDevicePage();
  else {
    drawHistoryPage(RANGES[currentPage - 1]);
    if (currentPage == PAGE_24H) dirty1m = false; else dirty15m = false;
  }
}

void switchPage() {
  currentPage = (Page)((currentPage + 1) % PAGE_COUNT);
  drawPage();
}

// called after every internet test
void logSample() {
  if (!timeValid()) return;                 // no clock yet -> can't place it in time
  uint32_t now = (uint32_t)time(nullptr);
  Sample sm;
  sm.q = (!wifiOK || !internetOK) ? 0 : health;
  sm.up = wifiOK && internetOK;
  sm.ping = (int)latencyMs;
  sm.pingMax = (int)pingMaxMs;
  sm.sent = wifiOK ? pingSent : 0;
  sm.lost = wifiOK ? pingLost : 0;
  sm.jit = (int)jitterMs;
  sm.gw = (int)gwMs;
  sm.p2 = (int)ping2Ms;
  sm.isp = (int)ispMs;
  sm.dns = (int)dnsMs;
  if (log1m.add(now, sm)) dirty1m = true;
  if (log15m.add(now, sm)) dirty15m = true;
}

// =======================================================================================
// BACKLIGHT + NIGHT MODE
// =======================================================================================
void initBacklight() {
  pwmInit(TFT_BL, 0);
  setBacklight(dayBrightness());
}

void setBacklight(uint8_t v) {
  if (v == currentBrightness) return;
  currentBrightness = v;
  pwmWrite(TFT_BL, 0, v);
}

bool isNightNow() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return false;      // no time yet -> treat as day
  if (setNight == 0) return false;                // night dimming off
  int h = ti.tm_hour, s = NIGHT_PRESET[setNight][0], e = NIGHT_PRESET[setNight][1];
  if (s > e) return (h >= s || h < e);
  return (h >= s && h < e);
}

void updateBacklight() {
  bool awake = (millis() - touchWakeMillis < WAKE_DURATION_MS) && touchWakeMillis != 0;
  bool dim = isNightNow() && !awake;
  setBacklight(dim ? BRIGHT_NIGHT : dayBrightness());
  if (dim != nightDimmed) {
    nightDimmed = dim;
    applyStatusLed();
  }
}

// =======================================================================================
// REAR RGB LED (CYD only, active LOW)
// =======================================================================================
void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
#if defined(HAS_RGB_LED)
  r = (uint16_t)r * LED_BRIGHTNESS / 255;
  g = (uint16_t)g * LED_BRIGHTNESS / 255;
  b = (uint16_t)b * LED_BRIGHTNESS / 255;
  pwmWrite(LED_R_PIN, 1, 255 - r);   // active LOW -> invert
  pwmWrite(LED_G_PIN, 2, 255 - g);
  pwmWrite(LED_B_PIN, 3, 255 - b);
#else
  (void)r; (void)g; (void)b;
#endif
}

void initStatusLed() {
#if defined(HAS_RGB_LED)
  pwmInit(LED_R_PIN, 1);
  pwmInit(LED_G_PIN, 2);
  pwmInit(LED_B_PIN, 3);
  ledWrite(0, 0, 0);
#endif
}

void applyStatusLed() {
#if defined(HAS_RGB_LED)
  if (!setLed || (NIGHT_LED_OFF && nightDimmed)) { ledWrite(0, 0, 0); return; }
  switch (level) {
    case LV_GOOD: ledWrite(0, 255, 0); break;                 // green
    case LV_WARN: ledWrite(LED_YELLOW_RED, 255, 0); break;    // yellow
    default:      ledWrite(255, 0, 0); break;                 // red
  }
#endif
}

// =======================================================================================
// TOUCH  (only "is it pressed" is needed: tap anywhere switches page)
// =======================================================================================
#if defined(TOUCH_XPT2046)
#if defined(TOUCH_SHARES_SD_BUS)
// Touch chip on the microSD bus: hardware SPI at 2 MHz, its own chip-select
uint16_t xptRead12(uint8_t cmd) {
  sdSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(XPT_CS, LOW);
  sdSPI.transfer(cmd);
  uint8_t hi = sdSPI.transfer(0x00);
  uint8_t lo = sdSPI.transfer(0x00);
  digitalWrite(XPT_CS, HIGH);
  sdSPI.endTransaction();
  return (((uint16_t)hi << 8) | lo) >> 3;
}
#else
uint8_t xptTransfer(uint8_t data) {
  uint8_t out = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(XPT_MOSI, (data & (1 << i)) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(XPT_SCK, HIGH);
    delayMicroseconds(1);
    out <<= 1;
    if (digitalRead(XPT_MISO)) out |= 1;
    digitalWrite(XPT_SCK, LOW);
    delayMicroseconds(1);
  }
  return out;
}

uint16_t xptRead12(uint8_t cmd) {
  digitalWrite(XPT_CS, LOW);
  delayMicroseconds(2);
  xptTransfer(cmd);
  uint8_t hi = xptTransfer(0x00);
  uint8_t lo = xptTransfer(0x00);
  digitalWrite(XPT_CS, HIGH);
  return (((uint16_t)hi << 8) | lo) >> 3;
}
#endif   // TOUCH_SHARES_SD_BUS
#endif

#if defined(TOUCH_GT911)
uint8_t gtAddr = 0;

bool gtWriteReg(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool gtReadReg(uint16_t reg, uint8_t &val) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)gtAddr, 1) != 1) return false;
  val = Wire.read();
  return true;
}
#endif

void initTouch() {
#if defined(TOUCH_XPT2046) && defined(TOUCH_SHARES_SD_BUS)
  pinMode(XPT_CS, OUTPUT);
  digitalWrite(XPT_CS, HIGH);
  spiBusBegin();
  logMsg("Touch: XPT2046 (shared SPI with microSD)\n");
#elif defined(TOUCH_XPT2046)
  pinMode(XPT_CS, OUTPUT);
  pinMode(XPT_SCK, OUTPUT);
  pinMode(XPT_MOSI, OUTPUT);
  pinMode(XPT_MISO, INPUT);
  digitalWrite(XPT_CS, HIGH);
  digitalWrite(XPT_SCK, LOW);
  digitalWrite(XPT_MOSI, LOW);
  logMsg("Touch: XPT2046 (bit-bang)\n");
#elif defined(TOUCH_GT911)
  pinMode(GT_INT, OUTPUT);
  pinMode(GT_RST, OUTPUT);
  digitalWrite(GT_INT, LOW);          // INT low during reset -> address 0x5D
  digitalWrite(GT_RST, LOW);
  delay(10);
  digitalWrite(GT_RST, HIGH);
  delay(10);
  pinMode(GT_INT, INPUT);
  delay(60);
  Wire.begin(GT_SDA, GT_SCL, 400000);
  const uint8_t addrs[2] = { 0x5D, 0x14 };
  for (uint8_t a : addrs) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { gtAddr = a; break; }
  }
  logMsg("Touch: GT911 at 0x%02X\n", gtAddr);
#endif
}

bool touchPressedRaw() {
#if defined(TOUCH_XPT2046)
  // Pressure + position sanity check: rejects the 0 / 4095 ghost values
  uint16_t z1 = xptRead12(0xB0);
  uint16_t z2 = xptRead12(0xC0);
  int z = (int)z1 + 4095 - (int)z2;
  xptRead12(0xD0);                    // first read after channel switch is noisy
  uint16_t x = xptRead12(0xD0);
  xptRead12(0x90);
  uint16_t y = xptRead12(0x90);
  if (z < 300) return false;
  if (x < 120 || x > 3950) return false;
  if (y < 120 || y > 3950) return false;
  return true;
#elif defined(TOUCH_GT911)
  static bool lastState = false;
  static unsigned long lastPoll = 0;
  if (!gtAddr) return false;
  if (millis() - lastPoll < 15) return lastState;
  lastPoll = millis();
  uint8_t st;
  if (!gtReadReg(0x814E, st)) return lastState;
  if (st & 0x80) {                    // new report ready
    lastState = (st & 0x0F) > 0;
    gtWriteReg(0x814E, 0);
  }
  return lastState;
#else
  return false;
#endif
}

// Tap (press + release) = next page, or switch choice in the update prompt.
// Hold (TOUCH_HOLD_MS) = confirm in the update prompt, or open it when an update is waiting.
void handleTouch() {
  static unsigned long pressStart = 0;
  static bool holdFired = false, wakeOnly = false;
  static int lastHoldPct = -1;
  unsigned long now = millis();
  if (touchPressedRaw()) {
    lastTouchSeenMs = now;
    if (!touchLatched) {
      if (now - lastTouchMs < TOUCH_DEBOUNCE_MS) return;
      touchLatched = true;
      lastTouchMs = now;
      pressStart = now;
      holdFired = false;
      lastHoldPct = -1;
      wakeOnly = nightDimmed;                  // first tap at night only wakes the screen
      touchWakeMillis = now;
      updateBacklight();
      return;
    }
    if (wakeOnly || holdFired) return;
    unsigned long held = now - pressStart;
    if (overlayOpen() && held > 200) {         // show the hold filling up
      int pct = (int)min(100UL, (held - 200) * 100 / (TOUCH_HOLD_MS - 200));
      if (pct != lastHoldPct) { lastHoldPct = pct; if (settingsOpen) drawSettingsHold(pct); else drawModalHold(pct); }
    }
    if (held >= TOUCH_HOLD_MS) {
      holdFired = true;
      if (settingsOpen) settingsActivate();
      else if (modalOpen) modalConfirm();
      else if (currentPage == PAGE_DEVICE) openSettings();
      else if (updAvail) openModal();
    }
    return;
  }
  if (touchLatched && now - lastTouchSeenMs > TOUCH_RELEASE_MS) {
    touchLatched = false;
    lastTouchMs = now;
    if (wakeOnly || holdFired) return;
    if (lastTouchSeenMs - pressStart >= TOUCH_HOLD_MS) return;
    if (settingsOpen) settingsNext();
    else if (modalOpen) { modalSel ^= 1; modalOpenedMs = now; drawModal(); }
    else switchPage();
  }
}

// =======================================================================================
// BOOT / SETUP / CONNECTING SCREENS
// =======================================================================================
void drawPulseIcon(int cx, int cy, int s, uint16_t col) {
  // little heartbeat line: s = half width
  int pts[][2] = { {-s, 0}, {-s / 2, 0}, {-s / 4, -s * 2 / 3}, {s / 8, s * 2 / 3}, {s * 3 / 8, -s / 3}, {s / 2, 0}, {s, 0} };
  for (int i = 0; i < 6; i++)
    for (int t = -1; t <= 1; t++)
      gfx->drawLine(cx + pts[i][0], cy + pts[i][1] + t, cx + pts[i + 1][0], cy + pts[i + 1][1] + t, col);
}

static int16_t bootBarX, bootBarY, bootBarW, bootShown;

void showBoot() {
  gfx->fillScreen(C_BG);
  textBg = C_BG;
  const int cx = L.W / 2;
  int iconR = L.big ? 34 : 24;
  int blockH = iconR * 2 + 14 + capH(L.fL) + 10 + capH(L.fS) + 26 + capH(L.fXS) * 3 + 24;
  int y = (L.H - blockH) / 2;

  gfx->fillCircle(cx, y + iconR, iconR, C_CYAN_D);
  gfx->drawCircle(cx, y + iconR, iconR, C_CYAN);
  drawPulseIcon(cx, y + iconR, iconR * 2 / 3, C_CYAN);
  y += iconR * 2 + 14;

  drawText(cx, y, "NETPULSE", L.fL, C_WHITE, AL_CENTER);   y += capH(L.fL) + 10;
  drawText(cx, y, "// NETWORK HEALTH LOGGER", L.fS, C_PINK, AL_CENTER);
  y += capH(L.fS) + 26;

  drawText(cx, y, "Original design by Francesco Landi", L.fXS, C_CYAN, AL_CENTER); y += capH(L.fXS) + 8;
  drawText(cx, y, FW_VERSION "   " BOARD_NAME, L.fXS, C_DIM, AL_CENTER);    y += capH(L.fXS) + 22;

  bootBarW = min(220, L.W * 6 / 10);
  bootBarX = cx - bootBarW / 2;
  bootBarY = y;
  bootShown = 0;
  gfx->fillRect(bootBarX, bootBarY, bootBarW, 6, C_TRACK);
  bootProgress(0);
}

void bootProgress(int pct) {
  int target = bootBarW * pct / 100;
  for (; bootShown <= target; bootShown += 4) {
    gfx->fillRect(bootBarX, bootBarY, max(6, (int)bootShown), 6, C_CYAN);
    delay(8);
  }
}

void showNeedSetup() {
  gfx->fillScreen(C_BG);
  textBg = C_BG;
  const int cx = L.W / 2;
  int y = L.land ? 10 : L.H / 6;
  int r = L.land ? (L.big ? 22 : 16) : (L.big ? 28 : 20);
  gfx->fillCircle(cx, y + r, r, C_YELL_D);
  gfx->drawCircle(cx, y + r, r, C_YELLOW);
  // simple Wi-Fi symbol
  for (int i = 0; i < 3; i++) fillArc(cx, y + r + r / 2, r * (3 + i * 2) / 10 + 3, 3, 225, 315, C_YELLOW);
  gfx->fillCircle(cx, y + r + r / 2 - 1, 3, C_YELLOW);
  y += 2 * r + (L.land ? 10 : 14);
  drawText(cx, y, "Wi-Fi setup", L.fL, C_WHITE, AL_CENTER);  y += capH(L.fL) + (L.land ? 8 : 12);
  drawText(cx, y, "No network saved yet", L.fS, C_YELLOW, AL_CENTER);  y += capH(L.fS) + (L.land ? 14 : (L.big ? 26 : 18));

  const char *steps[] = { "1  Keep the USB cable plugged in", "2  Open the web flasher (Chrome/Edge)",
                          "3  Connect, then Settings", "4  Enter Wi-Fi and Save to board" };
  const AAFont *sf = L.fS;
  int w = 0;
  for (auto s : steps) w = max(w, textW(s, sf));
  if (w > L.W - 16) { sf = L.fXS; w = 0; for (auto s : steps) w = max(w, textW(s, sf)); }
  int x = cx - w / 2;
  for (auto s : steps) { drawText(x, y, s, sf, C_GREY); y += capH(sf) + (L.land ? 8 : (L.big ? 14 : 10)); }
  drawText(cx, L.H - capH(L.fXS) - 10, BOARD_NAME "   " FW_VERSION, L.fXS, C_DIM, AL_CENTER);
}

void showConnecting() {
  gfx->fillScreen(C_BG);
  textBg = C_BG;
  const int cx = L.W / 2, cy = L.H / 2 - 20;
  int r = L.big ? 26 : 20;
  fillArc(cx, cy, r, 5, 0, 360, C_TRACK);
  fillArc(cx, cy, r, 5, 270, 360, C_CYAN);
  drawText(cx, cy + r + 18, "Connecting to Wi-Fi", L.fM, C_WHITE, AL_CENTER);
  drawText(cx, cy + r + 18 + capH(L.fM) + 10, fitText(cfgSsid, L.W - 20, L.fS), L.fS, C_GREY, AL_CENTER);
}

// =======================================================================================
// HEADER / FOOTER
// =======================================================================================
int sdBadgeW() { return SD_LOG ? textW("SD", L.fXS) + 12 : 0; }
int updBadgeW() { return textW("UPD", L.fXS) + 12; }
int clockLeft() { return L.W - 10 - textW("00:00", L.fM) - (SD_LOG ? sdBadgeW() + 8 : 0) - (updAvail ? updBadgeW() + 6 : 0); }

// small "SD" pill left of the clock: green = logging to the card, red = card stopped working
void drawSdBadge() {
#if SD_LOG
  if (!gfx || !pageShown || overlayOpen()) return;
  drawUpdBadge();
  int w = sdBadgeW(), h = capH(L.fXS) + 8;
  int x = L.W - 10 - textW("00:00", L.fM) - 8 - w, y = (L.hdrH - h) / 2;
  gfx->fillRect(x, y, w, h, C_HEADER);
  if (sdState == SD_NONE || !setSdLog) return;
  uint16_t c = sdState == SD_OK ? C_GREEN : C_RED;
  uint16_t bg = sdState == SD_OK ? C_GREEN_D : C_RED_D;
  gfx->fillRect(x, y, w, h, bg);
  gfx->drawRect(x, y, w, h, c);
  textBg = bg;
  drawText(x + w / 2, y + 4, "SD", L.fXS, c, AL_CENTER);
  textBg = C_HEADER;
#endif
}

void drawHeader(const char *title) {
  gfx->fillRect(0, 0, L.W, L.hdrH, C_HEADER);
  gfx->drawFastHLine(0, L.hdrH - 1, L.W, C_LINE);
  gfx->drawFastHLine(0, L.hdrH - 1, L.W / 3, C_PINK);            // neon underline
  gfx->drawFastHLine(L.W / 3, L.hdrH - 1, 18, C_CYAN);
  textBg = C_HEADER;
  int ty = (L.hdrH - capH(L.fM)) / 2;
  // status block + "// TITLE"
  int dr = L.big ? 5 : 4;
  gfx->fillRect(10, L.hdrH / 2 - dr, dr * 2, dr * 2, levelColor(level));
  int tx = 10 + dr * 2 + 8;
  tx += drawText(tx, ty, "//", L.fM, C_PINK) + (L.big ? 6 : 4);
  const AAFont *tf = textW(title, L.fM) <= clockLeft() - 10 - tx ? L.fM : L.fS;   // long title: smaller
  drawText(tx, ty + (capH(L.fM) - capH(tf)) / 2, title, tf, C_WHITE);

  // page dots between title and clock
  int dots = PAGE_COUNT, ds = L.big ? 12 : 9, rr = L.big ? 3 : 2;
  int dx = clockLeft() - 14 - (dots - 1) * ds;
  int minX = tx + textW(title, tf) + 12;
  if (dx > minX) {
    for (int i = 0; i < dots; i++) {
      if (i == currentPage) gfx->fillRect(dx + i * ds - rr - 1, L.hdrH / 2 - rr, rr * 2 + 3, rr * 2 + 1, C_CYAN);
      else gfx->fillRect(dx + i * ds - rr / 2 - 1, L.hdrH / 2 - rr / 2 - 1, rr + 2, rr + 2, C_DIM);
    }
  }
  drawClock();
}

void drawClock() {
  int x = L.W - 10 - textW("00:00", L.fM) - 2;
  gfx->fillRect(x, 1, L.W - x, L.hdrH - 3, C_HEADER);
  textBg = C_HEADER;
  drawSdBadge();
  textBg = C_HEADER;
  drawText(L.W - 10, (L.hdrH - capH(L.fM)) / 2, timeText, L.fM, C_CYAN, AL_RIGHT);
  // keep the status block current
  int dr = L.big ? 5 : 4;
  gfx->fillRect(10, L.hdrH / 2 - dr, dr * 2, dr * 2, levelColor(level));
}

// Footer: the path out of your house, hop by hop - router > ISP > DNS - plus board uptime
void drawFooter() {
  if (currentPage != PAGE_HOME) return;
  gfx->fillRect(0, L.footY, L.W, L.footH, C_BG);
  textBg = C_BG;
  int y = L.footY + (L.footH - capH(L.fXS)) / 2;
  unsigned long up = millis() / 1000;
  char buf[24];
  if (up < 3600) snprintf(buf, sizeof(buf), "UP %lum", up / 60);
  else if (up < 86400) snprintf(buf, sizeof(buf), "UP %luh%02lum", up / 3600, (up / 60) % 60);
  else snprintf(buf, sizeof(buf), "UP %lud%luh", up / 86400, (up / 3600) % 24);
  int rw = drawText(L.W - L.m - 2, y, buf, L.fXS, C_DIM, AL_RIGHT);
  const int xMax = L.W - L.m - 2 - rw - 8;
  int x = L.m + 2;
  struct Hop { const char *n; long ms; uint16_t c; };
  Hop hops[3] = { { "RTR", gwMs, C_ROUTER }, { "ISP", ispMs, C_ISP }, { "DNS", dnsMs, C_GREY } };
  for (int i = 0; i < 3; i++) {
    String v = hops[i].ms >= 0 ? String(hops[i].ms) : String("--");
    int need = textW(String(hops[i].n) + " " + v + (i ? "  " : ""), L.fXS);
    if (x + need > xMax) break;
    if (i) x += textW("  ", L.fXS);
    x += drawText(x, y, String(hops[i].n) + " ", L.fXS, C_DIM);
    x += drawText(x, y, v, L.fXS, hops[i].ms < 0 ? C_DIM : (hops[i].ms > PING_BAD_MS ? C_RED : (hops[i].ms > PING_WARN_MS ? C_YELLOW : hops[i].c)));
  }
}

// =======================================================================================
// HOME PAGE
// =======================================================================================
void drawHomeStatic() {
  gfx->fillScreen(C_BG);
  drawHeader("LIVE");
  card(L.hero);
  card(L.trend);
  drawFooter();
}

void drawGauge() {
  const int cx = L.gx, cy = L.gy, r = L.gr, t = L.gt;
  const float A0 = 135, SWEEP = 270;
  uint16_t c = levelColor(level);
  // inner clear
  gfx->fillCircle(cx, cy, r - t - 4, C_CARD);
  textBg = C_CARD;
  // segmented neon ring: 30 segments, lit up to the health value
  const int SEG = 30;
  const float segA = SWEEP / SEG, gap = 2.2f;
  int lit = (health * SEG + 50) / 100;
  if (health > 0 && lit == 0) lit = 1;
  for (int i = 0; i < SEG; i++) {
    float a0 = A0 + i * segA;
    fillArc(cx, cy, r, t, a0, a0 + segA - gap, i < lit ? c : C_TRACK);
  }
  // thin outer + inner guide rings
  fillArc(cx, cy, r + 4, 1, A0, A0 + SWEEP, C_LINE);
  fillArc(cx, cy, r - t - 3, 1, A0, A0 + SWEEP, C_LINE);
  // value
  String v = String(health);
  const AAFont *pf = L.fM;
  if (textW("100", L.fXL) + textW("%", pf) + 10 > 2 * (r - t)) pf = L.fXS;   // small gauge: smaller %
  const AAFont *nf = L.fXL;                                                    // number must clear the ring
  if (textW("100", nf) + 2 + textW("%", pf) > 2 * (r - t - 4) - 12) nf = L.fL;     // must clear the inner ring
  int numW = textW(v, nf), pctW = textW("%", pf);
  int hNum = capH(nf), hLab = capH(L.fXS);
  int top = cy - (hNum + 8 + hLab) / 2;
  int x0 = cx - (numW + 2 + pctW) / 2;
  drawText(x0, top, v, nf, C_WHITE);
  drawText(x0 + numW + 2, top, "%", pf, C_GREY);
  // label must fit inside the inner ring at its height
  int ly = top + hNum + 8 + capH(L.fXS) / 2 - cy, ri = r - t - 6;
  int room = ly < ri ? 2 * (int)sqrtf((float)(ri * ri - ly * ly)) - 6 : 0;
  drawText(cx, top + hNum + 8, textW("NET HEALTH", L.fXS) <= room ? "NET HEALTH" : "HEALTH", L.fXS, C_GREY, AL_CENTER);
}

void drawStatusLine() {
  const Box &B = L.hero;
  int maxW = B.w - 16;
  const AAFont *f = fitFont(statusText, maxW, L.fL, L.fM, L.fS);
  int h = capH(L.fL) + 8;
  gfx->fillRect(B.x + 4, L.statusY - 4, B.w - 8, h + 4, C_CARD);
  textBg = C_CARD;
  drawText(B.x + B.w / 2, L.statusY + (capH(L.fL) - capH(f)) / 2, statusText, f, levelColor(level), AL_CENTER);
}

void drawSignalBars(int x, int y, int h, int rssi) {
  int bars = 0;
  if (wifiOK) {
    if (rssi > -85) bars = 1;
    if (rssi > -75) bars = 2;
    if (rssi > -67) bars = 3;
    if (rssi > -55) bars = 4;
  }
  int bw = max(3, h / 5), gap = max(2, bw / 2);
  for (int i = 0; i < 4; i++) {
    int bh = h * (i + 1) / 4;
    gfx->fillRect(x + i * (bw + gap), y + h - bh, bw, bh, i < bars ? rssiColor(rssi) : C_TRACK);
  }
}

// returns the x where the label ends (used to place the Wi-Fi bars)
int drawTile(const Box &b, const char *label, const char *shortLabel, const String &value, const char *unit, uint16_t accent) {
  card(b, C_CARD, accent);
  textBg = C_CARD;
  gfx->fillRect(b.x + 1, b.y + b.h / 3, 2, b.h / 3, accent);
  int pad = L.big ? 12 : 9;
  if (shortLabel && textW(label, L.fXS) > b.w - pad - 6) label = shortLabel;   // keep it inside the tile
  if (b.w > b.h * 3) {
    // wide + short: label left, value right on one line
    int lw = drawText(b.x + pad, b.y + (b.h - capH(L.fXS)) / 2, label, L.fXS, C_GREY);
    int unitW = unit ? textW(String(" ") + unit, L.fXS) : 0;
    const AAFont *vf = fitFont(value, b.w - 2 * pad - lw - unitW - 40, L.fL, L.fM, L.fS);
    if (capH(vf) > b.h - 12) vf = L.fM;
    int vy = b.y + (b.h - capH(vf)) / 2;
    int xr = b.r() - pad;
    if (unit) xr -= drawText(xr, vy + capH(vf) - capH(L.fXS), unit, L.fXS, C_GREY, AL_RIGHT) + 3;
    drawText(xr, vy, value, vf, accent, AL_RIGHT);
    return b.x + pad + lw;
  }
  drawText(b.x + pad, b.y + (L.big ? 10 : 7), label, L.fXS, C_GREY);
  const AAFont *vf = L.fL;
  int unitW = unit ? textW(String(" ") + unit, L.fXS) : 0;
  if (textW(value, vf) + unitW > b.w - pad - 6) vf = L.fM;
  if (textW(value, vf) + unitW > b.w - pad - 6) vf = L.fS;
  int vy = b.b() - (L.big ? 12 : 8) - capH(vf);
  int w = drawText(b.x + pad, vy, value, vf, accent);
  if (unit) drawText(b.x + pad + w + 3, vy + capH(vf) - capH(L.fXS), unit, L.fXS, C_GREY);
  return -1;
}

void drawTiles() {
  Box t[3];
  const Box &B = L.tiles;
  const int g = L.m;
  for (int i = 0; i < 3; i++) {
    if (L.tilesVertical) t[i] = { B.x, (int16_t)(B.y + i * (B.h + g) / 3), B.w, (int16_t)((B.h - 2 * g) / 3) };
    else                 t[i] = { (int16_t)(B.x + i * (B.w + g) / 3), B.y, (int16_t)((B.w - 2 * g) / 3), B.h };
  }
  // PING
  drawTile(t[0], "PING", nullptr, latencyMs >= 0 ? String(latencyMs) : String("--"), latencyMs >= 0 ? "ms" : nullptr, pingColor(latencyMs));
  // WI-FI
  int lx = drawTile(t[1], "WI-FI", nullptr, wifiOK ? String(wifiRSSI) : String("--"), wifiOK ? "dBm" : nullptr, wifiOK ? rssiColor(wifiRSSI) : C_RED);
  if (lx >= 0) {            // one-line tile: bars right after the label
    int bh = t[1].h / 2;
    drawSignalBars(lx + 8, t[1].y + (t[1].h - bh) / 2, bh, wifiRSSI);
  } else {
    int bh = t[1].h * 2 / 5, bw = 4 * max(3, bh / 5) + 3 * max(2, max(3, bh / 5) / 2);
    drawSignalBars(t[1].r() - bw - (L.big ? 12 : 8), t[1].y + (L.big ? 10 : 7), bh, wifiRSSI);
  }
  // UPTIME (24 h)
  RangeStats st = calcStats(RANGES[0]);
  String up = "--";
  uint16_t uc = C_GREY;
  if (st.slots) {
    float u = st.upPct;
    up = (u >= 99.95f) ? String("100") : String(u, 1);
    uc = u >= 99.5f ? C_GREEN : (u >= 97.0f ? C_YELLOW : C_RED);
  }
  drawTile(t[2], "UPTIME 24H", "UP 24H", up, st.slots ? "%" : nullptr, uc);
}

void drawTrend() {
  const Box &B = L.trend;
  int pad = L.big ? 12 : 9;
  int x = B.x + pad, w = B.w - 2 * pad;
  int ty = B.y + (L.big ? 10 : 7);
  gfx->fillRect(x, ty, w, capH(L.fXS) + 2, C_CARD);
  textBg = C_CARD;
  // loss + jitter on the right, coloured by their thresholds
  const char *title = "LAST 10 MINUTES";
  String jit = jitterMs >= 0 ? String(jitterMs) + " ms" : String("--");
  String los = fmtLoss(lossPct);
  int need = textW("LOSS " + los + "   JITTER " + jit, L.fXS);
  if (textW(title, L.fXS) + 12 + need > w) title = "10 MIN";
  drawText(x, ty, title, L.fXS, C_GREY);
  int xr = x + w;
  xr -= drawText(xr, ty, jit, L.fXS, jitterColor(jitterMs), AL_RIGHT);
  xr -= drawText(xr, ty, "JITTER ", L.fXS, C_DIM, AL_RIGHT);
  xr -= textW("   ", L.fXS);
  xr -= drawText(xr, ty, los, L.fXS, lossColor(lossPct), AL_RIGHT);
  drawText(xr, ty, "LOSS ", L.fXS, C_DIM, AL_RIGHT);

  int gy = ty + capH(L.fXS) + (L.big ? 10 : 7);
  int gh = B.b() - pad + 2 - gy;
  if (gh < 8) return;
  gfx->fillRect(x, gy, w, gh, C_CARD);
  for (int i = 1; i < 4; i++) gfx->drawFastHLine(x, gy + gh * i / 4, w, C_CARD2);
  float colW = (float)w / TREND_SIZE;
  for (int i = 0; i < trendCount; i++) {
    int idx = (trendIndex - trendCount + i + TREND_SIZE) % TREND_SIZE;
    int slot = TREND_SIZE - trendCount + i;
    int cx0 = x + (int)(slot * colW);
    int cx1 = x + (int)((slot + 1) * colW);
    int cw = max(1, cx1 - cx0 - (colW >= 3 ? 1 : 0));
    int v = trendHealth[idx];
    int bh = max(2, gh * v / 100);
    gfx->fillRect(cx0, gy + gh - bh, cw, bh, levelColor((Level)trendLevel[idx]));
  }
}

void drawHomeDynamic() {
  drawGauge();
  drawStatusLine();
  drawTiles();
  drawTrend();
  drawClock();
  drawFooter();
}

// =======================================================================================
// HISTORY PAGES (24 HOURS / 7 DAYS / 14 DAYS)
// =======================================================================================
String fmtDuration(uint32_t s) {
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + " min";
  if (s < 86400) return String(s / 3600) + "h " + String((s / 60) % 60) + "m";
  return String(s / 86400) + "d " + String((s / 3600) % 24) + "h";
}

void dashedH(int x, int y, int w, uint16_t c) {
  for (int i = 0; i < w; i += 6) gfx->drawFastHLine(x + i, y, min(3, w - i), c);
}

void drawHistoryPage(const RangeDef &R) {
  gfx->fillScreen(C_BG);
  drawHeader(R.title);
  const int m = L.m;
  const AAFont *fx = L.fXS;
  const int capX = capH(fx);

  // ---- stats strip ----
  RangeStats st = calcStats(R);
  int sh = capX + capH(L.fM) + (L.big ? 26 : 18);
  Box sb = { (int16_t)m, (int16_t)(L.hdrH + m), (int16_t)(L.W - 2 * m), (int16_t)sh };
  const char *lab[3] = { "UPTIME", "AVG PING", "PACKET LOSS" };
  const char *labShort[3] = { "UP", "PING", "LOSS" };
  String val[3];
  uint16_t col[3];
  if (st.slots) {
    val[0] = (st.upPct >= 99.95f) ? String("100%") : String(st.upPct, st.upPct >= 99 ? 2 : 1) + "%";
    col[0] = st.upPct >= 99.5f ? C_GREEN : (st.upPct >= 97.0f ? C_YELLOW : C_RED);
    val[1] = st.avgPing >= 0 ? String(st.avgPing) + " ms" : String("--");
    col[1] = pingColor(st.avgPing);
    val[2] = st.lossPct >= 0 ? (st.lossPct > 0 && st.lossPct < 0.1f ? String("<0.1%") : fmtLoss(st.lossPct)) : String("--");
    col[2] = lossColor(st.lossPct);
  } else {
    for (int i = 0; i < 3; i++) { val[i] = "--"; col[i] = C_GREY; }
  }
  for (int i = 0; i < 3; i++) {
    Box c = { (int16_t)(sb.x + i * (sb.w + m) / 3), sb.y, (int16_t)((sb.w - 2 * m) / 3), sb.h };
    card(c);
    textBg = C_CARD;
    int pad = L.big ? 10 : 7;
    drawText(c.x + pad, c.y + pad, textW(lab[i], fx) > c.w - 2 * pad ? labShort[i] : lab[i], fx, C_GREY);
    const AAFont *vf = fitFont(val[i], c.w - 2 * pad, L.fM, L.fS, fx);
    drawText(c.x + pad, c.b() - pad - capH(vf), val[i], vf, col[i]);
  }

  // ---- chart geometry ----
  int axisW = textW("150", fx) + 6;
  int gx = m + axisW, gw = L.W - m - gx;
  int timeH = capX + 10;
  int titleH = capX + 8;
  int top = sb.b() + m;
  int avail = L.H - m / 2 - timeH - top - 2 * titleH - m;
  int qh = avail * (L.land ? 50 : 55) / 100, ph = avail - qh;
  int qTitle = top, qy = qTitle + titleH;
  int pTitle = qy + qh + m, py = pTitle + titleH;
  int timeY = py + ph + 6;

  textBg = C_BG;
  drawText(gx, qTitle, "QUALITY", fx, C_GREY);
  if (st.slots) drawText(gx + gw, qTitle, "DOWN " + fmtDuration(st.downSec), fx, st.downSec == 0 ? C_DIM : (st.downSec < 600 ? C_YELLOW : C_RED), AL_RIGHT);
  int ptw = drawText(gx, pTitle, "PING", fx, C_GREY);
  // legend: internet vs ISP router vs your router
  {
    int lx = gx + ptw + 8, ly = pTitle + capX / 2;
    struct Lg { const char *n; uint16_t c; } lg[3] = { { "NET", C_GREEN }, { "ISP", C_ISP }, { "RTR", C_ROUTER } };
    for (int i = 0; i < 3; i++) {
      gfx->fillRect(lx, ly - 1, 8, 2, lg[i].c);
      lx += 11 + drawText(lx + 11, pTitle, lg[i].n, fx, C_DIM) + 7;
    }
    String wst = "WORST " + String(st.worstPing) + " MS";
    if (st.worstPing >= 0 && lx + 6 + textW(wst, fx) < gx + gw) drawText(gx + gw, pTitle, wst, fx, C_DIM, AL_RIGHT);
  }
  if (R.log->count() == 0 && st.slots == 0) {
    panel(gx, qy, gw, qh, C_CARD, C_CYAN, C_BG);
    panel(gx, py, gw, ph, C_CARD, C_PINK, C_BG);
    textBg = C_CARD;
    drawText(gx + gw / 2, qy + qh / 2 - capH(L.fS) / 2, timeValid() ? "COLLECTING DATA..." : "WAITING FOR TIME SYNC...", L.fS, C_GREY, AL_CENTER);
    return;
  }

  // ---- data per pixel column (oldest left, now at the right edge) ----
  const int cols = gw;
  uint32_t nowSlot = timeValid() ? (uint32_t)time(nullptr) / R.log->slotSec() : 0;
  // ping scale: 150 ms, or more if the average is higher
  int pingMax = 150;
  {
    LogRec r;
    int worstAvg = 0;
    for (uint32_t a = 0; a < R.span; a++) if (R.log->recAt(nowSlot - a, r) && r.pAvg != 0xFFFF) worstAvg = max(worstAvg, (int)r.pAvg);
    while (pingMax < worstAvg * 11 / 10 && pingMax < 1000) pingMax += 50;
  }

  panel(gx, qy, gw, qh, C_CARD, C_CYAN, C_BG);
  panel(gx, py, gw, ph, C_CARD, C_PINK, C_BG);
  for (int p = 25; p < 100; p += 25) gfx->drawFastHLine(gx + 1, qy + qh - 1 - (qh - 2) * p / 100, gw - 2, C_CARD2);
  auto pY = [&](int ms) { return py + ph - 2 - (ph - 4) * constrain(ms, 0, pingMax) / pingMax; };
  auto qY = [&](int q) { return qy + qh - 2 - (qh - 8) * q / 100; };

  // time grid first (behind the data)
  const uint32_t spanSec = R.span * R.log->slotSec();
  const bool is24 = spanSec <= 86400UL, is7 = !is24 && spanSec <= 7UL * 86400UL;
  struct Tick { int x; time_t t; };
  Tick ticks[40];
  int nt = 0;
  uint32_t ss = R.log->slotSec();
  time_t tEnd = (time_t)(nowSlot + 1) * ss, tStart = tEnd - (time_t)R.span * ss;
  auto tX = [&](time_t t) { return gx + (int)((int64_t)(t - tStart) * cols / ((int64_t)R.span * ss)); };
  if (timeValid()) {
    struct tm tmv;
    localtime_r(&tStart, &tmv);
    tmv.tm_min = 0; tmv.tm_sec = 0;
    if (is24) { tmv.tm_hour = (tmv.tm_hour / 6) * 6; }
    else { tmv.tm_hour = 0; }
    for (int guard = 0; guard < 60 && nt < 40; guard++) {
      if (is24) tmv.tm_hour += 6; else tmv.tm_mday += 1;
      tmv.tm_isdst = -1;
      time_t t = mktime(&tmv);
      if (t >= tEnd) break;
      if (t <= tStart) continue;
      ticks[nt++] = { tX(t), t };
    }
    for (int i = 0; i < nt; i++) {
      gfx->drawFastVLine(ticks[i].x, qy + 2, qh - 4, C_CARD2);
      gfx->drawFastVLine(ticks[i].x, py + 2, ph - 4, C_CARD2);
    }
  }
  dashedH(gx + 2, pY(PING_WARN_MS), gw - 4, C_YELL_D);
  dashedH(gx + 2, pY(PING_BAD_MS), gw - 4, C_RED_D);

  int prevQx = -1, prevQy = 0, prevPx = -1, prevPy = 0, prevGx = -1, prevGy = 0, prevIx = -1, prevIy = 0;
  int stripH = L.big ? 5 : 4;
  for (int c = 0; c < cols; c++) {
    uint32_t ageHi = (uint32_t)((uint64_t)(cols - c) * R.span / cols);
    uint32_t ageLo = (uint32_t)((uint64_t)(cols - 1 - c) * R.span / cols);
    int n = 0, qSum = 0, upMin = 100, pN = 0, pSum = 0, pMx = -1, gN = 0, gSum = 0, lossMx = -1, iN = 0, iSum = 0;
    LogRec r;
    for (uint32_t a = ageLo; a < max(ageHi, ageLo + 1); a++) {
      if (!R.log->recAt(nowSlot - a, r)) continue;
      n++; qSum += r.qAvg; upMin = min(upMin, (int)r.up);
      if (r.pAvg != 0xFFFF) { pN++; pSum += r.pAvg; pMx = max(pMx, (int)r.pMax); }
      if (r.gw != 0xFFFF) { gN++; gSum += r.gw; }
      if (r.isp != 0xFFFF) { iN++; iSum += r.isp; }
      if (r.loss != 0xFFFF) lossMx = max(lossMx, (int)r.loss);
    }
    int x = gx + c;
    if (!n) { prevQx = prevPx = prevGx = prevIx = -1; continue; }
    int q = qSum / n;
    int yq = qY(q);
    gfx->drawFastVLine(x, yq, qy + qh - 2 - yq, healthDim(q));
    if (prevQx >= 0) { gfx->drawLine(prevQx, prevQy, x, yq, healthColor(q)); gfx->drawLine(prevQx, prevQy - 1, x, yq - 1, healthColor(q)); }
    else gfx->drawFastVLine(x, yq - 1, 2, healthColor(q));
    prevQx = x; prevQy = yq;
    if (upMin < (is24 ? 100 : 95)) gfx->drawFastVLine(x, qy + 2, stripH, upMin < 50 ? C_RED : C_ORANGE);   // outage marker

    // packet loss marks along the top of the ping chart
    if (lossMx > 0) gfx->drawFastVLine(x, py + 2, stripH, lossMx > LOSS_BAD_PCT * 10 ? C_RED : C_ORANGE);
    // router ping underneath the internet line
    if (gN) {
      int yg = pY(gSum / gN);
      if (prevGx >= 0) gfx->drawLine(prevGx, prevGy, x, yg, C_ROUTER);
      else gfx->drawPixel(x, yg, C_ROUTER);
      prevGx = x; prevGy = yg;
    } else prevGx = -1;
    // ISP's first router
    if (iN) {
      int yi = pY(iSum / iN);
      if (prevIx >= 0) gfx->drawLine(prevIx, prevIy, x, yi, C_ISP);
      else gfx->drawPixel(x, yi, C_ISP);
      prevIx = x; prevIy = yi;
    } else prevIx = -1;
    if (pN) {
      int pa = pSum / pN;
      int ya = pY(pa), ym = pY(pMx);
      if (ym < ya) gfx->drawFastVLine(x, ym, ya - ym, pingColor(pMx) == C_GREEN ? C_GREEN_D : (pingColor(pMx) == C_YELLOW ? C_YELL_D : C_RED_D));
      uint16_t pc = pingColor(pa);
      if (prevPx >= 0) { gfx->drawLine(prevPx, prevPy, x, ya, pc); gfx->drawLine(prevPx, prevPy - 1, x, ya - 1, pc); }
      else gfx->drawFastVLine(x, ya - 1, 2, pc);
      prevPx = x; prevPy = ya;
    } else prevPx = -1;
  }

  // y axis labels
  textBg = C_BG;
  drawText(gx - 4, qY(100) - capX / 2, "100", fx, C_DIM, AL_RIGHT);
  if (qh > capX * 5) drawText(gx - 4, qY(50) - capX / 2, "50", fx, C_DIM, AL_RIGHT);
  drawText(gx - 4, qY(0) - capX, "0", fx, C_DIM, AL_RIGHT);
  // ping labels, top to bottom, skipping any that would collide
  int lastY = -1000;
  auto pLabel = [&](int ms, uint16_t c) {
    int y = constrain(pY(ms) - capX / 2, py, py + ph - capX);
    if (y < lastY + capX + 3) return;
    drawText(gx - 4, y, String(ms), fx, c, AL_RIGHT);
    lastY = y;
  };
  if (pingMax > PING_BAD_MS + 30 && ph > capX * 7) pLabel(pingMax, C_DIM);
  pLabel(PING_BAD_MS, C_RED);
  pLabel(PING_WARN_MS, C_YELLOW);

  // time labels: hours for 24h, weekday for 7d, date for 14d (skipped when they'd overlap)
  int lastRight = -1000;
  static const char *WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  for (int i = 0; i < nt; i++) {
    struct tm tmv;
    localtime_r(&ticks[i].t, &tmv);
    char lb[12];
    int lx;
    if (is24) { snprintf(lb, sizeof(lb), "%02d:00", tmv.tm_hour); lx = ticks[i].x; }
    else if (is7) {
      snprintf(lb, sizeof(lb), "%s", WD[tmv.tm_wday]);
      int nx = (i + 1 < nt) ? ticks[i + 1].x : gx + gw;
      lx = (ticks[i].x + nx) / 2;              // centre of the day
      if (nx - ticks[i].x < textW(lb, fx)) continue;
    } else { snprintf(lb, sizeof(lb), "%d/%d", tmv.tm_mday, tmv.tm_mon + 1); lx = ticks[i].x; }
    int w = textW(lb, fx);
    int x0 = constrain(lx - w / 2, gx, gx + gw - w);
    if (x0 < lastRight + 8) continue;
    drawText(x0, timeY, lb, fx, C_GREY);
    lastRight = x0 + w;
  }
  if (lastRight < gx + gw - textW("NOW", fx) - 8) drawText(gx + gw, timeY, "NOW", fx, C_CYAN, AL_RIGHT);
}

// =======================================================================================
// FIRMWARE UPDATES  -  check GitHub at start-up and once a day, prompt, install
// =======================================================================================
// Asks GitHub; returns 1 = update waiting, 0 = none, -1 = error. manual = from the SETTINGS menu.
int runUpdateCheck(bool manual) {
  updChecked = true;
  updLastCheck = millis();
  OtaInfo info;
  int r = otaCheck(UPDATE_REPO, UPDATE_ASSET, FW_VERSION, info, updErr);
  idleWork();
  logMsg("Update check: %u bytes of stack left\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  updCheckedT = timeValid() ? (uint32_t)time(nullptr) : 0;
  if (r < 0) { updCheckMsg = "FAILED: " + updErr; logMsg("Update check: %s\n", updErr.c_str()); return -1; }
  if (r == 0) {
    updCheckMsg = updErr.length() ? updErr : String("UP TO DATE");
    updCheckMsg.toUpperCase();
    logMsg("Update check: up to date (%s)%s%s\n", FW_VERSION, updErr.length() ? " - " : "", updErr.c_str());
    return 0;
  }
  bool ignored = !manual && updSkip.length() && otaRank(info.tag.c_str()) <= otaRank(updSkip.c_str());
  logMsg("Update check: %s available%s\n", info.tag.c_str(), ignored ? " (ignored)" : "");
  updCheckMsg = info.tag + (ignored ? " IGNORED" : " AVAILABLE");
  if (ignored) return 0;
  bool fresh = !updAvail || info.tag != updInfo.tag;
  updInfo = info;
  if (!updAvail) { updAvail = true; pageDirty = true; }    // header gets the UPD tag
  if (fresh && !manual) openModal();
  return 1;
}

void updateCheckDue() {
#if UPDATE_CHECK
  if (!setUpdChk || !wifiOK || !internetOK || overlayOpen()) return;
  bool due = !updChecked ? millis() > 45000UL : millis() - updLastCheck >= UPDATE_CHECK_MS;
  if (due) runUpdateCheck(false);
#endif
}

// word-wrap `t` into up to maxLines lines that fit `w`
int wrapText(const String &t, const AAFont *f, int w, String *lines, int maxLines) {
  int n = 0;
  String cur;
  bool finished = false;
  int i = 0, len = t.length();
  while (i <= len && n < maxLines) {
    int j = i;
    while (j < len && t[j] != ' ' && t[j] != '\n') j++;
    String word = t.substring(i, j);
    String cand = cur.length() ? cur + " " + word : word;
    if (cur.length() && textW(cand, f) > w) { lines[n++] = cur; cur = word; }
    else cur = cand;
    if (j >= len || t[j] == '\n') {
      bool lost = cur.length() && n >= maxLines;
      if (!lost && cur.length()) lines[n++] = cur;
      cur = "";
      if (lost) break;
      if (j >= len) { finished = true; break; }
    }
    i = j + 1;
  }
  if (n == maxLines && !finished) lines[n - 1] = fitText(lines[n - 1] + " ...", w, f);
  return n;
}

// modal geometry (shared by the drawing helpers)
ModalGeo modalGeo() {
  ModalGeo g;
  int w = min((int)L.W - 2 * L.m, L.big ? 400 : 300);
  int h = min((int)L.H - L.hdrH - 2 * L.m, L.land ? (int)L.H - 2 * L.m : (L.big ? 330 : 250));
  g.box = { (int16_t)((L.W - w) / 2), (int16_t)((L.H - h) / 2), (int16_t)w, (int16_t)h };
  int pad = L.big ? 16 : 11;
  int bh = capH(L.fM) + (L.big ? 22 : 16);
  int hintH = capH(L.fXS) + 8;
  int by = g.box.b() - pad - hintH - 10 - bh;
  int bw = (w - 2 * pad - pad) / 2;
  g.b0 = { (int16_t)(g.box.x + pad), (int16_t)by, (int16_t)bw, (int16_t)bh };
  g.b1 = { (int16_t)(g.box.x + pad + bw + pad), (int16_t)by, (int16_t)bw, (int16_t)bh };
  g.bar = { (int16_t)(g.box.x + pad), (int16_t)(by + bh + 6), (int16_t)(w - 2 * pad), 3 };
  return g;
}

void drawModalButton(const Box &b, const char *label, bool sel, uint16_t c) {
  panel(b.x, b.y, b.w, b.h, sel ? c : C_CARD, c, C_CARD);
  textBg = sel ? c : C_CARD;
  drawText(b.x + b.w / 2, b.y + (b.h - capH(L.fM)) / 2, label, L.fM, sel ? C_BG : c, AL_CENTER);
}

void drawModal() {
  ModalGeo g = modalGeo();
  gfx->fillScreen(C_BG);
  panel(g.box.x, g.box.y, g.box.w, g.box.h, C_CARD, C_PINK, C_BG);
  textBg = C_CARD;
  int pad = L.big ? 16 : 11;
  int x = g.box.x + pad, w = g.box.w - 2 * pad, y = g.box.y + pad;
  int tw = drawText(x, y, "//", L.fM, C_PINK);
  const AAFont *tf = textW("UPDATE AVAILABLE", L.fM) <= w - tw - 6 ? L.fM : L.fS;
  drawText(x + tw + 6, y + (capH(L.fM) - capH(tf)) / 2, "UPDATE AVAILABLE", tf, C_WHITE);
  y += capH(L.fM) + (L.big ? 16 : 11);
  int vw = drawText(x, y, FW_VERSION, L.fL, C_DIM);
  vw += drawText(x + vw, y, "  >  ", L.fL, C_PINK);
  drawText(x + vw, y, updInfo.tag, L.fL, C_CYAN);
  y += capH(L.fL) + (L.big ? 14 : 10);
  // release notes, as many lines as fit above the buttons
  int lineH = capH(L.fXS) + (L.big ? 8 : 6);
  int maxLines = max(0, (g.b0.y - 8 - y) / lineH);
  String lines[8];
  String notes = updInfo.notes.length() ? updInfo.notes : String("New firmware from GitHub.");
  int n = wrapText(notes, L.fXS, w, lines, min(maxLines, 8));
  for (int i = 0; i < n; i++) { drawText(x, y, lines[i], L.fXS, C_GREY); y += lineH; }
  drawModalButton(g.b0, "UPDATE", modalSel == 0, C_CYAN);
  drawModalButton(g.b1, "IGNORE", modalSel == 1, C_PINK);
  gfx->fillRect(g.bar.x, g.bar.y, g.bar.w, g.bar.h, C_TRACK);
  textBg = C_CARD;
  drawText(g.box.x + g.box.w / 2, g.bar.y + 10, "TAP = SWITCH    HOLD = SELECT", L.fXS, C_DIM, AL_CENTER);
}

void drawModalHold(int pct) {
  ModalGeo g = modalGeo();
  int fw = g.bar.w * pct / 100;
  gfx->fillRect(g.bar.x, g.bar.y, fw, g.bar.h, modalSel == 0 ? C_CYAN : C_PINK);
  gfx->fillRect(g.bar.x + fw, g.bar.y, g.bar.w - fw, g.bar.h, C_TRACK);
}

void openModal() {
  if (!updAvail) return;
  modalOpen = true;
  modalSel = 0;
  modalOpenedMs = millis();
  touchWakeMillis = millis();         // light the screen up at night too
  updateBacklight();
  drawModal();
}

void closeModal() {
  modalOpen = false;
  pageDirty = true;
}

// progress screen while downloading
static int updShownPct = -2;
void drawUpdateScreen(const char *line, uint16_t c) {
  gfx->fillScreen(C_BG);
  ModalGeo g = modalGeo();
  int h = capH(L.fM) + capH(L.fL) + capH(L.fXS) * 2 + (L.big ? 90 : 70);
  Box b = { g.box.x, (int16_t)((L.H - h) / 2), g.box.w, (int16_t)h };
  panel(b.x, b.y, b.w, b.h, C_CARD, C_CYAN, C_BG);
  textBg = C_CARD;
  int pad = L.big ? 16 : 11, x = b.x + pad, y = b.y + pad;
  int tw = drawText(x, y, "//", L.fM, C_PINK);
  drawText(x + tw + 6, y, "UPDATING", L.fM, C_WHITE);
  y += capH(L.fM) + 12;
  drawText(x, y, updInfo.tag, L.fL, C_CYAN);
  y += capH(L.fL) + 14;
  updShownPct = -2;
  drawText(x, b.b() - pad - capH(L.fXS), line, L.fXS, c);
}

void updProgress(int pct) {
  if (wdtStarted) esp_task_wdt_reset();
  if (pct == updShownPct || pct < 0) return;
  updShownPct = pct;
  ModalGeo g = modalGeo();
  int h = capH(L.fM) + capH(L.fL) + capH(L.fXS) * 2 + (L.big ? 90 : 70);
  int top = (L.H - h) / 2, pad = L.big ? 16 : 11;
  int x = g.box.x + pad, w = g.box.w - 2 * pad;
  int y = top + pad + capH(L.fM) + 12 + capH(L.fL) + 14;
  gfx->fillRect(x, y, w, 8, C_TRACK);
  gfx->fillRect(x, y, w * pct / 100, 8, C_CYAN);
  textBg = C_CARD;
  gfx->fillRect(x + w - textW("100%", L.fS) - 2, y - capH(L.fS) - 8, textW("100%", L.fS) + 2, capH(L.fS) + 4, C_CARD);
  drawText(x + w, y - capH(L.fS) - 6, String(pct) + "%", L.fS, C_WHITE, AL_RIGHT);
}

void modalConfirm() {
  if (modalSel == 1) {                       // ignore this version until a newer one comes out
    Preferences prefs;
    prefs.begin("imon", false);
    prefs.putString("skipver", updInfo.tag.c_str());
    prefs.end();
    updSkip = updInfo.tag;
    updAvail = false;
    logMsg("Update %s ignored\n", updInfo.tag.c_str());
    closeModal();
    return;
  }
  drawUpdateScreen("DOWNLOADING - DON'T UNPLUG", C_YELLOW);
  updProgress(0);
  saveEvents();
  String err;
  if (otaInstall(updInfo.url, FW_VERSION, updProgress, err)) {
    drawUpdateScreen("INSTALLED - RESTARTING", C_GREEN);
    updProgress(100);
    logMsg("Update %s installed, restarting\n", updInfo.tag.c_str());
    delay(1500);
    ESP.restart();
  }
  logMsg("Update failed: %s\n", err.c_str());
  drawUpdateScreen(("FAILED: " + err).c_str(), C_RED);
  unsigned long t = millis();
  while (millis() - t < 5000) { if (wdtStarted) esp_task_wdt_reset(); delay(20); }
  closeModal();                              // still offered: hold anywhere to try again
}

void drawUpdBadge() {
  int w = updBadgeW(), h = capH(L.fXS) + 8;
  int x = L.W - 10 - textW("00:00", L.fM) - 8 - (SD_LOG ? sdBadgeW() + 6 : 0) - w, y = (L.hdrH - h) / 2;
  if (!updAvail) return;
  gfx->fillRect(x, y, w, h, C_RED_D);
  gfx->drawRect(x, y, w, h, C_PINK);
  textBg = C_RED_D;
  drawText(x + w / 2, y + 4, "UPD", L.fXS, C_PINK, AL_CENTER);
  textBg = C_HEADER;
}

// =======================================================================================
// DEVICE PAGE  -  network / device / storage / test stats. Hold here for SETTINGS.
// =======================================================================================

// short labels for narrow columns (CYD landscape)
const char *shortLabel(const char *l) {
  static const char *MAP[][2] = { { "ISP ROUTER", "ISP" }, { "PUBLIC IP", "WAN" }, { "DNS SERVER", "DNS" }, { "ROUTER", "RTR" },
    { "HISTORY", "FLASH" }, { "MICROSD", "SD" }, { "FIRMWARE", "FW" }, { "UPDATES", "UPD" }, { "UPTIME", "UP" },
    { "LAST START", "START" }, { "MEMORY", "RAM" }, { "TESTS RUN", "TESTS" }, { "PINGS LOST", "LOST" },
    { "EVENTS SAVED", "EVENTS" }, { "CHIP TEMP", "TEMP" }, { "SIGNAL", "RSSI" } };
  for (auto &m : MAP) if (!strcmp(l, m[0])) return m[1];
  return l;
}

int buildStats(StatRow *r, int max, bool leftCol, bool both, bool narrow) {
  int n = 0;
  auto add = [&](const char *l, const String &v, uint16_t c, uint8_t k) { if (n < max) r[n++] = { (narrow && k != 1) ? shortLabel(l) : l, v, c, k }; };
  if (both || leftCol) {
    add("NETWORK", "", C_PINK, 1);
    add("WI-FI", cfgSsid.length() ? cfgSsid : String("--"), C_WHITE, 0);
    add("SIGNAL", wifiOK ? String(wifiRSSI) + " dBm  CH " + String(WiFi.channel()) : String(narrow ? "OFF" : "DISCONNECTED"), wifiOK ? rssiColor(wifiRSSI) : C_RED, 0);
    add("IP", ipText, C_WHITE, 0);
    add("ROUTER", wifiOK ? WiFi.gatewayIP().toString() : String("--"), C_ROUTER, 0);
    add("ISP ROUTER", ispIp ? ipStr(ispIp) + (narrow ? String("") : "  HOP " + String(ispTtl)) : String("NOT FOUND YET"), C_ISP, 0);
    add("PUBLIC IP", wanIp.length() ? wanIp : String("--"), C_PINK, 0);
    add("DNS SERVER", wifiOK ? WiFi.dnsIP(0).toString() : String("--"), C_WHITE, 2);
    add("STORAGE", "", C_PINK, 1);
    String hist = fsOK ? String((unsigned)(LittleFS.usedBytes() / 1024)) + " / " + String((unsigned)(LittleFS.totalBytes() / 1024)) + " KB" : String("NOT AVAILABLE");
    add("HISTORY", hist, fsOK ? C_WHITE : C_RED, 0);
    String sd = !setSdLog ? String("OFF") : sdState == SD_OK ? String((unsigned)(SD.cardSize() / (1024ULL * 1024ULL * 1024ULL))) + " GB  " + String((unsigned long)sdRows) + " ROWS"
              : sdState == SD_ERR ? String("CARD ERROR") : String("NO CARD");
    add("MICROSD", sd, sdState == SD_OK && setSdLog ? C_GREEN : (sdState == SD_ERR ? C_RED : C_DIM), 0);
  }
  if (both || !leftCol) {
    add("DEVICE", "", C_PINK, 1);
    add("FIRMWARE", String(FW_VERSION) + (updAvail ? "  >  " + updInfo.tag : String("")), updAvail ? C_PINK : C_CYAN, 0);
    add("UPDATES", updCheckMsg.length() ? updCheckMsg : String(setUpdChk ? "NOT CHECKED YET" : "AUTO CHECK OFF"), C_WHITE, 0);
    add("UPTIME", fmtDuration(millis() / 1000), C_WHITE, 0);
    add("LAST START", resetReasonName(), C_WHITE, 0);
    add("MEMORY", narrow ? String(ESP.getFreeHeap() / 1024) + "K  MIN " + String(ESP.getMinFreeHeap() / 1024) + "K"
                         : String(ESP.getFreeHeap() / 1024) + " KB FREE  (MIN " + String(ESP.getMinFreeHeap() / 1024) + ")", C_WHITE, 0);
#if CONFIG_IDF_TARGET_ESP32S3
    add("CHIP TEMP", String(temperatureRead(), 0) + " C", C_WHITE, 2);
#endif
    add("STACK LEFT", String((unsigned)uxTaskGetStackHighWaterMark(NULL)) + " BYTES", C_DIM, 2);
    add("BOARD", BOARD_NAME, C_DIM, 2);
    add("TESTS", "", C_PINK, 1);
    add("TESTS RUN", String((unsigned long)testsRun) + (narrow ? "" : " SINCE START"), C_WHITE, 0);
    float lp = totPingSent ? 100.0f * totPingLost / totPingSent : -1;
    add("PINGS LOST", String((unsigned long)totPingLost) + " / " + String((unsigned long)totPingSent) + "  (" + fmtLoss(lp) + ")", lossColor(lp), 0);
    add("EVENTS SAVED", String(evCount), C_WHITE, 2);
  }
  return n;
}

void drawStatColumn(int x, int y, int w, int h, StatRow *r, int n) {
  const AAFont *f = L.fXS;
  int rowH = capH(f) + (L.big ? 8 : 5);
  // drop optional rows (from the end) until it fits
  int need = n * rowH;
  for (int i = n - 1; i >= 0 && need > h; i--) if (r[i].kind == 2) { r[i].kind = 3; need -= rowH; }
  int labW = 0;
  for (int i = 0; i < n; i++) if (r[i].kind == 0 || r[i].kind == 2) labW = max(labW, textW(r[i].label, f));
  labW = min(labW + 8, w * 45 / 100);
  int yy = y;
  for (int i = 0; i < n; i++) {
    if (r[i].kind == 3) continue;
    if (yy + capH(f) > y + h) break;
    if (r[i].kind == 1) {
      int tw = drawText(x, yy, "// ", f, C_PINK);
      tw += drawText(x + tw, yy, r[i].label, f, C_PINK);
      gfx->drawFastHLine(x + tw + 6, yy + capH(f) / 2, w - tw - 6, C_CARD2);
    } else {
      drawText(x, yy, r[i].label, f, C_DIM);
      drawText(x + labW, yy, fitText(r[i].value, w - labW, f), f, r[i].color);
    }
    yy += rowH;
  }
}

void drawDevicePage() {
  gfx->fillScreen(C_BG);
  drawHeader("DEVICE");
  const int m = L.m;
  int hintH = capH(L.fXS) + (L.big ? 12 : 9);
  Box B = { (int16_t)m, (int16_t)(L.hdrH + m), (int16_t)(L.W - 2 * m), (int16_t)(L.H - L.hdrH - 2 * m - hintH) };
  int pad = L.big ? 12 : 9;
  static StatRow rows[32];
  if (L.land) {                                           // two columns side by side
    int cw = (B.w - m) / 2;
    Box a = { B.x, B.y, (int16_t)cw, B.h }, b = { (int16_t)(B.x + cw + m), B.y, (int16_t)(B.w - cw - m), B.h };
    card(a, C_CARD, C_CYAN);
    card(b, C_CARD, C_PINK);
    textBg = C_CARD;
    bool narrow = cw - 2 * pad < 200;
    int n = buildStats(rows, 32, true, false, narrow);
    drawStatColumn(a.x + pad, a.y + pad, a.w - 2 * pad, a.h - 2 * pad, rows, n);
    n = buildStats(rows, 32, false, false, narrow);
    drawStatColumn(b.x + pad, b.y + pad, b.w - 2 * pad, b.h - 2 * pad, rows, n);
  } else {
    card(B, C_CARD, C_CYAN);
    textBg = C_CARD;
    int n = buildStats(rows, 32, true, true, false);
    drawStatColumn(B.x + pad, B.y + pad, B.w - 2 * pad, B.h - 2 * pad, rows, n);
  }
  textBg = C_BG;
  drawText(L.W / 2, L.H - hintH + (hintH - capH(L.fXS)) / 2 - 1, "HOLD FOR SETTINGS", L.fXS, C_CYAN, AL_CENTER);
}

// =======================================================================================
// SETTINGS MENU  -  tap = next item, hold = change it / run it. Saved straight away.
// =======================================================================================

int settingsItems(uint8_t *ids) {
  int n = 0;
  for (uint8_t i = 0; i < SI_COUNT; i++) {
#if !defined(HAS_RGB_LED)
    if (i == SI_LED) continue;
#endif
    if (i == SI_INSTALL && !updAvail) continue;
    ids[n++] = i;
  }
  return n;
}

String settingLabel(uint8_t id) {
  switch (id) {
    case SI_ORIENT:   return "ORIENTATION";
    case SI_BRIGHT:   return "BRIGHTNESS";
    case SI_NIGHT:    return "NIGHT DIMMING";
    case SI_LED:      return "STATUS LED";
    case SI_PING:     return "PING TARGET";
    case SI_SD:       return "MICROSD LOG";
    case SI_UPDCHK:   return "AUTO UPDATES";
    case SI_CHECKNOW: return "CHECK FOR UPDATE";
    case SI_INSTALL:  return "INSTALL " + updInfo.tag;
    case SI_CLEAR:    return "CLEAR HISTORY";
    case SI_RESTART:  return "RESTART";
    default:          return "BACK";
  }
}

String settingValue(uint8_t id) {
  static const char *ORI[4] = { "PORTRAIT", "LANDSCAPE", "PORTRAIT FLIP", "LANDSCAPE FLIP" };
  static const char *BRI[4] = { "25%", "50%", "75%", "100%" };
  static const char *NGT[4] = { "OFF", "22:00-06:00", "23:00-07:00", "21:00-07:00" };
  switch (id) {
    case SI_ORIENT:   return ORI[cfgRot & 3];
    case SI_BRIGHT:   return BRI[setBri & 3];
    case SI_NIGHT:    return NGT[setNight & 3];
    case SI_LED:      return setLed ? "ON" : "OFF";
    case SI_PING:     return pingHost1();
    case SI_SD:       return !setSdLog ? "OFF" : (sdState == SD_OK ? "ON" : (sdState == SD_ERR ? "ON - ERROR" : "ON - NO CARD"));
    case SI_UPDCHK:   return setUpdChk ? "DAILY" : "OFF";
    case SI_CHECKNOW: return updCheckMsg;
    case SI_CLEAR:    return (clearArmedMs && millis() - clearArmedMs < 5000UL) ? "HOLD AGAIN" : "";
    default:          return "";
  }
}

SetGeo settingsGeo(int n) {
  SetGeo g;
  int hintH = capH(L.fXS) + (L.big ? 12 : 9);
  g.listY = L.hdrH + L.m;
  int listH = L.H - g.listY - L.m - hintH;
  g.rowH = capH(L.fS) + (L.big ? 18 : 12);
  g.rows = max(1, listH / g.rowH);
  g.first = constrain((int)setSel - g.rows / 2, 0, max(0, n - g.rows));
  g.x = L.m; g.w = L.W - 2 * L.m;
  return g;
}

void drawSettings() {
  uint8_t ids[SI_COUNT];
  int n = settingsItems(ids);
  if (setSel >= n) setSel = 0;
  gfx->fillScreen(C_BG);
  drawHeader("SETTINGS");
  SetGeo g = settingsGeo(n);
  int pad = L.big ? 12 : 9;
  panel(g.x, g.listY, g.w, g.rows * g.rowH + 4, C_CARD, C_PINK, C_BG);
  for (int r = 0; r < g.rows && g.first + r < n; r++) {
    int i = g.first + r;
    int y = g.listY + 2 + r * g.rowH;
    bool sel = i == setSel;
    uint16_t bg = sel ? C_CARD2 : C_CARD;
    if (sel) {
      gfx->fillRect(g.x + 2, y, g.w - 4, g.rowH, bg);
      gfx->fillRect(g.x + 2, y + 3, 3, g.rowH - 6, C_PINK);
    } else if (r) gfx->drawFastHLine(g.x + pad, y, g.w - 2 * pad, C_CARD2);
    textBg = bg;
    int ty = y + (g.rowH - capH(L.fS)) / 2;
    String lab = settingLabel(ids[i]), val = settingValue(ids[i]);
    uint16_t lc = ids[i] == SI_INSTALL ? C_PINK : (ids[i] == SI_BACK ? C_CYAN : (sel ? C_WHITE : C_GREY));
    int lw = drawText(g.x + pad + 4, ty, lab, L.fS, lc);
    if (val.length()) drawText(g.x + g.w - pad, ty, fitText(val, g.w - 3 * pad - lw - 4, L.fS), L.fS, sel ? C_CYAN : C_DIM, AL_RIGHT);
  }
  // scroll marks
  int cx = g.x + g.w / 2, by = g.listY + g.rows * g.rowH + 4;
  if (g.first > 0) gfx->fillTriangle(cx - 6, g.listY + 1, cx + 6, g.listY + 1, cx, g.listY - 4, C_PINK);
  if (g.first + g.rows < n) gfx->fillTriangle(cx - 6, by, cx + 6, by, cx, by + 5, C_PINK);
  textBg = C_BG;
  int hintH = capH(L.fXS) + (L.big ? 12 : 9);
  drawText(L.W / 2, L.H - hintH + (hintH - capH(L.fXS)) / 2 - 1, "TAP = NEXT    HOLD = CHANGE", L.fXS, C_DIM, AL_CENTER);
}

void drawSettingsHold(int pct) {
  uint8_t ids[SI_COUNT];
  int n = settingsItems(ids);
  SetGeo g = settingsGeo(n);
  int r = setSel - g.first;
  int y = g.listY + 2 + (r + 1) * g.rowH - 3;
  int w = g.w - 8;
  gfx->fillRect(g.x + 4, y, w * pct / 100, 2, C_PINK);
}

void openSettings() {
  settingsOpen = true;
  setSel = 0;
  clearArmedMs = 0;
  settingsTouchedMs = millis();
  drawSettings();
}

void closeSettings() {
  settingsOpen = false;
  pageDirty = true;
}

void settingsNext() {
  uint8_t ids[SI_COUNT];
  int n = settingsItems(ids);
  setSel = (setSel + 1) % n;
  settingsTouchedMs = millis();
  drawSettings();
}

void settingsActivate() {
  uint8_t ids[SI_COUNT];
  int n = settingsItems(ids);
  settingsTouchedMs = millis();
  uint8_t id = ids[setSel % n];
  switch (id) {
    case SI_ORIENT:                                   // turn the screen right away
      cfgRot = (cfgRot + 1) & 3; saveSetting("rot", cfgRot);
      gfx->setRotation(panelRotation(cfgRot));
      computeLayout();
      break;
    case SI_BRIGHT:  setBri = (setBri + 1) & 3; saveSetting("bri", setBri); touchWakeMillis = millis(); updateBacklight(); break;
    case SI_NIGHT:   setNight = (setNight + 1) & 3; saveSetting("ngt", setNight); updateBacklight(); break;
    case SI_LED:     setLed = !setLed; saveSetting("led", setLed); applyStatusLed(); break;
    case SI_PING:                                     // new target: start the windows fresh
      setPing = (setPing + 1) % 3; saveSetting("png", setPing);
      jitCount = jitIndex = 0; jitterMs = -1; lossCount = lossIndex = 0; lossPct = -1;
      ispIp = 0; ispLastSearch = 0;
      break;
    case SI_SD:
      setSdLog = !setSdLog; saveSetting("sdl", setSdLog);
      if (!setSdLog) { SD.end(); sdState = SD_NONE; } else sdMount();
      break;
    case SI_UPDCHK:  setUpdChk = !setUpdChk; saveSetting("upd", setUpdChk); break;
    case SI_CHECKNOW:
      if (!wifiOK || !internetOK) { updCheckMsg = "NO INTERNET"; break; }
      updCheckMsg = "CHECKING...";
      drawSettings();
      runUpdateCheck(true);
      break;
    case SI_INSTALL:                                  // the update prompt, UPDATE pre-selected
      settingsOpen = false;
      openModal();
      return;
    case SI_CLEAR:
      if (clearArmedMs && millis() - clearArmedMs < 5000UL) {
        log1m.clear(); log15m.clear(); trendCount = 0;
        evCount = evHead = 0; evDirty = true; saveEvents();
        clearArmedMs = 0;
        logMsg("History cleared from SETTINGS\n");
      } else clearArmedMs = millis();
      break;
    case SI_RESTART:
      saveEvents();
      gfx->fillScreen(C_BG);
      textBg = C_BG;
      drawText(L.W / 2, L.H / 2 - capH(L.fM) / 2, "RESTARTING...", L.fM, C_CYAN, AL_CENTER);
      delay(300);
      ESP.restart();
      return;
    default:
      closeSettings();
      return;
  }
  drawSettings();
}

// =======================================================================================
// EVENTS PAGE  -  outages, IP changes, Wi-Fi drops, restarts (newest first)
// =======================================================================================
uint16_t evColor(uint8_t t) {
  switch (t) {
    case EV_OUTAGE:    return C_RED;
    case EV_NET_BACK:  return C_GREEN;
    case EV_WIFI_LOST: return C_YELLOW;
    case EV_IP_CHANGE: return C_PINK;
    case EV_AP_CHANGE: return C_ROUTER;
    case EV_ISP_HOP:   return C_ISP;
    case EV_FIRMWARE:  return C_CYAN;
    default:           return C_GREY;
  }
}

void drawEventsPage() {
  gfx->fillScreen(C_BG);
  drawHeader("EVENTS");
  const int m = L.m;
  const AAFont *fx = L.fXS;
  const int capX = capH(fx);

  // ---- summary: outages in 24 h, downtime in 24 h, public IP ----
  int outs = 0;
  uint32_t now = timeValid() ? (uint32_t)time(nullptr) : 0;
  for (int i = 0; i < evCount; i++) { const Event &e = evAt(i); uint32_t t = evTime(e); if (e.type == EV_OUTAGE && now && t && now - t < 86400UL) outs++; }
  RangeStats st = calcStats(RANGES[0]);
  int sh = capX + capH(L.fM) + (L.big ? 26 : 18);
  Box sb = { (int16_t)m, (int16_t)(L.hdrH + m), (int16_t)(L.W - 2 * m), (int16_t)sh };
  const char *lab[3] = { "OUTAGES 24H", "DOWN 24H", "PUBLIC IP" };
  const char *labS[3] = { "OUTAGES", "DOWN", "WAN IP" };
  String val[3] = { String(outs), st.slots ? fmtDuration(st.downSec) : String("--"), wanIp.length() ? wanIp : String("--") };
  uint16_t col[3] = { outs ? C_RED : C_GREEN, st.downSec ? (st.downSec < 600 ? C_YELLOW : C_RED) : C_GREEN, C_PINK };
  // the IP needs the most room: give it half the strip
  int w2 = (sb.w - 2 * m) * 42 / 100, w0 = (sb.w - 2 * m - w2) / 2;
  w2 = sb.w - 2 * m - 2 * w0;
  int xs[3] = { sb.x, sb.x + w0 + m, sb.x + 2 * (w0 + m) };
  int ws[3] = { w0, w0, w2 };
  for (int i = 0; i < 3; i++) {
    Box c = { (int16_t)xs[i], sb.y, (int16_t)ws[i], sb.h };
    card(c, C_CARD, col[i]);
    textBg = C_CARD;
    int pad = L.big ? 10 : 7;
    drawText(c.x + pad, c.y + pad, textW(lab[i], fx) > c.w - 2 * pad ? labS[i] : lab[i], fx, C_GREY);
    const AAFont *vf = fitFont(val[i], c.w - 2 * pad, L.fM, L.fS, fx);
    drawText(c.x + pad, c.b() - pad - capH(vf), fitText(val[i], c.w - 2 * pad, vf), vf, col[i]);
  }

  // ---- list ----
  Box lb = { (int16_t)m, (int16_t)(sb.b() + m), (int16_t)(L.W - 2 * m), (int16_t)(L.H - sb.b() - 2 * m) };
  card(lb, C_CARD, C_PINK);
  textBg = C_CARD;
  int pad = L.big ? 12 : 9;
  int x = lb.x + pad, w = lb.w - 2 * pad;
  int y = lb.y + pad;
  if (!evCount) {
    drawText(lb.x + lb.w / 2, lb.y + lb.h / 2 - capH(L.fS) / 2, "NO EVENTS YET", L.fS, C_DIM, AL_CENTER);
    return;
  }
  // wide screens: one line per event; narrow: time + type, then the detail underneath
  int timeW = textW("00/00 00:00", fx) + 10;
  int typeW = textW("BACK ONLINE", fx) + 10;
  bool oneLine = w >= timeW + typeW + textW("after internet outage  (59 min)", fx);
  int rowH = oneLine ? capX + (L.big ? 10 : 7) : 2 * capX + (L.big ? 14 : 11);
  for (int i = 0; i < evCount && y + rowH - 4 <= lb.b() - pad / 2; i++) {
    const Event &e = evAt(i);
    uint32_t t = evTime(e);
    char tb[16] = "--/-- --:--";
    if (t) { time_t tt = t; struct tm tmv; localtime_r(&tt, &tmv); snprintf(tb, sizeof(tb), "%02d/%02d %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min); }
    uint16_t c = evColor(e.type);
    gfx->fillRect(x - pad + 3, y, 2, oneLine ? capX : 2 * capX + 4, c);           // colour mark
    drawText(x, y, tb, fx, C_DIM);
    drawText(x + timeW, y, EV_NAME[e.type], fx, c);
    String d = e.detail;
    if (oneLine) {
      if (e.dur) d += "  (" + fmtDuration(e.dur) + ")";
      drawText(x + timeW + typeW, y, fitText(d, w - timeW - typeW, fx), fx, C_GREY);
    } else {
      if (e.dur) drawText(x + w, y, fmtDuration(e.dur), fx, c, AL_RIGHT);    // duration stays visible
      drawText(x + timeW, y + capX + 4, fitText(d, w - timeW, fx), fx, C_GREY);
    }
    y += rowH;
    if (i + 1 < evCount && y - (L.big ? 6 : 4) < lb.b() - pad) gfx->drawFastHLine(x, y - (L.big ? 6 : 4), w, C_CARD2);
  }
}

// =======================================================================================
// COLOURS / TEXT
// =======================================================================================
String fmtLoss(float pct) {
  if (pct < 0) return "--";
  if (pct == 0) return "0%";
  if (pct < 10) return String(pct, 1) + "%";
  return String((int)(pct + 0.5f)) + "%";
}

uint16_t lossColor(float pct) {
  if (pct < 0) return C_DIM;
  return pct > LOSS_BAD_PCT ? C_RED : (pct > LOSS_WARN_PCT ? C_YELLOW : C_GREEN);
}

uint16_t jitterColor(long ms) {
  if (ms < 0) return C_DIM;
  return ms > JITTER_BAD_MS ? C_RED : (ms > JITTER_WARN_MS ? C_YELLOW : C_GREEN);
}

uint16_t levelColor(Level lv) {
  return lv == LV_GOOD ? C_GREEN : (lv == LV_WARN ? C_YELLOW : C_RED);
}

uint16_t healthColor(int v) {
  if (v >= 90) return C_GREEN;
  if (v >= 60) return C_YELLOW;
  return C_RED;
}

uint16_t healthDim(int v) {
  if (v >= 90) return C_GREEN_D;
  if (v >= 60) return C_YELL_D;
  return C_RED_D;
}

uint16_t pingColor(long ms) {
  if (ms < 0) return C_GREY;
  if (ms <= PING_WARN_MS) return C_GREEN;
  if (ms <= PING_BAD_MS) return C_YELLOW;
  return C_RED;
}

uint16_t rssiColor(int rssi) {
  if (rssi > -55) return C_GREEN;
  if (rssi > -67) return C_CYAN;
  if (rssi > -75) return C_YELLOW;
  return C_RED;
}


// =======================================================================================
// SERIAL SETTINGS PROTOCOL (used by the web flasher)
//   IMON GET                                   -> IMON INFO board=.. ver=.. ssid=<hex> tz=<hex> rot=0-3
//   IMON SET <ssidHex> <passHex> <tzHex> <rot>  -> IMON OK, then reboot
//            ssidHex "-" = keep the saved Wi-Fi (change only timezone / orientation)
//   IMON WIPE                                  -> IMON OK, clears settings, reboot
//   IMON CLEARLOG                              -> IMON OK, wipes the 24h / 7d / 14d history
// Values are hex-encoded so any character (spaces etc.) survives.
// =======================================================================================
static String toHex(const String &in) {
  static const char *hx = "0123456789ABCDEF";
  String out;
  for (unsigned i = 0; i < in.length(); i++) {
    uint8_t b = (uint8_t)in.c_str()[i];
    char pair[3] = { hx[b >> 4], hx[b & 15], 0 };
    out += pair;
  }
  return out;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool fromHex(const char *in, String &out) {
  out = "";
  if (strcmp(in, "-") == 0) return true;           // "-" = empty value
  size_t n = strlen(in);
  if (n % 2) return false;
  char buf[2] = { 0, 0 };
  for (size_t i = 0; i < n; i += 2) {
    int a = hexNibble(in[i]), b = hexNibble(in[i + 1]);
    if (a < 0 || b < 0) return false;
    buf[0] = (char)((a << 4) | b);
    out += buf;
  }
  return true;
}

void handleCommand(char *line) {
  char *argv[6] = { 0 };
  int argc = 0;
  for (char *tok = strtok(line, " \r\n"); tok && argc < 6; tok = strtok(nullptr, " \r\n")) argv[argc++] = tok;
  if (argc < 2 || strcmp(argv[0], "IMON") != 0) return;

  if (strcmp(argv[1], "GET") == 0) {
    replyMsg("IMON INFO board=" BOARD_NAME " ver=" FW_VERSION " ssid=%s tz=%s rot=%d log24=%u log14d=%u fs=%d sd=%d sdrows=%lu upd=%s\n",
           toHex(cfgSsid.length() ? cfgSsid : String("-")).c_str(), toHex(cfgTz).c_str(), (int)cfgRot,
           (unsigned)log1m.count(), (unsigned)log15m.count(), fsOK ? 1 : 0, (int)sdState, (unsigned long)sdRows, updAvail ? updInfo.tag.c_str() : "-");
  } else if (strcmp(argv[1], "SET") == 0 && argc >= 5) {
    bool keepWifi = strcmp(argv[2], "-") == 0;
    String ssid, pass, tz;
    if (!fromHex(argv[2], ssid) || !fromHex(argv[3], pass) || !fromHex(argv[4], tz)) {
      replyMsg("IMON ERR bad values\n");
      return;
    }
    if (keepWifi && cfgSsid.length() == 0) {
      replyMsg("IMON ERR no Wi-Fi saved yet\n");
      return;
    }
    Preferences prefs;
    prefs.begin("imon", false);
    if (!keepWifi) {
      prefs.putString("ssid", ssid.c_str());
      prefs.putString("pass", pass.c_str());
    }
    if (tz.length()) prefs.putString("tz", tz.c_str());
    if (argc >= 6 && argv[5][0] >= '0' && argv[5][0] <= '3') prefs.putUChar("rot", argv[5][0] - '0');
    prefs.end();
    replyMsg("IMON OK\n");
    delay(300);
    ESP.restart();
  } else if (strcmp(argv[1], "CLEARLOG") == 0) {
    log1m.clear();
    log15m.clear();
    trendCount = 0;
    evCount = evHead = 0; evDirty = true; saveEvents();
    replyMsg("IMON OK\n");
    pageDirty = true;
  } else if (strcmp(argv[1], "WIPE") == 0) {
    Preferences prefs;
    prefs.begin("imon", false);
    prefs.clear();
    prefs.end();
    replyMsg("IMON OK\n");
    delay(300);
    ESP.restart();
  }
}

void pollSerial() {
  static char buf[400];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = 0; len = 0; handleCommand(buf); }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;   // overflow - drop line
    }
  }
}

// =======================================================================================
// NETWORK
// =======================================================================================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(true);
  delay(150);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
}

void updateWiFiState() {
  wl_status_t st = WiFi.status();
  wifiStateText = wifiStatusName(st);
  wifiOK = (st == WL_CONNECTED);
  if (wifiOK) {
    wifiRSSI = WiFi.RSSI();
    ipText = WiFi.localIP().toString();
  } else {
    wifiRSSI = -100;
    ipText = "--";
    dnsOK = internetOK = captivePortal = false;
    httpCode = 0;
    latencyMs = -1;
  }
}

// =======================================================================================
// MICROSD LOG  -  one CSV per day with every test, for showing your ISP
// =======================================================================================
bool sdMount() {
#if SD_LOG
  sdLastTry = millis();
  spiBusBegin();
  SD.end();
  if (!SD.begin(SD_CS_PIN, sdSPI, SD_SPI_HZ, "/sd", 2, false) || SD.cardType() == CARD_NONE) {
    SD.end();
    if (sdState == SD_OK) sdState = SD_ERR;   // it was working: show the problem
    return false;
  }
  SD.mkdir("/NetPulse");
  sdState = SD_OK;
  logMsg("microSD: %llu MB card, logging to /NetPulse/\n", SD.cardSize() / (1024ULL * 1024ULL));
  return true;
#else
  return false;
#endif
}

static void csvNum(String &row, long v) { row += ','; if (v >= 0) row += String(v); }

bool sdAppend(const char *path, const char *header, const String &row) {
  bool fresh = !SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) { sdState = SD_ERR; SD.end(); drawSdBadge(); return false; }
  if (fresh) f.print(header);
  size_t w = f.print(row);
  f.close();                                                  // closed every time: safe to pull the card
  if (w != row.length()) { sdState = SD_ERR; SD.end(); drawSdBadge(); return false; }
  return true;
}

String fmtLocal(uint32_t t) {
  time_t tt = t;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
  return b;
}

bool sdReady() {
#if SD_LOG
  if (!setSdLog) return false;                                // switched off in SETTINGS
  if (!timeValid()) return false;                             // need the date for names / times
  if (sdState != SD_OK && (millis() - sdLastTry < 30000UL || !sdMount())) { drawSdBadge(); return false; }
  return true;
#else
  return false;
#endif
}

// One row per test: /NetPulse/tests/2026-11-17.csv
void sdLogTest() {
#if SD_LOG
  if (!sdReady()) return;
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char path[56];
  snprintf(path, sizeof(path), "/NetPulse/tests/%04d-%02d-%02d.csv", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  SD.mkdir("/NetPulse/tests");
  String hdrS = String("time,unix,wifi_connected,ping_ms(") + pingHost1() + "),ping_worst_ms,ping_sent,ping_lost,"
                "ping2_ms(" + pingHost2() + "),ping2_sent,ping2_lost,jitter_ms,router_ms,isp_hop_ms,dns_ms,"
                "web_ok,http_code,web_ms,wifi_rssi_dbm,health,status,ping_host\r\n";
  const char *hdr = hdrS.c_str();
  String row = fmtLocal((uint32_t)now);
  row += ',';  row += String((unsigned long)now);
  row += ',';  row += wifiOK ? '1' : '0';
  csvNum(row, latencyMs);
  csvNum(row, pingMaxMs);
  row += ',';  row += String(ping1Sent);
  row += ',';  row += String(ping1Lost);
  csvNum(row, ping2Ms);
  row += ',';  row += String(ping2Sent);
  row += ',';  row += String(ping2Lost);
  csvNum(row, jitterMs);
  csvNum(row, gwMs);
  csvNum(row, ispMs);
  csvNum(row, dnsMs);
  row += ',';  row += internetOK ? '1' : '0';
  row += ',';  row += String(httpCode);
  csvNum(row, webMs);
  row += ',';  if (wifiOK) row += String(wifiRSSI);
  row += ',';  row += String(health);
  row += ",\""; row += statusText; row += '"';
  row += ',';  row += pingHost1();                            // target can be changed in SETTINGS
  row += "\r\n";
  if (sdAppend(path, hdr, row)) { sdState = SD_OK; sdRows++; }
  drawSdBadge();
#endif
}

// Events -> /NetPulse/events.csv, finished outages also -> /NetPulse/outages.csv
void sdEventRows() {
#if SD_LOG
  bool any = false;
  for (int i = 0; i < evCount; i++) { const Event &e = evAt(i); if (!e.saved && evTime(e)) { any = true; break; } }
  if (!any || !sdReady()) return;
  for (int i = evCount - 1; i >= 0; i--) {                    // oldest first
    Event &e = evAt(i);
    uint32_t t = evTime(e);
    if (e.saved || !t) continue;
    String row = fmtLocal(t) + "," + EV_NAME[e.type] + ",\"" + e.detail + "\",";
    if (e.dur) row += String(e.dur);
    row += "\r\n";
    if (!sdAppend("/NetPulse/events.csv", "time,event,detail,duration_s\r\n", row)) return;
    if (e.type == EV_NET_BACK && e.dur) {
      String o = fmtLocal(t - e.dur) + "," + fmtLocal(t) + "," + String(e.dur) + "," + fmtDuration(e.dur) + ",\"" + e.detail + "\"\r\n";
      if (!sdAppend("/NetPulse/outages.csv", "start,end,duration_s,duration,cause\r\n", o)) return;
    }
    e.saved = 1;
    evDirty = true;
  }
#endif
}

// =======================================================================================
// EVENT LOG  -  kept in flash (last 48) and appended to the microSD card
// =======================================================================================
Event &evAt(int i) { return evRing[(evHead - 1 - i + EV_MAX * 2) % EV_MAX]; }   // 0 = newest

uint32_t evTime(const Event &e) {
  if (e.t) return e.t;
  if (!timeValid()) return 0;
  return (uint32_t)time(nullptr) - (millis() - e.ms) / 1000;
}

void saveEvents() {
  if (!fsOK || !evDirty || !fsRoomFor(sizeof(Event) * EV_MAX + 8)) return;
  File f = LittleFS.open("/events.bin", "w");
  if (!f) return;
  uint32_t magic = 0x45564D49, n = evCount;   // "IMVE"
  f.write((const uint8_t *)&magic, 4);
  f.write((const uint8_t *)&n, 4);
  for (int i = evCount - 1; i >= 0; i--) { Event e = evAt(i); e.t = evTime(e); f.write((const uint8_t *)&e, sizeof(Event)); }
  f.close();
  evDirty = false;
}

void loadEvents() {
  evCount = evHead = 0;
  if (!fsOK) return;
  File f = LittleFS.open("/events.bin", "r");
  if (!f) return;
  uint32_t magic = 0, n = 0;
  if (f.read((uint8_t *)&magic, 4) == 4 && magic == 0x45564D49 && f.read((uint8_t *)&n, 4) == 4 && n <= (uint32_t)EV_MAX) {
    for (uint32_t i = 0; i < n; i++) {
      Event e;
      if (f.read((uint8_t *)&e, sizeof(Event)) != sizeof(Event)) break;
      if (!e.t || e.type >= EV_TYPES) continue;          // from an earlier boot with no clock: can't place it
      e.saved = 1;
      evRing[evHead] = e; evHead = (evHead + 1) % EV_MAX; if (evCount < EV_MAX) evCount++;
    }
  }
  f.close();
}

void addEvent(EvType type, const char *detail, uint32_t dur, uint32_t whenT, uint32_t whenMs) {
  Event &e = evRing[evHead];
  e.t = whenT; e.ms = whenMs; e.dur = dur; e.type = type; e.saved = 0;
  strncpy(e.detail, detail ? detail : "", sizeof(e.detail) - 1);
  e.detail[sizeof(e.detail) - 1] = 0;
  evHead = (evHead + 1) % EV_MAX;
  if (evCount < EV_MAX) evCount++;
  evDirty = true;
  logMsg("EVENT %s %s%s\n", EV_NAME[type], e.detail, dur ? (" (" + fmtDuration(dur) + ")").c_str() : "");
  if (currentPage == PAGE_EVENTS) pageDirty = true;
}
void addEvent(EvType type, const char *detail) {
  addEvent(type, detail, 0, timeValid() ? (uint32_t)time(nullptr) : 0, millis());
}

// Called after every test: opens / closes outages
void trackOutage() {
  bool down = !wifiOK || !internetOK;
  if (down && !inOutage) {
    inOutage = true;
    outStartT = timeValid() ? (uint32_t)time(nullptr) : 0;
    outStartMs = millis();
    outCause = !wifiOK ? "Wi-Fi" : (captivePortal ? "login page" : (!dnsOK ? "DNS" : "internet"));
    addEvent(EV_OUTAGE, (outCause + " down").c_str(), 0, outStartT, outStartMs);
  } else if (!down && inOutage) {
    inOutage = false;
    outageJustEnded = true;
    uint32_t dur = (millis() - outStartMs) / 1000;
    addEvent(EV_NET_BACK, ("after " + outCause + " outage").c_str(), dur, timeValid() ? (uint32_t)time(nullptr) : 0, millis());
  }
}

// Wi-Fi drop reasons (the common ones)
const char *wifiReasonName(uint8_t r) {
  switch (r) {
    case 2: return "auth expired";        case 3: return "router left us";   case 4: return "inactivity";
    case 8: return "we left";             case 15: return "4-way handshake";  case 23: return "802.1X auth";
    case 200: return "beacon timeout";    case 201: return "AP not found";    case 202: return "auth failed";
    case 203: return "assoc failed";      case 204: return "handshake timeout"; case 205: return "connection failed";
    default: return "reason";
  }
}

void onWiFiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) { wifiDiscReason = info.wifi_sta_disconnected.reason; wifiDiscFlag = true; }
}

// Called every second: turns a Wi-Fi drop into an event (only the first one while it's down)
void checkWiFiEvents() {
  if (wifiDiscFlag) {
    wifiDiscFlag = false;
    if (wifiWasUp) {
      char d[46];
      snprintf(d, sizeof(d), "%s (%u)", wifiReasonName(wifiDiscReason), (unsigned)wifiDiscReason);
      addEvent(EV_WIFI_LOST, d);
      wifiWasUp = false;
    }
  }
  if (wifiOK) wifiWasUp = true;
}

void checkAccessPoint() {
  String k = WiFi.BSSIDstr() + " ch" + String(WiFi.channel());
  if (apKey.length() && k != apKey) addEvent(EV_AP_CHANGE, k.c_str());
  apKey = k;
}

String resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "restart (settings/update)";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog (was stuck)";
    case ESP_RST_BROWNOUT: return "brownout (power dipped)";
    case ESP_RST_EXT:      return "reset button";
    case ESP_RST_DEEPSLEEP:return "wake";
    default:               return "other";
  }
}

// once the clock is known: fill in times, save to flash and the card
void flushEvents() {
  if (timeValid()) for (int i = 0; i < evCount; i++) { Event &e = evAt(i); if (!e.t) { e.t = evTime(e); evDirty = true; } }
  sdEventRows();
  saveEvents();
}

// ---- ping helpers --------------------------------------------------------------------
struct PingAcc { long sumUs = 0; int got = 0, sent = 0, lost = 0; long worst = -1;
  void add(long us) { sent++; if (us < 0) { lost++; return; } got++; sumUs += us; long ms = (us + 500) / 1000; if (ms > worst) worst = ms; }
  long avgMs() const { return got ? (sumUs / got + 500) / 1000 : -1; } };

void jitterPush(long us) {
  if (us < 0) return;
  jitPing[jitIndex] = (int16_t)min((us + 500) / 1000, 30000L);
  jitIndex = (jitIndex + 1) % JITTER_SAMPLES;
  if (jitCount < JITTER_SAMPLES) jitCount++;
}


void idleWork() { handleTouch(); pollSerial(); if (wdtStarted) esp_task_wdt_reset(); }

bool resolveHost(const char *h, IPAddress &ip) {
  if (ip.fromString(h)) return true;
  return WiFi.hostByName(h, ip) == 1;
}

String ipStr(uint32_t ip4) { const uint8_t *b = (const uint8_t *)&ip4; return String(b[0]) + "." + b[1] + "." + b[2] + "." + b[3]; }

// Finds the first router past your home network (the ISP's), using pings with a growing TTL
void findIspHop(const IPAddress &target) {
  ispLastSearch = millis();
  uint32_t prev = ispIp;
  for (int ttl = 2; ttl <= ISP_MAX_TTL; ttl++) {
    uint32_t from = 0;
    long us = -1;
    for (int tries = 0; tries < 2 && us < 0; tries++) { us = probeHopUs((uint32_t)target, 800, ttl, &from); idleWork(); }
    if (us < 0) continue;                                   // silent hop: try the next one
    if (from == (uint32_t)target) break;                    // reached the target itself
    if (!isPrivateIp(from)) {
      ispIp = from; ispTtl = ttl; ispFails = 0;
      if (ispIp != prev) {
        // remembered across restarts, so only a real change of route is logged
        Preferences prefs;
        prefs.begin("imon", false);
        uint32_t saved = prefs.isKey("isp") ? prefs.getUInt("isp", 0) : 0;
        if (saved != ispIp) {
          addEvent(EV_ISP_HOP, ("ISP router " + ipStr(ispIp) + " (hop " + String(ttl) + ")").c_str());
          prefs.putUInt("isp", ispIp);
        }
        prefs.end();
      }
      return;
    }
  }
  if (!ispIp) logMsg("ISP hop: not found (routers may not answer)\n");
}

void checkPublicIp() {
  wanLastCheck = millis();
  HTTPClient http;
  http.setTimeout(2000);
  http.setConnectTimeout(2000);
  http.begin(PUBLIC_IP_URL);
  int code = http.GET();
  String ip = code == 200 ? http.getString() : String("");
  http.end();
  ip.trim();
  IPAddress chk;
  if (!ip.length() || ip.length() > 15 || !chk.fromString(ip.c_str())) return;
  if (ip != wanIp) {
    Preferences prefs;
    prefs.begin("imon", false);
    String saved = prefs.isKey("wan") ? prefs.getString("wan", "").c_str() : "";
    if (saved.length() && saved != ip) addEvent(EV_IP_CHANGE, (saved + " > " + ip).c_str());
    if (saved != ip) prefs.putString("wan", ip.c_str());
    prefs.end();
    wanIp = ip;
  }
}

void runInternetTest() {
  updateWiFiState();
  pingSent = pingLost = ping1Sent = ping1Lost = ping2Sent = ping2Lost = 0;
  ping2Ms = ispMs = dnsMs = -1;
  if (wifiOK) {
    checkAccessPoint();

    // 1. real pings to both internet targets, interleaved
    IPAddress t1, t2;
    bool ok1 = resolveHost(pingHost1(), t1), ok2 = resolveHost(pingHost2(), t2);
    PingAcc a1, a2;
    for (int i = 0; i < PING_COUNT; i++) {
      if (i) { unsigned long t = millis(); while (millis() - t < PING_GAP_MS) { idleWork(); delay(5); } }
      if (ok1) { long us = pingOnceUs((uint32_t)t1, PING_TIMEOUT_MS); a1.add(us); jitterPush(us); }
      if (ok2) a2.add(pingOnceUs((uint32_t)t2, PING_TIMEOUT_MS));
      idleWork();
    }
    latencyMs = a1.avgMs(); pingMaxMs = a1.worst;
    ping2Ms = a2.avgMs();
    ping1Sent = a1.sent; ping1Lost = a1.lost; ping2Sent = a2.sent; ping2Lost = a2.lost;
    pingSent = a1.sent + a2.sent; pingLost = a1.lost + a2.lost;

    // 2. your router - fine while the internet isn't = the problem is past your house
    IPAddress gw = WiFi.gatewayIP();
    PingAcc ag;
    if ((uint32_t)gw) for (int i = 0; i < 2; i++) { ag.add(pingOnceUs((uint32_t)gw, PING_TIMEOUT_MS)); idleWork(); }
    gwMs = ag.avgMs();

    // 3. the ISP's first router (found once, re-checked every 30 min or when it stops answering)
    if (ok1 && (!ispIp ? (millis() - ispLastSearch > 60000UL || ispLastSearch == 0) : (millis() - ispLastSearch > 1800000UL || ispFails >= 6)))
      findIspHop(t1);
    if (ispIp) {
      PingAcc ai;
      for (int i = 0; i < 2; i++) {
        uint32_t from = 0;
        long us = probeHopUs((uint32_t)t1, PING_TIMEOUT_MS, ispTtl, &from);
        ai.add((us >= 0 && from != (uint32_t)t1 && !isPrivateIp(from)) ? us : -1);
        idleWork();
      }
      ispMs = ai.avgMs();
      ispFails = ispMs < 0 ? ispFails + 1 : 0;
    }

    // 4. DNS: one lookup sent straight to your DNS server (past the board's cache)
    IPAddress dnsSrv = WiFi.dnsIP(0);
    if ((uint32_t)dnsSrv) {
      long us = dnsQueryUs((uint32_t)dnsSrv, DNS_TEST_HOST, 2000);
      if (us < 0) us = dnsQueryUs((uint32_t)dnsSrv, DNS_TEST_HOST, 2000);   // one retry
      dnsOK = us >= 0;
      dnsMs = dnsOK ? (us + 500) / 1000 : -1;
    } else {
      IPAddress ip;
      dnsOK = WiFi.hostByName(DNS_TEST_HOST, ip) == 1;
    }
    idleWork();

    // 5. web check (decides "up" and catches login pages)
    if (dnsOK) {
      HTTPClient http;
      http.setTimeout(HTTP_TIMEOUT_MS);
      http.setConnectTimeout(HTTP_TIMEOUT_MS);
      unsigned long t0 = millis();
      http.begin(TEST_URL);
      httpCode = http.GET();
      webMs = millis() - t0;
      http.end();
    } else {
      httpCode = 0;
      webMs = -1;
    }
    internetOK    = (httpCode == 204 || httpCode == 200);
    captivePortal = (httpCode == 301 || httpCode == 302 || httpCode == 307 || httpCode == 308);
    if (!internetOK) webMs = -1;

    // 6. public IP: every few minutes, and straight after an outage
    if (internetOK && (wanLastCheck == 0 || millis() - wanLastCheck > PUBLIC_IP_EVERY_MS || outageJustEnded)) checkPublicIp();
    outageJustEnded = false;
  } else {
    latencyMs = pingMaxMs = gwMs = webMs = -1;
  }
  testsRun++;
  totPingSent += pingSent;
  totPingLost += pingLost;
  updateJitter();
  updateLoss();
  calculateStatus();
  trackOutage();
  addTrendSample();
  logSample();
  sdLogTest();
  flushEvents();
  updateTimeString();
  logMsg("Ping %ld/%ld ms (worst %ld)  Loss %d/%d (%.1f%% recent)  Jitter %ld  Router %ld  ISP %ld  DNS %ld  Web %ld HTTP %d  RSSI %d  %d%%  %s\n",
         latencyMs, ping2Ms, pingMaxMs, pingLost, pingSent, lossPct, jitterMs, gwMs, ispMs, dnsMs, webMs, httpCode, wifiRSSI, health, statusText.c_str());
}

// Jitter = how much the ping changes from one reply to the next, averaged over the last
// JITTER_SAMPLES replies. Steady 30 ms pings -> ~0 jitter; 15, 60, 22, 90 -> ~50 ms jitter.
void updateJitter() {
  if (!wifiOK) { jitCount = 0; jitIndex = 0; jitterMs = -1; return; }   // fresh start after Wi-Fi drops
  if (jitCount < 3) { jitterMs = -1; return; }
  long sum = 0;
  int first = (jitIndex - jitCount + JITTER_SAMPLES) % JITTER_SAMPLES;
  for (int i = 1; i < jitCount; i++) {
    int a = jitPing[(first + i - 1) % JITTER_SAMPLES], b = jitPing[(first + i) % JITTER_SAMPLES];
    sum += abs(b - a);
  }
  jitterMs = (sum + (jitCount - 1) / 2) / (jitCount - 1);
}

// Packet loss over the last LOSS_WINDOW tests (with Wi-Fi up)
void updateLoss() {
  if (!wifiOK || pingSent == 0) return;           // Wi-Fi down is counted as downtime, not loss
  lossSentW[lossIndex] = pingSent;
  lossLostW[lossIndex] = pingLost;
  lossIndex = (lossIndex + 1) % LOSS_WINDOW;
  if (lossCount < LOSS_WINDOW) lossCount++;
  int s = 0, l = 0;
  for (int i = 0; i < lossCount; i++) { s += lossSentW[i]; l += lossLostW[i]; }
  lossPct = s ? 100.0f * l / s : -1;
}

// One place decides status text, health and colour level - everything else follows it.
void calculateStatus() {
  if (!wifiOK) {
    statusText = "DOWN"; health = 0; level = LV_BAD;
    return;
  }
  int score = 100;
  if (!dnsOK)        score -= 25;
  if (!internetOK)   score -= 50;
  if (captivePortal) score -= 35;
  if (wifiRSSI < -67) score -= 8;
  if (wifiRSSI < -75) score -= 15;
  if (wifiRSSI < -85) score -= 25;
  if (internetOK) {
    if (latencyMs > PING_WARN_MS) score -= 20;
    if (latencyMs > PING_BAD_MS)  score -= 25;
    if (jitterMs > JITTER_WARN_MS) score -= 15;
    if (jitterMs > JITTER_BAD_MS)  score -= 20;
    if (lossPct > LOSS_WARN_PCT) score -= 20;
    if (lossPct > LOSS_BAD_PCT)  score -= 25;
  }
  health = constrain(score, 0, 100);

  if (captivePortal)                  { statusText = "LOGIN";        level = LV_BAD;  }
  else if (!dnsOK)                    { statusText = "DNS FAIL";     level = LV_BAD;  }
  else if (!internetOK)               { statusText = "NO WEB";       level = LV_BAD;  }
  else if (lossPct > LOSS_BAD_PCT)    { statusText = "LOSS " + fmtLoss(lossPct); level = LV_BAD; }
  else if (latencyMs > PING_BAD_MS)   { statusText = "HIGH PING " + String(latencyMs) + " ms"; level = LV_BAD; }
  else if (wifiRSSI < -85)            { statusText = "WEAK WIFI";    level = LV_BAD;  }
  else if (jitterMs > JITTER_BAD_MS)  { statusText = "JITTER " + String(jitterMs) + " ms"; level = LV_BAD; }
  else if (lossPct > LOSS_WARN_PCT)   { statusText = "LOSS " + fmtLoss(lossPct); level = LV_WARN; }
  else if (jitterMs > JITTER_WARN_MS) { statusText = "JITTER " + String(jitterMs) + " ms"; level = LV_WARN; }
  else if (latencyMs > PING_WARN_MS)  { statusText = "SLOW " + String(latencyMs) + " ms";  level = LV_WARN; }
  else if (wifiRSSI < -75)            { statusText = "LOW SIGNAL";   level = LV_WARN; }
  else                                { statusText = "STABLE :)";    level = LV_GOOD; }
}

void addTrendSample() {
  trendHealth[trendIndex] = health;
  trendLevel[trendIndex] = level;
  trendIndex = (trendIndex + 1) % TREND_SIZE;
  if (trendCount < TREND_SIZE) trendCount++;
}

void updateTimeString() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) { timeText = "--:--"; return; }
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
  timeText = buf;
}

String qualityFromRSSI(int rssi) {
  if (rssi > -55) return "Excellent";
  if (rssi > -67) return "Good";
  if (rssi > -75) return "Medium";
  if (rssi > -85) return "Weak";
  return "Critical";
}

String wifiStatusName(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "SSID NOT FOUND";
    case WL_SCAN_COMPLETED:  return "SCAN OK";
    case WL_CONNECTED:       return "OK";
    case WL_CONNECT_FAILED:  return "FAILED";
    case WL_CONNECTION_LOST: return "LOST";
    case WL_DISCONNECTED:    return "WAITING";
    default:                 return "State " + String((int)st);
  }
}
