#!/usr/bin/env python3
"""
Builds the firmware for every board and packs it into ONE self-contained HTML flasher:
    NetPulse-Flasher.html

Usage (or just double-click build_flasher.command on Mac / build_flasher.bat on Windows):
    python3 build_flasher.py                 # build all boards
    python3 build_flasher.py cyd guition_c   # only some boards
    python3 build_flasher.py --no-build      # re-pack what's already in .pio/build

Needs Python 3.8+. PlatformIO is installed automatically if it's missing.
"""
import base64
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(ROOT, "flasher", "flasher_template.html")
OUTPUT = os.path.join(ROOT, "NetPulse-Flasher.html")
ESPTOOL_JS_URL = "https://unpkg.com/esptool-js@0.4.5/bundle.js"

BOARDS = {
    # env        chip       bootloader offset
    "cyd":       ("ESP32",    0x1000),
    "guition_c": ("ESP32-S3", 0x0),
    "guition_r": ("ESP32-S3", 0x0),
}


def say(msg):
    print(msg, flush=True)


def pio_cmd():
    """Return a command list that runs PlatformIO, installing it if needed."""
    try:
        subprocess.run([sys.executable, "-m", "platformio", "--version"],
                       check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        say("PlatformIO not found - installing it (one-time, takes a minute)...")
        subprocess.run([sys.executable, "-m", "pip", "install", "--user", "-U", "platformio"], check=True)
    return [sys.executable, "-m", "platformio"]


def core_dir():
    return os.environ.get("PLATFORMIO_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")


def find_boot_app0():
    for fw in ("framework-arduinoespressif32", "framework-arduinoespressif32-libs"):
        p = os.path.join(core_dir(), "packages", fw, "tools", "partitions", "boot_app0.bin")
        if os.path.isfile(p):
            return p
    return None


def firmware_version():
    src = open(os.path.join(ROOT, "NetPulse", "NetPulse.ino"), encoding="utf-8").read()
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', src)
    return m.group(1) if m else "?"


def part(path, addr):
    data = open(path, "rb").read()
    return {"addr": addr, "size": len(data), "b64": base64.b64encode(data).decode("ascii")}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    no_build = "--no-build" in sys.argv
    envs = args or list(BOARDS)
    for e in envs:
        if e not in BOARDS:
            sys.exit(f"Unknown board '{e}'. Choose from: {', '.join(BOARDS)}")

    pio = None if no_build else pio_cmd()
    boards = {}
    for env in envs:
        chip, boot_off = BOARDS[env]
        if pio:
            say(f"\n=== Building {env} ({chip}) ===")
            r = subprocess.run(pio + ["run", "-e", env], cwd=ROOT)
            if r.returncode != 0:
                sys.exit(f"Build failed for {env} - see the errors above.")
        bdir = os.path.join(ROOT, ".pio", "build", env)
        files = {n: os.path.join(bdir, n) for n in ("bootloader.bin", "partitions.bin", "firmware.bin")}
        missing = [n for n, p in files.items() if not os.path.isfile(p)]
        if missing:
            sys.exit(f"{env}: missing {', '.join(missing)} in {bdir} (build it first).")
        boot_app0 = find_boot_app0()
        if not boot_app0:
            sys.exit("Couldn't find boot_app0.bin in the PlatformIO packages folder.")

        parts = [
            part(files["bootloader.bin"], boot_off),
            part(files["partitions.bin"], 0x8000),
            part(boot_app0, 0xE000),
            part(files["firmware.bin"], 0x10000),
        ]
        boards[env] = {"chip": chip, "parts": parts}
        say(f"{env}: firmware {parts[-1]['size'] / 1024:.0f} KB")

    payload = {
        "version": firmware_version(),
        "built": datetime.date.today().isoformat(),
        "boards": boards,
    }

    # Try to inline esptool-js so the flasher also works offline; fall back to the CDN.
    esptool_src = None
    try:
        say("Downloading esptool-js so the flasher works offline...")
        with urllib.request.urlopen(ESPTOOL_JS_URL, timeout=30) as resp:
            esptool_src = resp.read().decode("utf-8")
    except Exception as ex:  # noqa: BLE001
        say(f"  (couldn't download it: {ex} - the flasher will load it online instead)")

    def js(value):
        # JSON is valid JS; escape "</" so a "</script>" inside can't end the tag early
        return json.dumps(value, separators=(",", ":")).replace("</", "<\\/")

    html = open(TEMPLATE, encoding="utf-8").read()
    html = html.replace("/*FIRMWARE_JSON*/null", js(payload), 1)
    html = html.replace("/*ESPTOOL_SRC*/null", js(esptool_src), 1)
    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write(html)

    say(f"\nDone! {os.path.basename(OUTPUT)} ({os.path.getsize(OUTPUT) / 1024 / 1024:.1f} MB)")
    say("Open it in Chrome or Edge, pick your board, click Connect & Flash.")


if __name__ == "__main__":
    main()
