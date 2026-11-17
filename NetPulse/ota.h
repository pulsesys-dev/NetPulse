#ifndef OTA_H
#define OTA_H
// =====================================================================================
//  Firmware updates from GitHub releases
//
//  Push a tag to the repo (e.g. v2.1.0) and the release workflow builds the firmware and
//  attaches one "-ota.bin" per board. The board asks GitHub for the newest release, compares
//  version numbers and, if it's newer, downloads that board's file straight into the spare
//  firmware slot. The new firmware is checked before it's used; if anything goes wrong the
//  board keeps running the old version.
//
//  Versions: "v2.1.0" = official release, "b2.1.0" = beta (published as a GitHub pre-release).
//  Official firmware only looks at official releases; beta firmware takes betas too.
// =====================================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

struct OtaInfo {
  String tag;        // e.g. "v2.1.0"
  String url;        // download address of this board's -ota.bin
  String notes;      // release notes (plain text, trimmed)
};

// "v1.10.2" / "b1.10.2" -> one comparable number; at the same number an official release ranks
// above its beta, so a beta board moves on to the official version when it's out
uint32_t otaRank(const char *v) {
  bool official = !(*v == 'b' || *v == 'B');
  if (isalpha((unsigned char)*v)) v++;
  unsigned a = 0, b = 0, c = 0;
  sscanf(v, "%u.%u.%u", &a, &b, &c);
  return (a * 1000000UL + b * 1000UL + c) * 2 + (official ? 1 : 0);
}
bool otaIsBeta(const char *v) { return *v == 'b' || *v == 'B'; }

// value of "key":"..." starting at `from` (JSON string unescaped: \n, \", \\, \uXXXX dropped)
static String otaJsonStr(const String &js, const char *key, int from, int to, int *at = nullptr) {
  String k = String("\"") + key + "\":";
  int p = js.indexOf(k, from);
  if (p < 0 || (to >= 0 && p >= to)) return "";
  p += k.length();
  while (p < (int)js.length() && js[p] == ' ') p++;
  if (p >= (int)js.length() || js[p] != '"') return "";
  p++;
  String out;
  while (p < (int)js.length() && js[p] != '"') {
    char c = js[p++];
    if (c == '\\' && p < (int)js.length()) {
      char e = js[p++];
      if (e == 'n') out += '\n';
      else if (e == 'r' || e == 't') out += ' ';
      else if (e == 'u') p += 4;              // emoji etc.: skip
      else out += e;
    } else out += c;
  }
  if (at) *at = p;
  return out;
}

static bool otaJsonTrue(const String &js, const char *key, int from, int to) {
  String k = String("\"") + key + "\":true";
  int p = js.indexOf(k, from);
  return p >= 0 && (to < 0 || p < to);
}

// Asks GitHub. Returns 1 = newer release found (info filled), 0 = up to date / none yet, -1 = error.
int otaCheck(const char *repo, const char *asset, const char *current, OtaInfo &info, String &err) {
  WiFiClientSecure client;
  client.setInsecure();                       // the firmware itself is verified before it's used
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(12000);
  http.setUserAgent("NetPulse/" + String(current) + " (ESP32)");
  bool beta = otaIsBeta(current);
  String url = String("https://api.github.com/repos/") + repo + (beta ? "/releases?per_page=3" : "/releases/latest");
  if (!http.begin(client, url)) { err = "couldn't reach GitHub"; return -1; }
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code == 404) { http.end(); err = "no releases yet"; return 0; }
  if (code != 200) { http.end(); err = "GitHub answered " + String(code); return -1; }
  String js = http.getString();
  http.end();
  if (!js.length()) { err = "empty reply"; return -1; }

  // walk the release(s): each starts at its "tag_name"
  String bestTag; int bestAt = -1, bestEnd = -1;
  int p = 0;
  while (true) {
    int t = js.indexOf("\"tag_name\":", p);
    if (t < 0) break;
    int next = js.indexOf("\"tag_name\":", t + 10);
    String tag = otaJsonStr(js, "tag_name", t, next);
    bool draft = otaJsonTrue(js, "draft", t, next);
    if (tag.length() && !draft && (!bestTag.length() || otaRank(tag.c_str()) > otaRank(bestTag.c_str()))) {
      bestTag = tag; bestAt = t; bestEnd = next;
    }
    if (next < 0) break;
    p = next;
  }
  if (!bestTag.length()) { err = "no releases yet"; return 0; }
  if (otaRank(bestTag.c_str()) <= otaRank(current)) { err = ""; return 0; }

  // this board's file among the release's assets
  String want = String("/") + asset;
  int a = bestAt;
  while (true) {
    int at = -1;
    String u = otaJsonStr(js, "browser_download_url", a, bestEnd, &at);
    if (!u.length()) break;
    if (u.endsWith(want)) { info.url = u; break; }
    a = at;
  }
  if (!info.url.length()) { err = bestTag + " has no file for this board"; return -1; }
  info.tag = bestTag;
  String n = otaJsonStr(js, "body", bestAt, bestEnd);
  // markdown -> plain lines
  n.replace("**", ""); n.replace("##", ""); n.replace("# ", ""); n.replace("* ", "- ");
  if (n.length() > 600) n = n.substring(0, 600);
  info.notes = n;
  return 1;
}

// Downloads and installs. progress(pct) is called as it goes (also to keep the screen and
// watchdog alive). Returns true when the new firmware is in place (then restart).
bool otaInstall(const String &url, const char *current, void (*progress)(int pct), String &err) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub hands the file over from another address
  http.setUserAgent("NetPulse/" + String(current) + " (ESP32)");
  if (!http.begin(client, url)) { err = "couldn't start the download"; return false; }
  int code = http.GET();
  if (code != 200) { http.end(); err = "download failed (" + String(code) + ")"; return false; }
  int len = http.getSize();
  if (len <= 0) { http.end(); err = "download has no size"; return false; }
  if (!Update.begin(len, U_FLASH)) { http.end(); err = "no room - flash once over USB with the new flasher"; return false; }
  WiFiClient *s = http.getStreamPtr();
  static uint8_t buf[2048];
  int got = 0, lastPct = -1;
  uint32_t lastData = millis();
  while (got < len && http.connected()) {
    int avail = s->available();
    if (avail > 0) {
      int n = s->readBytes(buf, min((int)sizeof(buf), avail));
      if (n > 0 && Update.write(buf, n) != (size_t)n) break;
      got += n;
      lastData = millis();
      int pct = (int)((int64_t)got * 100 / len);
      if (pct != lastPct) { lastPct = pct; if (progress) progress(pct); }
    } else {
      if (millis() - lastData > 20000) break;
      if (progress) progress(lastPct);
      delay(5);
    }
  }
  http.end();
  if (got != len) { Update.abort(); err = "download was cut short - still on the old version"; return false; }
  if (!Update.end(true)) { err = "the new firmware didn't check out - still on the old version"; return false; }
  return true;
}
#endif
