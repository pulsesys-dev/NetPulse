# NetPulse — network health logger (v2.3)

Cyberpunk-styled internet quality monitor and logger by Pulse Systems, built on Francesco Landi's
Internet Connection Monitor 24H (v1.0). Made to collect evidence for your ISP. Runs on:

| Board | Screen | Touch | Build |
|---|---|---|---|
| ESP32-2432S028R (CYD 2.8") | 240x320 ILI9341 | XPT2046 resistive | `cyd` / `#define BOARD_CYD` |
| Guition JC4827W543**C** | 272x480 NV3041A | GT911 capacitive | `guition_c` / `#define BOARD_GUITION_C` |
| Guition JC4827W543**R** | 272x480 NV3041A | XPT2046 resistive | `guition_r` / `#define BOARD_GUITION_R` |

Tap anywhere to cycle the pages (squares in the header show where you are); hold is used for the update prompt. To switch between
portrait and landscape: flasher > **Connect** > **Settings** > Orientation > **Save to board** (no reflash).

| Page | What it shows |
|---|---|
| **LIVE** | Health gauge, status, ping / Wi-Fi / 24 h uptime, last 10 minutes with loss + jitter, and the path out of your house along the bottom: `RTR` (your router) > `ISP` (their first router) > `DNS` |
| **24 HOURS** | 1-minute history: quality + ping (internet, ISP router, your router), uptime, average ping, packet loss |
| **7 DAYS** | 15-minute history with weekday labels |
| **14 DAYS** | 15-minute history with date labels |
| **EVENTS** | Outages in the last 24 h, downtime, your public IP, and a list of outages, IP changes, Wi-Fi drops, access-point changes and restarts |
| **DEVICE** | Stats: Wi-Fi, signal, IPs, your router, the ISP's router, public IP, firmware and update status, uptime, why it last restarted, memory, storage, tests and pings since start. **Hold** for SETTINGS |

### Settings (on the board)
On the DEVICE page, **hold** to open SETTINGS. **Tap** moves to the next item, **hold** changes it (or runs it).
Everything is saved straight away and survives restarts and updates.

| Setting | Choices |
|---|---|
| Orientation | Portrait, landscape, either flipped (turns immediately) |
| Brightness | 25 / 50 / 75 / 100 % |
| Night dimming | Off, 22:00-06:00, 23:00-07:00, 21:00-07:00 |
| Status LED | On / off (CYD only) |
| Ping target | 1.1.1.1, 8.8.8.8 or 9.9.9.9 (the second target is picked to match) |
| microSD log | On / off |
| Auto updates | Daily check / off |
| Check for update | Asks GitHub now |
| Install vX | Shown when an update is waiting |
| Clear history | Hold twice (within 5 s) |
| Restart / Back | |

The menu closes by itself after a minute without a touch.

History and the last 48 events are saved to the board's flash, so they survive reboots, power cuts
and firmware updates (unless you tick *Erase everything first* or click *Clear history* in the flasher).

## What's measured every 10 seconds
| Measurement | How | Why it helps with your ISP |
|---|---|---|
| **Ping** | 5 real ICMP pings to `PING_HOST` (1.1.1.1) | The headline number, same as `ping` on a PC |
| **Second target** | 5 pings to `PING_HOST2` (8.8.8.8), interleaved | Both bad at once = your line, not one website |
| **Packet loss** | Pings with no reply within 1 s (both targets) | The clearest sign of a faulty line |
| **Jitter** | Average change between consecutive ping replies | Why calls and games stutter |
| **Your router** | 2 pings to your gateway | Stays low while the rest is bad = the problem is past your house |
| **ISP's first router** | Found automatically with TTL-limited pings, then timed every test | Pins the problem on the ISP's own network |
| **DNS** | One lookup sent straight to your DNS server (past the board's cache) | Slow / failing DNS makes "the internet" feel broken |
| **Web check** | Loads a tiny Google page | Decides *up / down* and catches Wi-Fi login pages |
| **Public IP** | Asks `api.ipify.org` every 5 min and after outages | A changed IP = the line dropped and reconnected |
| **Wi-Fi** | Signal, drops with the reason code, access-point / channel changes | Rules out the Wi-Fi (and the board) as the cause |
| **Restarts** | Why the board last started (power, crash, watchdog, brownout...) | Shows gaps aren't the device's fault |

Status examples: `LOSS 2.7%`, `JITTER 34 ms`, `SLOW 72 ms`, `HIGH PING 140 ms`, `NO WEB`, `DNS FAIL`.
Some ISP routers don't answer test pings: then `ISP` shows `--` and everything else still works.

At night (22:00–06:00) the screen dims; the first tap wakes it for 30 s, then the next tap switches
pages. On the CYD the rear RGB LED shows green / yellow / red. The Guition boards have no LED.

## microSD log (your evidence)
Put a FAT32 microSD card in the board's slot. Everything opens straight in Excel:

| File | Contents |
|---|---|
| `NetPulse/tests/2026-11-17.csv` | Every test (every 10 s), one file per day: ping, worst ping, loss for both targets, jitter, router / ISP / DNS times, web check, Wi-Fi signal, health, status |
| `NetPulse/outages.csv` | One row per outage: start, end, duration, cause (Wi-Fi / internet / DNS / login page) |
| `NetPulse/events.csv` | Every event: outages, back online, public IP changes, Wi-Fi drops (with reason), access-point changes, ISP router found, restarts (with reason) |

- A green **SD** tag next to the clock = logging. Red = the card stopped working (it retries every 30 s).
  No tag = no card. A card can be put in at any time.
- Files are closed after every row, so you can pull the card whenever you like. About 1.5 MB per day.
- Times are local (your timezone setting); `unix` is the same moment as a Unix timestamp.
- The flasher shows the card status when you Connect.
- Turn it off with `SD_LOG 0` in `config.h`. If a card won't mount, try `SD_SPI_HZ 4000000`.
- v2.0 writes tests into the `tests` folder; any `.csv` files from v1.6 in `NetPulse/` use the old columns.

## Updates over Wi-Fi (from GitHub)
The board checks `UPDATE_REPO` (default `pulsesys-dev/NetPulse`) about 45 s after it starts and
then once a day. If there's a newer release it shows **UPDATE AVAILABLE** with the release notes:

- **Tap** anywhere to switch between **UPDATE** and **IGNORE**, **hold** (about 1 s, the bar fills) to choose.
- **UPDATE** downloads and installs it (progress on screen, ~30-60 s), then restarts. History, events,
  Wi-Fi and settings are kept. If anything goes wrong it stays on the current version.
- **IGNORE** skips that version: you won't be asked again until a newer release comes out.
- No answer within a minute: the prompt closes and a pink **UPD** tag stays next to the clock.
  **Hold anywhere** to bring the prompt back.
- The Events page logs every firmware change (`FIRMWARE v2.0.0 > v2.1.0`).

**One-time step:** v2.3 uses its own flash layout (two firmware slots for Wi-Fi updates + 640 KB for
history). Flash v2.3 **once over USB** with the web flasher (this clears the on-board history once; the
microSD files are untouched). Every update after that can come over Wi-Fi.

### Publishing a release
1. Set `FW_VERSION` in `NetPulse/NetPulse.ino`, e.g. `"v2.2.0"` (`"b2.2.0"` for a beta), commit and push.
2. On github.com: **Releases > Draft a new release**, type the tag (`v2.2.0`) and pick *Create new tag on
   publish*, write the notes (these show on the board's update prompt), tick *Set as a pre-release* for a
   beta, then **Publish**. (Or with git: `git tag -a v2.2.0 -m "notes"` and `git push origin v2.2.0`.)
3. GitHub Actions (`.github/workflows/release.yml`) builds all three boards and attaches
   `NetPulse-cyd-ota.bin`, `-guition_c-ota.bin`, `-guition_r-ota.bin` (Wi-Fi updates),
   `NetPulse-Flasher.html` (USB installs) and the source zip. Takes about 5 minutes; boards
   see it at their next daily check or restart.

Beta tags (`b...`) are published as pre-releases: only boards running beta firmware take them.
Turn checking off with `UPDATE_CHECK 0`.

## Web flasher (easiest)
1. **Make the flasher (once per firmware version):** double-click `build_flasher.command` (Mac) or
   `build_flasher.bat` (Windows). It installs PlatformIO if needed, builds all three boards and writes
   **`NetPulse-Flasher.html`** next to it: one file with all firmware inside.
2. Open `NetPulse-Flasher.html` in **Chrome or Edge** (desktop).
3. **Connect** and choose the USB port. If the firmware is already on the board it's detected
   (board, version, saved Wi-Fi) and the right board is picked for you.
4. **Flash** (skip if already flashed): pick the board, click Flash. You stay connected afterwards.
5. **Settings**: Wi-Fi, timezone and **orientation** (portrait / landscape / either upside-down),
   then **Save to board**. Leave Wi-Fi blank to keep the saved network and only change timezone/orientation.

The Serial monitor (under Connect) shows the board's live log. Only one program can use the
USB port at a time, so close Arduino IDE / PlatformIO serial monitors first.

The HTML is self-contained: send it to anyone and they can flash and set up their own board.
Reflashing an update keeps the saved Wi-Fi unless you tick "Erase everything first".
A board with no Wi-Fi saved shows a "Wi-Fi setup" screen and waits for the flasher.

## Manual build
Wi-Fi is normally set from the flasher. If you build manually you can still put a fallback
`WIFI_SSID` / `WIFI_PASS` in `NetPulse/config.h`. Build with **one** of these:

**PlatformIO:** `pio run -e cyd -t upload` (or `guition_c` / `guition_r`). The board is picked by the environment.

**Arduino IDE:** open `NetPulse/NetPulse.ino`, uncomment your board at the top of `config.h`,
and install the library **GFX Library for Arduino** (Moon On Our Nation).
- CYD: board *ESP32 Dev Module*, Partition Scheme *Custom* (it uses `partitions.csv` in the sketch folder)
- Guition: board *ESP32S3 Dev Module*, Flash Size *4MB*, PSRAM *OPI PSRAM*, Partition Scheme *Custom* (it uses `partitions.csv` in the sketch folder),
  **USB CDC On Boot: Enabled** (its USB-C is the S3's built-in USB; the build stops with a reminder if this is off)

Works on ESP32 Arduino core 2.x and 3.x.

## Options (config.h)
- `DEFAULT_ORIENTATION`: 0 portrait, 1 landscape, 2/3 upside-down (the flasher can change it per board)
- `CYD_USE_ST7789`: for CYD variants with the ST7789 panel
- `PING_HOST`, `PING_HOST2`, `PING_COUNT`, `PING_GAP_MS`, `PING_TIMEOUT_MS`: what gets pinged and how
- `ISP_MAX_TTL`: how many hops to search for the ISP's first router
- `PUBLIC_IP_URL`, `PUBLIC_IP_EVERY_MS`: public IP check
- `PING_WARN_MS` / `PING_BAD_MS`: yellow and red thresholds (50 / 100 ms)
- `LOSS_WINDOW`, `LOSS_WARN_PCT` / `LOSS_BAD_PCT`: loss window and thresholds (5 min, 1 / 5 %)
- `UPDATE_CHECK`, `UPDATE_REPO`, `UPDATE_CHECK_MS`, `TOUCH_HOLD_MS`: firmware updates and the hold time
- `WATCHDOG_S`: restart the board if the main loop is ever stuck this long (60 s)
- `JITTER_SAMPLES`, `JITTER_WARN_MS` / `JITTER_BAD_MS`: jitter window and thresholds (20 replies, 30 / 80 ms)
- Night hours, brightness levels, LED brightness and yellow mix

## Licence
Original design © 2026 Francesco Landi. Free for personal and educational use. Commercial
redistribution or sale of modified versions requires permission from the original author.

## Fonts
The screens use Chakra Petch (SIL Open Font License, `fonts/OFL.txt`), converted to 4-bit
anti-aliased fonts in `fonts.h`. `tools/make_aa_fonts.py` regenerates them
(`pip install freetype-py`, then `python3 tools/make_aa_fonts.py NetPulse/fonts.h fonts`).
