#pragma once
// =====================================================================================
//  Board definitions - pins, display driver and touch controller per board
// =====================================================================================
#include <Arduino_GFX_Library.h>

#if defined(BOARD_CYD)
  // ---------------- ESP32-2432S028R "Cheap Yellow Display" 2.8" 240x320 ----------------
  #define BOARD_NAME      "CYD 2.8\" ESP32-2432S028R"
  #define TFT_BL          21
  #define DISPLAY_SPEED   40000000

  // Touch: XPT2046 bit-banged on its own pins (no conflict with the display SPI bus)
  #define TOUCH_XPT2046   1
  #define XPT_SCK         25
  #define XPT_MOSI        32
  #define XPT_MISO        39
  #define XPT_CS          33

  // Rear RGB LED, active LOW
  #define HAS_RGB_LED     1
  #define LED_R_PIN       4
  #define LED_G_PIN       16
  #define LED_B_PIN       17

  // microSD slot: its own SPI pins (VSPI), separate from the display
  #define SD_CS_PIN       5
  #define SD_SCK          18
  #define SD_MISO         19
  #define SD_MOSI         23
  #define SD_SPI_HOST     VSPI

  #define UPDATE_ASSET    "NetPulse-cyd-ota.bin"      // this board's file in GitHub releases

  // orient: 0 portrait, 1 landscape, 2 portrait upside-down, 3 landscape upside-down
inline uint8_t panelRotation(uint8_t orient) { return orient & 3; }       // panel is natively portrait
inline Arduino_GFX *createDisplay(uint8_t orient) {
    // DC, CS, SCK, MOSI, MISO, SPI host
    Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 15, 14, 13, 12, HSPI);
  #ifdef CYD_USE_ST7789
    return new Arduino_ST7789(bus, GFX_NOT_DEFINED, panelRotation(orient), true, 240, 320);
  #else
    return new Arduino_ILI9341(bus, GFX_NOT_DEFINED, panelRotation(orient), false);
  #endif
  }

#elif defined(BOARD_GUITION_C) || defined(BOARD_GUITION_R)
  // ---------------- Guition JC4827W543 4.3" 480x272 NV3041A (QSPI) -> 272x480 portrait ----
  #define TFT_BL          1
  #define DISPLAY_SPEED   GFX_NOT_DEFINED

  // The Guition's USB-C goes to the ESP32-S3's built-in USB, so Serial has to be the USB port
  // or the web flasher can't talk to the firmware.
  #if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
    #error "Guition: set Tools > USB CDC On Boot > Enabled (Arduino IDE). PlatformIO sets it already."
  #endif

  // microSD slot (SPI mode). The display uses the other SPI host for its QSPI bus.
  #define SD_CS_PIN       10
  #define SD_SCK          12
  #define SD_MISO         13
  #define SD_MOSI         11
  #define SD_SPI_HOST     HSPI

  #if defined(BOARD_GUITION_C)
    #define BOARD_NAME    "Guition JC4827W543C"
    #define UPDATE_ASSET  "NetPulse-guition_c-ota.bin"
    // Capacitive GT911 on I2C
    #define TOUCH_GT911   1
    #define GT_SDA        8
    #define GT_SCL        4
    #define GT_INT        3
    #define GT_RST        38
  #else
    #define BOARD_NAME    "Guition JC4827W543R"
    #define UPDATE_ASSET  "NetPulse-guition_r-ota.bin"
    // Resistive XPT2046 - on the SAME pins as the microSD slot, so the touch chip is driven
    // through the shared hardware SPI bus (its own CS 38) instead of bit-banging
    #define TOUCH_XPT2046 1
    #define TOUCH_SHARES_SD_BUS 1
    #define XPT_SCK       12
    #define XPT_MOSI      11
    #define XPT_MISO      13
    #define XPT_CS        38
  #endif

  // orient: 0 portrait, 1 landscape, 2 portrait upside-down, 3 landscape upside-down
inline uint8_t panelRotation(uint8_t orient) {                            // panel is natively landscape
    static const uint8_t rot[4] = { 1, 0, 3, 2 };
    return rot[orient & 3];
  }
inline Arduino_GFX *createDisplay(uint8_t orient) {
    // CS, SCK, D0, D1, D2, D3
    Arduino_DataBus *bus = new Arduino_ESP32QSPI(45, 47, 21, 48, 40, 39);
    return new Arduino_NV3041A(bus, GFX_NOT_DEFINED, panelRotation(orient), true);
  }

#else
  #error "No board selected - see config.h"
#endif
