#pragma once
// Types used by NetPulse.ino. They live in a header because the Arduino / PlatformIO
// .ino step writes function prototypes ABOVE the sketch's own code, so any type that appears
// in a function signature has to be declared before the sketch starts.
#include <Arduino.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include "fonts.h"


// ---------------- Serial output that can never freeze the board ----------------
// The Guition's USB-C is the ESP32-S3's built-in USB serial. When it's plugged into a computer
// but nothing has the port open, the send buffer fills up and on ESP32 Arduino core 2.x a
// normal Serial.print then waits for space forever - the whole sketch stops (screen, clock,
// touch). So every message goes through here: it's only sent if it fits in the buffer (or fits
// within waitMs while a program is reading), otherwise it's dropped.
#include <stdarg.h>
inline void serialSend(uint32_t waitMs, const char *fmt, ...) {
  char buf[384];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
  if (waitMs == 0) {                               // log line: all of it fits now, or drop it
    if (Serial.availableForWrite() >= n) Serial.write((const uint8_t *)buf, n);
    return;
  }
  uint32_t t0 = millis();                          // reply: send in pieces as the reader drains it
  int sent = 0;
  while (sent < n) {
    int room = Serial.availableForWrite();
    if (room > 0) { int k = min(room, n - sent); Serial.write((const uint8_t *)buf + sent, k); sent += k; continue; }
    if (millis() - t0 >= waitMs) return;
    delay(2);
  }
}
#define logMsg(...)   serialSend(0, __VA_ARGS__)      // status / debug lines
#define replyMsg(...) serialSend(500, __VA_ARGS__)    // answers to the web flasher (it is reading)

extern bool fsOK;     // defined in the sketch: true when the history file system is mounted

enum Level : uint8_t { LV_GOOD = 0, LV_WARN = 1, LV_BAD = 2 };

enum Page : uint8_t { PAGE_HOME, PAGE_24H, PAGE_7D, PAGE_14D, PAGE_EVENTS, PAGE_DEVICE, PAGE_COUNT };

// ---------------- Event log (outages, IP changes, Wi-Fi drops ...) ----------------
enum EvType : uint8_t { EV_BOOT, EV_OUTAGE, EV_NET_BACK, EV_WIFI_LOST, EV_IP_CHANGE, EV_AP_CHANGE, EV_ISP_HOP, EV_FIRMWARE, EV_TYPES };
struct Event {
  uint32_t t;         // unix time, 0 = not known yet (no clock): filled in from ms once it is
  uint32_t ms;        // millis() when it happened (this boot)
  uint32_t dur;       // seconds (outage length), 0 = n/a
  uint8_t  type;
  uint8_t  saved;     // written to the microSD card
  char     detail[46];
};
enum Align : uint8_t { AL_LEFT, AL_CENTER, AL_RIGHT };

struct Box { int16_t x, y, w, h; int16_t r() const { return x + w; } int16_t b() const { return y + h; } };
struct ModalGeo { Box box, b0, b1, bar; };        // update prompt layout
struct SetGeo { int listY, rowH, rows, first, x, w; };   // SETTINGS menu layout
struct StatRow { const char *label; String value; uint16_t color; uint8_t kind; };   // DEVICE page: 0 row, 1 header, 2 optional, 3 hidden
enum SetItem : uint8_t { SI_ORIENT, SI_BRIGHT, SI_NIGHT, SI_LED, SI_PING, SI_SD, SI_WEB, SI_UPDCHK, SI_CHECKNOW, SI_INSTALL, SI_CLEAR, SI_RESTART, SI_BACK, SI_COUNT };

// What the web page shows: copied by the main loop after every test, read by the web task
struct WebSnap {
  char status[28], ssid[33], ip[16], wan[16], ispip[16], upd[16], host1[16], host2[16];
  int level, health, ping, ping2, jitter, router, isp, dns, rssi, sd, stack;
  float loss;
  bool wifi;
  uint32_t sdrows, tests, sent, lost;
};

// Recursive mutex holder: { Lock l(mutex); ... } (no-op until the mutex exists)
struct Lock {
  SemaphoreHandle_t h;
  explicit Lock(SemaphoreHandle_t x) : h(x) { if (h) xSemaphoreTakeRecursive(h, portMAX_DELAY); }
  ~Lock() { if (h) xSemaphoreGiveRecursive(h); }
};

struct Layout {
  int16_t W, H;
  bool big, land;
  int16_t m;                 // margin
  int16_t hdrH, footY, footH;
  Box hero, tiles, trend;
  bool tilesVertical;
  // gauge
  int16_t gx, gy, gr, gt;    // centre, outer radius, thickness
  int16_t statusY;           // top of status text
  // fonts
  const AAFont *fXS, *fS, *fM, *fL, *fXL;
};


// ---------------- History log ----------------
struct LogRec {
  uint8_t  qAvg;     // 0-100, 255 = no data
  uint8_t  qMin;
  uint8_t  up;       // % of tests in this slot where the web answered
  uint8_t  rsv;
  uint16_t pAvg;     // internet ping, ms (0xFFFF = none)
  uint16_t pMax;     // worst single ping, ms
  uint16_t loss;     // ping packet loss, 0.1 % units (0-1000), 0xFFFF = no pings sent
  uint16_t jit;      // jitter, ms
  uint16_t gw;       // router (gateway) ping, ms
  uint16_t p2;       // second internet target ping, ms
  uint16_t isp;      // ISP's first router, ms
  uint16_t dns;      // DNS lookup time, ms
};

// One internet test, as fed to the history
struct Sample {
  int  q;            // health 0-100
  bool up;           // web reachable
  int  ping;         // average ping, ms (-1 none)
  int  pingMax;      // worst ping in the burst
  int  sent, lost;   // ping packets
  int  jit;          // jitter ms (-1 none)
  int  gw;           // router ping ms (-1 none)
  int  p2;           // second target ping ms (-1 none)
  int  isp;          // ISP hop ms (-1 none)
  int  dns;          // DNS lookup ms (-1 none / failed)
};

struct LogHdr {
  uint32_t magic;
  uint16_t ver, cap;
  uint32_t slotSec;
  uint32_t lastSlot;
  uint16_t head, count;
};

static const uint32_t LOG_MAGIC = 0x474C4D49;   // "IMLG"
static const uint16_t LOG_VER = 3;              // v3: + second target, ISP hop, DNS time

// LittleFS must never fill up: on this ESP32 core a full LittleFS crashes the board
// (littlefs 2.9 divides by zero while reporting "no more free space"). Every save checks first.
inline bool fsRoomFor(size_t bytes) {
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  if (total > used && total - used >= bytes + 3 * 4096) return true;
  static uint32_t lastWarn = 0;
  if (!lastWarn || millis() - lastWarn > 600000UL) { lastWarn = millis(); logMsg("History storage nearly full - save skipped\n"); }
  return false;
}

class SlotLog {
public:
  SlotLog(const char *path, uint32_t slotSec, uint16_t cap, LogRec *buf)
    : _path(path), _slotSec(slotSec), _cap(cap), _buf(buf) {}

  uint32_t slotSec() const { return _slotSec; }
  uint16_t count() const { return _count; }

  // Saved as a small header file + one file per CHUNK records ("/h1m.hdr", "/h1m_00.bin" ...),
  // so saving a minute rewrites ~1 KB instead of the whole history (LittleFS copies on write).
  enum : uint16_t { CHUNK = 64 };                      // records per file (64 x 20 bytes)

  void begin() {
    resetRam();
    if (!fsOK) return;
    LogHdr h;
    bool ok = false;
    File f = LittleFS.open(hdrPath().c_str(), "r");
    if (f) {
      ok = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) && h.magic == LOG_MAGIC && h.ver == LOG_VER &&
           h.cap == _cap && h.slotSec == _slotSec && h.head < _cap && h.count <= _cap;
      f.close();
    }
    for (uint16_t c = 0; ok && c < chunks(); c++) {
      File cf = LittleFS.open(chunkPath(c).c_str(), "r");
      size_t len = chunkLen(c) * sizeof(LogRec);
      ok = cf && cf.read((uint8_t *)&_buf[c * CHUNK], len) == len;
      if (cf) cf.close();
    }
    if (ok) { _head = h.head; _count = h.count; _lastSlot = h.lastSlot; }
    else { resetRam(); saveAll(); }
    logMsg("Log %s: %u slots loaded\n", _path, (unsigned)_count);
  }

  void clear() { resetRam(); saveAll(); }

  // Feed one test result. Returns true when a finished slot was written.
  bool add(uint32_t now, const Sample &s) {
    uint32_t slot = now / _slotSec;
    bool committed = false;
    if (_n && slot != _curSlot) { commit(); committed = true; }
    if (!_n) { _curSlot = slot; _sumQ = 0; _minQ = 100; _nUp = 0; _sumP = 0; _nP = 0; _maxP = 0;
               _sent = 0; _lost = 0; _sumJ = 0; _nJ = 0; _sumG = 0; _nG = 0;
               _sum2 = 0; _n2 = 0; _sumI = 0; _nI = 0; _sumD = 0; _nD = 0; }
    _n++;
    _sumQ += s.q;
    if (s.q < _minQ) _minQ = s.q;
    if (s.up) _nUp++;
    if (s.ping >= 0) { _sumP += s.ping; _nP++; }
    if (s.pingMax > _maxP) _maxP = s.pingMax;
    _sent += s.sent; _lost += s.lost;
    if (s.jit >= 0) { _sumJ += s.jit; _nJ++; }
    if (s.gw >= 0) { _sumG += s.gw; _nG++; }
    if (s.p2 >= 0) { _sum2 += s.p2; _n2++; }
    if (s.isp >= 0) { _sumI += s.isp; _nI++; }
    if (s.dns >= 0) { _sumD += s.dns; _nD++; }
    return committed;
  }

  // Record for an absolute slot number (includes the slot still being filled).
  bool recAt(uint32_t slot, LogRec &r) const {
    if (_n && slot == _curSlot) { r = partial(); return true; }
    if (!_count || slot > _lastSlot || _lastSlot - slot >= _count) return false;
    r = _buf[(_head + _cap - 1 - (_lastSlot - slot)) % _cap];
    return r.qAvg != 255;
  }

private:
  const char *_path;
  uint32_t _slotSec;
  uint16_t _cap;
  LogRec *_buf;
  uint16_t _head = 0, _count = 0;
  uint32_t _lastSlot = 0;
  // accumulator for the slot in progress
  uint32_t _curSlot = 0, _sumQ = 0, _sumP = 0, _sumJ = 0, _sumG = 0, _sent = 0, _lost = 0;
  uint16_t _n = 0, _nUp = 0, _nP = 0, _maxP = 0, _nJ = 0, _nG = 0, _n2 = 0, _nI = 0, _nD = 0;
  uint32_t _sum2 = 0, _sumI = 0, _sumD = 0;
  uint8_t _minQ = 100;

  void resetRam() { _head = 0; _count = 0; _lastSlot = 0; _n = 0; }

  LogRec partial() const {
    LogRec r;
    r.qAvg = _n ? (uint8_t)(_sumQ / _n) : 255;
    r.qMin = _minQ;
    r.up   = _n ? (uint8_t)((_nUp * 100 + _n / 2) / _n) : 0;
    r.rsv  = 0;
    r.pAvg = _nP ? (uint16_t)min<uint32_t>(_sumP / _nP, 9999) : 0xFFFF;
    r.pMax = _nP ? _maxP : 0xFFFF;
    r.loss = _sent ? (uint16_t)((_lost * 1000 + _sent / 2) / _sent) : 0xFFFF;
    r.jit  = _nJ ? (uint16_t)min<uint32_t>(_sumJ / _nJ, 9999) : 0xFFFF;
    r.gw   = _nG ? (uint16_t)min<uint32_t>(_sumG / _nG, 9999) : 0xFFFF;
    r.p2   = _n2 ? (uint16_t)min<uint32_t>(_sum2 / _n2, 9999) : 0xFFFF;
    r.isp  = _nI ? (uint16_t)min<uint32_t>(_sumI / _nI, 9999) : 0xFFFF;
    r.dns  = _nD ? (uint16_t)min<uint32_t>(_sumD / _nD, 9999) : 0xFFFF;
    return r;
  }

  void push(const LogRec &r) {
    _buf[_head] = r;
    _head = (_head + 1) % _cap;
    if (_count < _cap) _count++;
  }

  void commit() {
    LogRec r = partial();
    _n = 0;
    if (_count && _curSlot <= _lastSlot) {               // clock stepped back: keep newest only
      if (_curSlot == _lastSlot) { _buf[(_head + _cap - 1) % _cap] = r; saveRec((_head + _cap - 1) % _cap); }
      return;
    }
    uint32_t gap = _count ? _curSlot - _lastSlot - 1 : 0;
    bool full = false;
    if (gap >= _cap) { resetRam(); gap = 0; full = true; }
    LogRec empty = { 255, 255, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    for (uint32_t i = 0; i < gap; i++) push(empty);
    if (gap > 4) full = true;
    push(r);
    _lastSlot = _curSlot;
    if (full) saveAll();
    else {
      for (uint32_t i = 0; i <= gap; i++) saveRec((_head + _cap - 1 - i) % _cap, i == gap);
    }
  }

  void header(LogHdr &h) const {
    h.magic = LOG_MAGIC; h.ver = LOG_VER; h.cap = _cap; h.slotSec = _slotSec;
    h.lastSlot = _lastSlot; h.head = _head; h.count = _count;
  }

  uint16_t chunks() const { return (_cap + CHUNK - 1) / CHUNK; }
  uint16_t chunkLen(uint16_t c) const { return min<uint16_t>(CHUNK, _cap - c * CHUNK); }
  String hdrPath() const { return String(_path) + ".hdr"; }
  String chunkPath(uint16_t c) const { char b[8]; snprintf(b, sizeof(b), "_%02u.bin", (unsigned)c); return String(_path) + b; }

  void saveHdr() {
    if (!fsOK || !fsRoomFor(sizeof(LogHdr))) return;
    LogHdr h; header(h);
    File f = LittleFS.open(hdrPath().c_str(), "w");
    if (!f) return;
    f.write((const uint8_t *)&h, sizeof(h));
    f.close();
  }

  void saveChunk(uint16_t c) {
    size_t len = chunkLen(c) * sizeof(LogRec);
    if (!fsOK || !fsRoomFor(len)) return;
    File f = LittleFS.open(chunkPath(c).c_str(), "w");
    if (!f) return;
    f.write((const uint8_t *)&_buf[c * CHUNK], len);
    f.close();
  }

  void saveRec(uint16_t idx, bool withHeader = true) {
    saveChunk(idx / CHUNK);
    if (withHeader) saveHdr();
  }

  void saveAll() {
    if (!fsOK) return;
    for (uint16_t c = 0; c < chunks(); c++) saveChunk(c);
    saveHdr();
  }
};

struct RangeDef { const char *title; const char *shortTitle; SlotLog *log; uint32_t span; };
struct RangeStats {
  uint32_t slots;     // slots with data
  float upPct;
  int avgPing;        // -1 none
  int worstPing;
  uint32_t downSec;
  float lossPct;      // -1 none
  int avgJit;         // -1 none
  int avgGw;          // -1 none
  int avgIsp;         // -1 none
  int avgDns;         // -1 none
};

