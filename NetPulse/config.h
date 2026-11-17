#pragma once
// =====================================================================================
//  NetPulse (based on Internet Connection Monitor 24H) - USER SETTINGS
//  Portrait build for ESP32-2432S028R (CYD 2.8") and Guition JC4827W543 C / R (4.3")
// =====================================================================================

// ---- 1. PICK YOUR BOARD (Arduino IDE only - PlatformIO sets this per environment) ----
#if !defined(BOARD_CYD) && !defined(BOARD_GUITION_C) && !defined(BOARD_GUITION_R)
  #define BOARD_CYD            // ESP32-2432S028R Cheap Yellow Display 2.8"
  // #define BOARD_GUITION_C   // JC4827W543C  - 4.3" capacitive (GT911)
  // #define BOARD_GUITION_R   // JC4827W543R  - 4.3" resistive  (XPT2046)
#endif

// CYD variant with the ST7789 panel (some 2-USB-port CYDs). Leave off for the normal ILI9341.
// #define CYD_USE_ST7789

// Default screen orientation (the web flasher can change it):
//   0 = portrait   1 = landscape   2 = portrait upside-down   3 = landscape upside-down
#define DEFAULT_ORIENTATION 0

// ---- 2. WI-FI -----------------------------------------------------------------------
// Normally set from the web flasher (stored on the board). These are only fallbacks
// used when nothing has been saved yet - leave them as-is for a shareable firmware.
#define WIFI_SSID "YOUR WIFI NAME"
#define WIFI_PASS "YOUR WIFI PASSWORD"

// ---- 3. TIME (default - the web flasher can change it) ------------------------------------------------------------------------
// POSIX timezone string. Examples:
//   Brisbane (no DST)           "AEST-10"
//   Sydney / Melbourne          "AEST-10AEDT,M10.1.0,M4.1.0/3"
//   Adelaide                    "ACST-9:30ACDT,M10.1.0,M4.1.0/3"
//   Perth                       "AWST-8"
//   New Zealand                 "NZST-12NZDT,M9.5.0,M4.1.0/3"
//   UK / Ireland                "GMT0BST,M3.5.0/1,M10.5.0/2"
//   Central Europe              "CET-1CEST,M3.5.0,M10.5.0/3"
//   US Pacific                  "PST8PDT,M3.2.0,M11.1.0"
//   US Eastern                  "EST5EDT,M3.2.0,M11.1.0"
#define MY_TIMEZONE "AEST-10"
#define NTP_SERVER_1 "au.pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define NTP_SERVER_3 "pool.ntp.org"

// ---- 4. TEST SETTINGS ---------------------------------------------------------------
// Real ping (ICMP, like the ping command on your PC). Use an IP so DNS problems don't hide
// in the ping figure. 1.1.1.1 = Cloudflare, 8.8.8.8 = Google.
#define PING_HOST         "1.1.1.1"   // main target (shown on screen)
#define PING_HOST2        "8.8.8.8"   // second target: both bad at once = your line, not one site
#define ISP_MAX_TTL       8           // how far to look for the ISP's first router
#define PING_COUNT        5         // pings per target per test (every 10 s)
#define PING_GAP_MS       150       // between ping rounds in a test
#define PING_TIMEOUT_MS   1000      // no reply within this = lost packet
#define LOSS_WINDOW       30        // status / health use the loss over the last 30 tests (5 min)
#define LOSS_WARN_PCT     1.0       // above this = yellow  (LOSS 2.7%)
#define LOSS_BAD_PCT      5.0       // above this = red

#define PUBLIC_IP_URL     "http://api.ipify.org"   // returns your public IP as plain text
#define PUBLIC_IP_EVERY_MS 300000UL                // check every 5 min (a change = the line reconnected)

// Web check: decides whether the internet is "up" and catches Wi-Fi login pages
#define TEST_URL          "http://clients3.google.com/generate_204"
#define DNS_TEST_HOST     "google.com"
#define TEST_INTERVAL_MS  10000UL   // internet test every 10 s
#define HTTP_TIMEOUT_MS   900       // short so touch stays responsive when the line is down
#define WIFI_RETRY_MS     60000UL
#define WATCHDOG_S        60        // restart the board if the main loop is stuck this long
#define PING_WARN_MS      50        // above this = yellow  (SLOW 72 ms)
#define PING_BAD_MS       100       // above this = red     (HIGH PING 140 ms)
#define JITTER_SAMPLES    20        // jitter = average change between the last 20 ping replies
#define JITTER_WARN_MS    30        // above this = yellow  (JITTER 34 ms)
#define JITTER_BAD_MS     80        // above this = red

// ---- 4b. MICROSD LOG ----------------------------------------------------------------
// With a card in, every test (every 10 s) is added to a CSV file, one file per day:
//   /NetPulse/2026-11-14.csv   (opens in Excel; FAT32 card, any size)
// Without a card everything else works as normal. A card can be put in any time.
#define SD_LOG            1         // 0 = never use the card
#define SD_SPI_HZ         10000000  // lower to 4000000 if a card won't mount

// ---- 4c. FIRMWARE UPDATES FROM GITHUB -------------------------------------------------
// The board checks the repo's releases at start-up and once a day. If there's a newer version
// it asks: UPDATE or IGNORE (ignored = not asked again until a newer release comes out).
// Tap switches the choice, hold confirms. Hold anywhere later to bring the question back.
#define UPDATE_CHECK      1                          // 0 = never check
#define UPDATE_REPO       "pulsesys-dev/NetPulse"
#define UPDATE_CHECK_MS   (24UL * 3600UL * 1000UL)   // once a day
#define TOUCH_HOLD_MS     1200                       // how long a "hold" is

// ---- 4d. WEB PAGE -----------------------------------------------------------------------
// Download the logs from a browser on the same Wi-Fi: http://netpulse.local (or the board's IP,
// shown on the DEVICE page). Read-only. Can also be switched off in SETTINGS.
#define WEB_SERVER        1
#define WEB_HOSTNAME      "netpulse"

// ---- 5. NIGHT DIMMING ---------------------------------------------------------------
#define NIGHT_START_HOUR  22        // dim from 22:00 ...
#define NIGHT_END_HOUR    6         // ... until 06:00
#define BRIGHT_DAY        220       // 0-255
#define BRIGHT_NIGHT      20        // 0-255
#define WAKE_DURATION_MS  30000UL   // a tap at night wakes the screen for 30 s
#define NIGHT_LED_OFF     1         // CYD: rear LED off while dimmed at night

// ---- 6. CYD REAR RGB LED ------------------------------------------------------------
#define LED_BRIGHTNESS    255       // 0-255 overall
#define LED_YELLOW_RED    90        // red mix for yellow (lower = greener yellow)
