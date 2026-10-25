/*
 * Booth Display v2 for Waveshare ESP32-S3-Touch-AMOLED-1.75C
 * ==========================================================
 * Menu-driven booth app. From the menu you can reach:
 *   - Flipbook  : photos (if you ran convert_photos.py) or an intro slide,
 *                 auto-advancing like a kiosk slideshow.
 *   - QR Codes  : swipe through LinkedIn, vCard contact, and book-a-meeting
 *                 QR codes. Flip to the right one for who you're talking to.
 *   - Badge     : wearable badge mode. Tap to switch between an animated
 *                 "Got evals?" screen and a photo version (your first photo,
 *                 or a static "Got evals?" slide if you have no photos).
 *   - Clicker   : Bluetooth presentation remote. Tap right = next slide,
 *                 tap left = previous slide. Pair it as "Waveshare Clicker".
 *   - Clock     : analog, digital, or split-flap flip clock (tap or BOOT
 *                 button to cycle). The flip clock animates the changing
 *                 cards each minute and plays a synthesized "clack" through
 *                 the speaker (tap the top-right corner to mute it).
 *                 Time comes from NTP if Wi-Fi is connected (see Settings),
 *                 else from the sketch's compile time — so flash soon after
 *                 compiling if you skip Wi-Fi.
 *   - Settings  : Wi-Fi search and connect, with an on-screen keyboard for
 *                 passwords. The network is saved on the device, so the
 *                 clock's NTP sync can use it after a reboot. Also has
 *                 Sleep: deep sleep, woken only by the side BOOT button.
 *
 * Navigation (everywhere):
 *   - Tap a menu item to open it. The menu shows 4 big buttons at a time;
 *     swipe up/down (or press BOOT) to scroll through the list.
 *   - LONG-PRESS (hold ~1 second) anywhere returns to the menu.
 *   - BOOT button (side button) = next / toggle, depending on the mode.
 *   - In the flipbook, swipe left/right flips pages; tap right 2/3 = next,
 *     tap left 1/3 = previous. Flipbook pages also auto-advance
 *     (see AUTO_ADVANCE_MS); any touch restarts the timer. Set it to 0
 *     to turn auto-advance off.
 *
 * Libraries (Arduino IDE Library Manager):
 *   - "GFX Library for Arduino" (moononournation)
 *   - "SensorLib" (lewis he) - for the CST9217 touch chip
 * Clicker mode uses the ESP32 board package's built-in BLE HID support
 * (no extra library): needs ESP32 board package 3.x or newer.
 * Board settings (Tools menu):
 *   - Board: ESP32S3 Dev Module
 *   - USB CDC On Boot: Enable
 *   - Flash Size: 32MB (this board has 32MB flash)
 *   - PSRAM: OPI PSRAM
 *   - Partition Scheme: default is fine without photos; with several photos
 *     pick a large-APP scheme, e.g. "32MB (16MB APP/15MB FATFS)".
 */

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "TouchDrvCSTXXX.hpp"
#include "qrcodegen.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLECharacteristic.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include <esp_sleep.h>

// ---------------------------------------------------------------------------
// CONFIG - edit this section
// ---------------------------------------------------------------------------

// >>> YOUR LINKEDIN URL (the LinkedIn QR encodes exactly this string).
const char *LINKEDIN_URL = "https://www.linkedin.com/in/billgrant";

// >>> YOUR CONTACT CARD. Phones scan this straight into their contacts -
// no app, no typing. Fill in your real phone and email; keep the MECARD
// format (semicolons between fields, double semicolon at the end).
const char *VCARD_TEXT =
  "MECARD:N:Grant,Bill;"
  "TEL:+1-917-202-9936;"       // <-- your phone
  "EMAIL:bigrant@cisco.com;"    // <-- your email
  "URL:https://www.linkedin.com/in/billgrant;;";

// >>> YOUR MEETING-BOOKING LINK (Calendly, Cal.com, HubSpot, etc.).
const char *MEETING_URL = "https://calendly.com/your-link";

// Text shown on the QR pages and the badge.
const char *BOOTH_NAME = "";
const char *BADGE_TEXT = "Got evals?";

// >>> OPTIONAL WI-FI, ONLY FOR CLOCK SYNC. Leave WIFI_SSID empty to skip
// NTP: the clock then runs from the sketch's compile time, so flash soon
// after compiling and it will be right. If you fill these in, entering
// Clock mode syncs via NTP once per boot.
const char *WIFI_SSID = "";
const char *WIFI_PASS = "";
// Timezone for the clock (POSIX TZ string, DST rules included).
#define CLOCK_TZ "EST5EDT,M3.2.0/2,M11.1.0"  // US Eastern

// Auto-advance the flipbook every N milliseconds (0 = off).
#define AUTO_ADVANCE_MS 10000

// Long-press duration (ms) that returns to the menu from any mode.
#define LONG_PRESS_MS 800

// ---------------------------------------------------------------------------

#define SCREEN_W 466
#define SCREEN_H 466
#define BOOT_BUTTON_PIN 0  // side BOOT button, active LOW

// Photos produced by convert_photos.py (optional).
#if __has_include("booth_photos.h")
#include "booth_photos.h"
#define HAS_PHOTOS 1
#else
#define HAS_PHOTOS 0
#endif

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0 /* SDIO0 */, LCD_SDIO1 /* SDIO1 */,
  LCD_SDIO2 /* SDIO2 */, LCD_SDIO3 /* SDIO3 */);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET /* RST */, 0 /* rotation */, LCD_WIDTH /* width */, LCD_HEIGHT /* height */, 6, 0, 0, 0);

TouchDrvCST92xx touch;
int16_t touchX[5], touchY[5];
bool touchOk = false;

// --- Clicker (BLE HID keyboard, built on the ESP32 core's BLE library) ---
// HID usage IDs for the arrow keys.
#define HID_KEY_LEFT_ARROW 0x50
#define HID_KEY_RIGHT_ARROW 0x4F

// Standard boot-keyboard report descriptor: 8-byte reports
// (modifiers, reserved, 6 key slots).
static const uint8_t keyboardReportMap[] = {
  0x05, 0x01,  // Usage Page (Generic Desktop)
  0x09, 0x06,  // Usage (Keyboard)
  0xA1, 0x01,  // Collection (Application)
  0x05, 0x07,  //   Usage Page (Key Codes)
  0x19, 0xE0,  //   Usage Minimum (224)
  0x29, 0xE7,  //   Usage Maximum (231)
  0x15, 0x00,  //   Logical Minimum (0)
  0x25, 0x01,  //   Logical Maximum (1)
  0x75, 0x01,  //   Report Size (1)
  0x95, 0x08,  //   Report Count (8)
  0x81, 0x02,  //   Input (Data, Variable, Absolute) - modifier byte
  0x95, 0x01,  //   Report Count (1)
  0x75, 0x08,  //   Report Size (8)
  0x81, 0x03,  //   Input (Constant, Variable, Absolute) - reserved byte
  0x95, 0x05,  //   Report Count (5)
  0x75, 0x01,  //   Report Size (1)
  0x05, 0x08,  //   Usage Page (LEDs)
  0x19, 0x01,  //   Usage Minimum (1)
  0x29, 0x05,  //   Usage Maximum (5)
  0x91, 0x02,  //   Output (Data, Variable, Absolute) - LED report
  0x95, 0x01,  //   Report Count (1)
  0x75, 0x03,  //   Report Size (3)
  0x91, 0x03,  //   Output (Constant, Variable, Absolute) - LED padding
  0x95, 0x06,  //   Report Count (6)
  0x75, 0x08,  //   Report Size (8)
  0x15, 0x00,  //   Logical Minimum (0)
  0x25, 0x65,  //   Logical Maximum (101)
  0x05, 0x07,  //   Usage Page (Key Codes)
  0x19, 0x00,  //   Usage Minimum (0)
  0x29, 0x65,  //   Usage Maximum (101)
  0x81, 0x00,  //   Input (Data, Array, Absolute) - key slots
  0xC0         // End Collection
};

BLECharacteristic *inputKeyboard = nullptr;
bool bleStarted = false;
bool clickerConnected = false;

class ClickerServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    clickerConnected = true;
  }
  void onDisconnect(BLEServer *pServer) override {
    clickerConnected = false;
    BLEDevice::startAdvertising();  // keep showing up for re-pairing
  }
};

void clickerBegin() {
  BLEDevice::init("Waveshare Clicker");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ClickerServerCallbacks());
  BLEHIDDevice *hid = new BLEHIDDevice(server);
  inputKeyboard = hid->inputReport(1);
  hid->manufacturer()->setValue("Waveshare");
  hid->pnp(0x02, 0x1234, 0x5678, 0x0110);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t *)keyboardReportMap, sizeof(keyboardReportMap));
  hid->startServices();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);  // HID Keyboard
  adv->addServiceUUID(hid->hidService()->getUUID());
  BLEDevice::startAdvertising();
  bleStarted = true;
}

void clickerSendKey(uint8_t key) {
  if (!clickerConnected || !inputKeyboard) return;
  uint8_t press[8] = {0, 0, key, 0, 0, 0, 0, 0};
  uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  inputKeyboard->setValue(press, sizeof(press));
  inputKeyboard->notify();
  delay(10);
  inputKeyboard->setValue(release, sizeof(release));
  inputKeyboard->notify();
}

// QR working buffers (Nayuki qrcodegen needs these as static storage).
static uint8_t qrTemp[qrcodegen_BUFFER_LEN_MAX];
static uint8_t qrCode[qrcodegen_BUFFER_LEN_MAX];

// --- Modes ---
enum Mode { MODE_MENU, MODE_FLIPBOOK, MODE_QR, MODE_BADGE, MODE_CLICKER, MODE_CLOCK, MODE_SETTINGS };
Mode mode = MODE_MENU;

// --- Menu ---
enum MenuItem { MENU_FLIPBOOK, MENU_QR, MENU_BADGE, MENU_CLICKER, MENU_CLOCK, MENU_SETTINGS, MENU_COUNT };
const char *MENU_LABELS[MENU_COUNT] = { "Flipbook", "QR Codes", "Badge", "Clicker", "Clock", "Settings" };
const char *MENU_HINTS[MENU_COUNT] = { "Photos", "LinkedIn - Contact - Book", "Got evals?", "Slide remote", "Analog / digital", "Wi-Fi" };
#define MENU_BTN_X 58
#define MENU_BTN_W 350
#define MENU_BTN_H 70
#define MENU_BTN_Y0 70
#define MENU_BTN_GAP 12
#define MENU_VISIBLE 4  // buttons shown at once; swipe (or BOOT) scrolls
int menuScroll = 0;

// --- Flipbook / QR state ---
int flipPage = 0;
int qrIndex = 0;
const char *QR_PAYLOADS[3] = { LINKEDIN_URL, VCARD_TEXT, MEETING_URL };
const char *QR_LABELS[3] = { "LinkedIn", "My contact card", "Book a meeting" };
const char *QR_CAPTIONS[3] = { "Scan to connect", "Scan to save my contact", "Scan to book time with me" };
#define QR_COUNT 3

// --- Badge animation state ---
bool badgeAnim = true;
int badgeX = 100, badgeY = 180, badgeVX = 3, badgeVY = 2;

int flipPageCount() {
#if HAS_PHOTOS
  return BOOTH_PHOTO_COUNT;
#else
  return 1;  // intro slide only
#endif
}

// ---- drawing helpers -------------------------------------------------------

void drawCentered(const char *text, int16_t y, uint8_t textSize, uint16_t color) {
  gfx->setTextSize(textSize);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((SCREEN_W - (int)w) / 2 - x1, y - y1);
  gfx->print(text);
}

// Page dots near the bottom, kept inside the round screen's chord.
void drawDots(int current, int total, uint16_t color) {
  if (total < 2) return;
  const int spacing = 22, r = 5, y = 432;
  int x0 = SCREEN_W / 2 - (total - 1) * spacing / 2;
  for (int i = 0; i < total; i++) {
    if (i == current) gfx->fillCircle(x0 + i * spacing, y, r, color);
    else gfx->drawCircle(x0 + i * spacing, y, r, color);
  }
}

// ---- Menu ------------------------------------------------------------------

void drawMenu() {
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Menu", 14, 3, RGB565_WHITE);
  if (menuScroll > 0) {  // more above
    gfx->fillTriangle(SCREEN_W / 2 - 10, 58, SCREEN_W / 2 + 10, 58, SCREEN_W / 2, 46, RGB565_CYAN);
  }
  for (int row = 0; row < MENU_VISIBLE; row++) {
    int i = menuScroll + row;
    if (i >= MENU_COUNT) break;
    int y = MENU_BTN_Y0 + row * (MENU_BTN_H + MENU_BTN_GAP);
    gfx->fillRoundRect(MENU_BTN_X, y, MENU_BTN_W, MENU_BTN_H, 12, RGB565_NAVY);
    gfx->drawRoundRect(MENU_BTN_X, y, MENU_BTN_W, MENU_BTN_H, 12, RGB565_CYAN);
    drawCentered(MENU_LABELS[i], y + 10, 3, RGB565_WHITE);
    drawCentered(MENU_HINTS[i], y + 44, 1, RGB565_LIGHTGREY);
  }
  if (menuScroll + MENU_VISIBLE < MENU_COUNT) {  // more below
    gfx->fillTriangle(SCREEN_W / 2 - 10, 400, SCREEN_W / 2 + 10, 400, SCREEN_W / 2, 412, RGB565_CYAN);
  }
  drawCentered("Swipe to scroll - tap to open", 428, 1, RGB565_DARKGREY);
}

int menuHitTest(int16_t x, int16_t y) {
  for (int row = 0; row < MENU_VISIBLE; row++) {
    int i = menuScroll + row;
    if (i >= MENU_COUNT) break;
    int by = MENU_BTN_Y0 + row * (MENU_BTN_H + MENU_BTN_GAP);
    if (x >= MENU_BTN_X && x < MENU_BTN_X + MENU_BTN_W && y >= by && y < by + MENU_BTN_H) return i;
  }
  return -1;
}

void menuScrollBy(int delta) {
  int maxScroll = MENU_COUNT - MENU_VISIBLE;
  if (maxScroll < 0) maxScroll = 0;
  int next = menuScroll + delta;
  if (next < 0) next = 0;
  if (next > maxScroll) next = maxScroll;
  if (next != menuScroll) {
    menuScroll = next;
    drawMenu();
  }
}

// ---- Flipbook --------------------------------------------------------------

void drawIntroPage() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, 225, RGB565_CYAN);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, 224, RGB565_CYAN);
  drawCentered(BOOTH_NAME, 190, 4, RGB565_WHITE);
  drawCentered("Tap or swipe to flip", 250, 2, RGB565_LIGHTGREY);
  drawCentered("Long-press for menu", 285, 2, RGB565_LIGHTGREY);
}

void drawFlipPage() {
#if HAS_PHOTOS
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)BOOTH_PHOTOS[flipPage], SCREEN_W, SCREEN_H);
  drawDots(flipPage, flipPageCount(), RGB565_WHITE);
#else
  drawIntroPage();
#endif
}

void nextFlipPage() {
  flipPage = (flipPage + 1) % flipPageCount();
  drawFlipPage();
}

void prevFlipPage() {
  flipPage = (flipPage + flipPageCount() - 1) % flipPageCount();
  drawFlipPage();
}

// ---- QR codes --------------------------------------------------------------

void drawQrPage(int index) {
  gfx->fillScreen(RGB565_WHITE);
  drawCentered(QR_LABELS[index], 28, 2, RGB565_BLACK);

  bool ok = qrcodegen_encodeText(QR_PAYLOADS[index], qrTemp, qrCode,
                                 qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                 qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
  if (!ok) {
    drawCentered("QR code too long!", 220, 2, RGB565_BLACK);
    return;
  }
  int size = qrcodegen_getSize(qrCode);
  int scale = 330 / size;
  if (scale < 2) scale = 2;
  int total = size * scale;
  int x0 = (SCREEN_W - total) / 2;
  int y0 = 66;

  gfx->fillRect(x0 - 10, y0 - 10, total + 20, total + 20, RGB565_WHITE);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcodegen_getModule(qrCode, x, y)) {
        gfx->fillRect(x0 + x * scale, y0 + y * scale, scale, scale, RGB565_BLACK);
      }
    }
  }

  drawCentered(BOOTH_NAME, 410, 2, RGB565_BLACK);
  drawCentered(QR_CAPTIONS[index], 436, 1, RGB565_DARKGREY);
}

void nextQr() {
  qrIndex = (qrIndex + 1) % QR_COUNT;
  drawQrPage(qrIndex);
}

void prevQr() {
  qrIndex = (qrIndex + QR_COUNT - 1) % QR_COUNT;
  drawQrPage(qrIndex);
}

// ---- Badge mode ------------------------------------------------------------

void drawBadgePhoto() {
#if HAS_PHOTOS
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)BOOTH_PHOTOS[0], SCREEN_W, SCREEN_H);
  // Bottom bar with the badge line (sized to fit the round screen's chord).
  gfx->fillRect(63, 398, 340, 48, RGB565_BLACK);
  drawCentered(BADGE_TEXT, 408, 3, RGB565_WHITE);
#else
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, 225, RGB565_CYAN);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, 224, RGB565_CYAN);
  drawCentered(BADGE_TEXT, 200, 4, RGB565_WHITE);
  drawCentered(BOOTH_NAME, 260, 2, RGB565_LIGHTGREY);
  drawCentered("Tap to animate", 300, 1, RGB565_DARKGREY);
#endif
}

void drawBadgeAnimFrame() {
  // Bouncing "Got evals?" with a pulsing ring. Text is ~240px wide at size 4.
  const int tw = 240, th = 34;
  badgeX += badgeVX;
  badgeY += badgeVY;
  if (badgeX < 10 || badgeX > SCREEN_W - 10 - tw) badgeVX = -badgeVX;
  if (badgeY < 40 || badgeY > SCREEN_H - 40 - th) badgeVY = -badgeVY;

  gfx->fillScreen(RGB565_BLACK);
  int pulse = 90 + (int)(40 * sin(millis() / 300.0));
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, pulse, RGB565_CYAN);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, pulse + 18, RGB565_BLUE);
  gfx->setTextSize(4);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(badgeX, badgeY);
  gfx->print(BADGE_TEXT);
}

void drawBadge() {
  if (badgeAnim) drawBadgeAnimFrame();
  else drawBadgePhoto();
}

// ---- Clicker mode ----------------------------------------------------------

void drawClicker() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawLine(SCREEN_W / 2, 120, SCREEN_W / 2, 380, RGB565_DARKGREY);
  // Big "<" / ">" glyphs, one per half of the screen.
  gfx->setTextSize(6);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(90, 200);
  gfx->print("<");
  gfx->setCursor(330, 200);
  gfx->print(">");
  // Labels under each arrow.
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(88, 300);
  gfx->print("Prev");
  gfx->setCursor(330, 300);
  gfx->print("Next");
  if (bleStarted && clickerConnected) {
    drawCentered("Connected - tap to advance", 60, 1, RGB565_GREEN);
  } else {
    drawCentered("Pairing: find 'Waveshare", 52, 1, RGB565_YELLOW);
    drawCentered("Clicker' in Bluetooth", 70, 1, RGB565_YELLOW);
  }
  drawCentered("Long-press = menu", 420, 1, RGB565_DARKGREY);
}

// ---- Clock -----------------------------------------------------------------
// Time source: compile time (__DATE__/__TIME__) as the base, advanced by
// millis() since boot. If Wi-Fi is configured, entering Clock mode syncs
// once via NTP and re-bases the clock on real time.

time_t clockBaseEpoch = 0;
enum ClockView { CLOCK_ANALOG, CLOCK_DIGITAL, CLOCK_FLIP };
int clockView = CLOCK_ANALOG;
bool ntpSynced = false;
unsigned long lastClockDraw = 0;

int monthFromStr(const char *m) {
  const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 12; i++) {
    if (strncmp(m, names[i], 3) == 0) return i;
  }
  return 0;
}

void clockInitFromCompileTime() {
  struct tm t = {};
  t.tm_mon = monthFromStr(__DATE__);
  t.tm_mday = atoi(__DATE__ + 4);
  t.tm_year = atoi(__DATE__ + 7) - 1900;
  t.tm_hour = atoi(__TIME__);
  t.tm_min = atoi(__TIME__ + 3);
  t.tm_sec = atoi(__TIME__ + 6);
  t.tm_isdst = -1;
  clockBaseEpoch = mktime(&t);
}

void clockNow(struct tm *out) {
  time_t now = clockBaseEpoch + (time_t)(millis() / 1000);
  struct tm *t = localtime(&now);
  if (t) *out = *t;
}

// Defined in the Settings section below; declared here for the clock.
bool wifiSavedCreds(String &ssid, String &pass);

void clockTryNtp() {
  if (ntpSynced) return;
  if (WiFi.status() != WL_CONNECTED) {
    String ssid, pass;
    if (WIFI_SSID[0] != '\0') {
      ssid = WIFI_SSID;
      pass = WIFI_PASS;
    } else if (!wifiSavedCreds(ssid, pass)) {
      return;  // no Wi-Fi configured: keep the compile-time base
    }
    gfx->fillScreen(RGB565_BLACK);
    drawCentered("Syncing time...", 220, 2, RGB565_WHITE);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(200);
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(true);
      return;
    }
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm tmNow;
  if (getLocalTime(&tmNow, 5000)) {
    clockBaseEpoch = mktime(&tmNow) - (time_t)(millis() / 1000);
    ntpSynced = true;
  }
}

void drawHand(float angleDeg, int len, uint16_t color, int width) {
  float a = angleDeg * PI / 180.0f;
  float s = sin(a), c = cos(a);
  int cx = SCREEN_W / 2, cy = SCREEN_H / 2;
  for (int o = -width / 2; o <= width / 2; o++) {
    gfx->drawLine(cx + (int)(c * o), cy + (int)(s * o),
                  cx + (int)(s * len + c * o), cy + (int)(-c * len + s * o), color);
  }
}

void drawClockAnalog(struct tm *t) {
  gfx->fillScreen(RGB565_BLACK);
  int cx = SCREEN_W / 2, cy = 222, r = 200;
  // Bezel + minute track.
  gfx->drawCircle(cx, cy, r + 8, RGB565_DARKGREY);
  gfx->drawCircle(cx, cy, r, RGB565_WHITE);
  gfx->drawCircle(cx, cy, r - 1, RGB565_WHITE);
  for (int m = 0; m < 60; m++) {
    float a = m * 6.0f * PI / 180.0f;
    int rOuter = r - 6;
    int rInner = (m % 5 == 0) ? r - 26 : r - 14;
    gfx->drawLine(cx + (int)(sin(a) * rInner), cy - (int)(cos(a) * rInner),
                  cx + (int)(sin(a) * rOuter), cy - (int)(cos(a) * rOuter),
                  (m % 5 == 0) ? RGB565_WHITE : RGB565_DARKGREY);
  }
  // Hour numerals around the face.
  for (int h = 1; h <= 12; h++) {
    float a = h * 30.0f * PI / 180.0f;
    int nx = cx + (int)(sin(a) * 140);
    int ny = cy - (int)(cos(a) * 140);
    char num[3];
    snprintf(num, sizeof(num), "%d", h);
    drawCentered(num, ny - 12, 3, RGB565_WHITE);
  }
  // Thick hands with tails (no second hand: this clock has no seconds).
  float hourAngle = ((t->tm_hour % 12) + t->tm_min / 60.0f) * 30.0f;
  float minAngle = t->tm_min * 6.0f;
  drawHand(hourAngle + 180, 26, RGB565_WHITE, 11);
  drawHand(minAngle + 180, 32, RGB565_WHITE, 7);
  drawHand(hourAngle, 95, RGB565_WHITE, 11);
  drawHand(minAngle, 150, RGB565_WHITE, 7);
  gfx->fillCircle(cx, cy, 11, RGB565_WHITE);
  gfx->fillCircle(cx, cy, 4, RGB565_BLACK);
  drawCentered("Tap = next view", 438, 1, RGB565_DARKGREY);
}

void drawClockDigital(struct tm *t) {
  gfx->fillScreen(RGB565_BLACK);
  char hm[6], date[16], ampm[3];
  strftime(hm, sizeof(hm), "%I:%M", t);
  strftime(date, sizeof(date), "%a %b %d", t);
  strftime(ampm, sizeof(ampm), "%p", t);
  drawCentered(hm, 180, 8, RGB565_WHITE);
  drawCentered(ampm, 272, 2, RGB565_LIGHTGREY);
  drawCentered(date, 312, 2, RGB565_LIGHTGREY);
  drawCentered("Tap = next view", 400, 1, RGB565_DARKGREY);
}

// Defined in the flip-clock section below (Arduino IDE would prototype this
// automatically; the PlatformIO check build needs it declared).
void drawFlipClock(struct tm *t);

int lastClockMinute = -1;  // analog/digital redraw only when this changes

void drawClock() {
  struct tm t;
  clockNow(&t);
  if (clockView == CLOCK_ANALOG) drawClockAnalog(&t);
  else if (clockView == CLOCK_DIGITAL) drawClockDigital(&t);
  else drawFlipClock(&t);
  if (clockView != CLOCK_FLIP) lastClockMinute = t.tm_min;
}

// ---- Flip clock + clack sound ----------------------------------------------
// View 3 of the clock: split-flap style cards. When the minute changes, the
// changed cards animate (a dark flap sweeps down, then the new digit reveals
// downward) and the speaker plays a synthesized mechanical "clack" through
// the board's ES8311 codec. Sound on/off is the top-right corner in flip
// view; it defaults to on.

#define FLIP_CARD_W 96
#define FLIP_CARD_H 150
#define FLIP_GAP 12
#define FLIP_COLON_W 28
#define FLIP_Y 158

bool flipSoundOn = true;
bool audioReady = false;
int flipDigits[4] = {-1, -1, -1, -1};
I2SClass flipI2s;

bool flipAudioInit() {
  if (audioReady) return true;
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);  // speaker amplifier enable
  flipI2s.setPins(9, 45, 8, 10, 16);  // BCK, WS, DO, DI, MCK (see pin_config.h)
  if (!flipI2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("Flip audio: I2S init failed, continuing silent");
    return false;
  }
  // ES8311 codec init (same sequence as Waveshare's 07_ES8311 example).
  es8311_handle_t es_handle = es8311_create(0, ES8311_ADDRRES_0);
  if (!es_handle) return false;
  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = 16000 * 256,
    .sample_frequency = 16000
  };
  if (es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_sample_frequency_config(es_handle, es_clk.mclk_frequency, es_clk.sample_frequency) != ESP_OK) return false;
  if (es8311_voice_volume_set(es_handle, 85, NULL) != ESP_OK) return false;
  audioReady = true;
  return true;
}

void flipPlayClack() {
  if (!flipSoundOn || !flipAudioInit()) return;
  const int sampleRate = 16000;
  const int frames = 2880;  // 180 ms
  static int16_t buf[frames * 2];
  uint32_t seed = 1234567;
  for (int i = 0; i < frames; i++) {
    float t = (float)i / sampleRate;
    seed = seed * 1664525u + 1013904223u;
    float noise = ((int)((seed >> 16) & 0x7FFF) / 16384.0f) - 1.0f;  // -1..1
    float burst = noise * expf(-t / 0.008f);                          // click transient
    float thock = sinf(2 * PI * 700 * t) * expf(-t / 0.030f) * 0.6f   // flap resonance
                + sinf(2 * PI * 180 * t) * expf(-t / 0.060f) * 0.8f;  // body thump
    float s = (burst * 0.7f + thock * 0.7f) * 0.5f;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    int16_t v = (int16_t)(s * 16000);
    buf[i * 2] = v;
    buf[i * 2 + 1] = v;
  }
  flipI2s.write((uint8_t *)buf, sizeof(buf));
}

void flipLayout(int xs[5]) {
  int x = (SCREEN_W - (4 * FLIP_CARD_W + 4 * FLIP_GAP + FLIP_COLON_W)) / 2;
  xs[0] = x;
  x += FLIP_CARD_W + FLIP_GAP;
  xs[1] = x;
  x += FLIP_CARD_W + FLIP_GAP;
  xs[2] = x;  // colon
  x += FLIP_COLON_W + FLIP_GAP;
  xs[3] = x;
  x += FLIP_CARD_W + FLIP_GAP;
  xs[4] = x;
}

void drawFlipCardBase(int x, char digit) {
  gfx->fillRoundRect(x, FLIP_Y, FLIP_CARD_W, FLIP_CARD_H, 10, RGB565_DARKGREY);
  gfx->drawLine(x + 6, FLIP_Y + FLIP_CARD_H / 2, x + FLIP_CARD_W - 6, FLIP_Y + FLIP_CARD_H / 2, RGB565_BLACK);
  char s[2] = {digit, '\0'};
  gfx->setTextSize(10);
  gfx->setTextColor(RGB565_WHITE);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(x + (FLIP_CARD_W - (int)tw) / 2 - x1, FLIP_Y + (FLIP_CARD_H - (int)th) / 2 - y1);
  gfx->print(s);
}

void drawFlipStatic(const int *d, const char *ampm) {
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Flip clock", 56, 2, RGB565_LIGHTGREY);
  gfx->setTextSize(1);
  gfx->setTextColor(flipSoundOn ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setCursor(398, 44);
  gfx->print(flipSoundOn ? "Snd on" : "Snd off");
  int xs[5];
  flipLayout(xs);
  for (int i = 0; i < 4; i++) {
    int pos = (i < 2) ? i : i + 1;
    drawFlipCardBase(xs[pos], '0' + d[i]);
  }
  gfx->fillCircle(xs[2] + FLIP_COLON_W / 2, FLIP_Y + 45, 7, RGB565_WHITE);
  gfx->fillCircle(xs[2] + FLIP_COLON_W / 2, FLIP_Y + 105, 7, RGB565_WHITE);
  drawCentered(ampm, 340, 2, RGB565_LIGHTGREY);
  drawCentered("Tap = next view", 396, 1, RGB565_DARKGREY);
  drawCentered("Top-right toggles sound", 418, 1, RGB565_DARKGREY);
}

void flipDigitsFromTm(struct tm *t, int *d, char *ampm) {
  int h12 = t->tm_hour % 12;
  if (h12 == 0) h12 = 12;
  d[0] = h12 / 10;
  d[1] = h12 % 10;
  d[2] = t->tm_min / 10;
  d[3] = t->tm_min % 10;
  strcpy(ampm, t->tm_hour < 12 ? "AM" : "PM");
}

void animateFlip(const int *oldD, const int *newD) {
  bool changed[4];
  for (int i = 0; i < 4; i++) changed[i] = (oldD[i] != newD[i]);
  int xs[5];
  flipLayout(xs);
  for (int step = 0; step < 5; step++) {  // flap sweeps down over old digits
    for (int i = 0; i < 4; i++) {
      if (!changed[i]) continue;
      int pos = (i < 2) ? i : i + 1;
      drawFlipCardBase(xs[pos], '0' + oldD[i]);
      int coverH = (step + 1) * (FLIP_CARD_H / 2) / 5;
      gfx->fillRect(xs[pos], FLIP_Y, FLIP_CARD_W, coverH, RGB565_BLACK);
      gfx->drawLine(xs[pos], FLIP_Y + coverH, xs[pos] + FLIP_CARD_W - 1, FLIP_Y + coverH, RGB565_LIGHTGREY);
    }
    delay(50);
  }
  flipPlayClack();  // the flap lands
  for (int step = 0; step < 5; step++) {  // new digit reveals downward
    for (int i = 0; i < 4; i++) {
      if (!changed[i]) continue;
      int pos = (i < 2) ? i : i + 1;
      drawFlipCardBase(xs[pos], '0' + newD[i]);
      int reveal = (step + 1) * (FLIP_CARD_H / 2) / 5;
      int my = FLIP_Y + FLIP_CARD_H / 2;
      gfx->fillRect(xs[pos], my + reveal, FLIP_CARD_W, FLIP_CARD_H / 2 - reveal, RGB565_BLACK);
    }
    delay(50);
  }
  for (int i = 0; i < 4; i++) {  // clean final state
    if (!changed[i]) continue;
    int pos = (i < 2) ? i : i + 1;
    drawFlipCardBase(xs[pos], '0' + newD[i]);
  }
}

void drawFlipClock(struct tm *t) {
  int d[4];
  char ampm[3];
  flipDigitsFromTm(t, d, ampm);
  drawFlipStatic(d, ampm);
  for (int i = 0; i < 4; i++) flipDigits[i] = d[i];
}

void flipClockTick(struct tm *t) {
  int d[4];
  char ampm[3];
  flipDigitsFromTm(t, d, ampm);
  bool anyChange = false;
  for (int i = 0; i < 4; i++) {
    if (d[i] != flipDigits[i]) anyChange = true;
  }
  if (!anyChange || flipDigits[0] < 0) {
    if (flipDigits[0] < 0) drawFlipStatic(d, ampm);
    for (int i = 0; i < 4; i++) flipDigits[i] = d[i];
    return;
  }
  animateFlip(flipDigits, d);
  gfx->fillRect(0, 330, SCREEN_W, 40, RGB565_BLACK);  // AM/PM may have flipped too
  drawCentered(ampm, 340, 2, RGB565_LIGHTGREY);
  for (int i = 0; i < 4; i++) flipDigits[i] = d[i];
}

// ---- Settings: Wi-Fi scan + connect --------------------------------------
// The saved network lives in NVS ("booth" namespace) so it survives reboots
// and the clock's NTP sync can use it. Password entry is an on-screen
// keyboard — no phone app or USB cable needed at the booth.

enum SettingsPage { SETTINGS_MENU, SETTINGS_SCAN, SETTINGS_KEYBOARD, SETTINGS_STATUS, SETTINGS_SLEEP };
SettingsPage settingsPage = SETTINGS_MENU;

#define SCAN_MAX 20
#define SCAN_PER_PAGE 6
String scanSsid[SCAN_MAX];
int scanRssi[SCAN_MAX];
bool scanOpen[SCAN_MAX];
int scanCount = 0;
int scanPage = 0;
String kbSsid = "";
char kbPass[64] = "";
int kbPassLen = 0;
bool kbShift = false;
bool kbSymbols = false;

bool wifiSavedCreds(String &ssid, String &pass) {
  Preferences prefs;
  prefs.begin("booth", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

void wifiSaveCreds(const String &ssid, const String &pass) {
  Preferences prefs;
  prefs.begin("booth", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void wifiForgetCreds() {
  Preferences prefs;
  prefs.begin("booth", false);
  prefs.clear();
  prefs.end();
  WiFi.disconnect(true);
}

void drawSettingsMenu() {
  settingsPage = SETTINGS_MENU;
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Settings", 28, 3, RGB565_WHITE);
  if (WiFi.status() == WL_CONNECTED) {
    drawCentered(("Connected: " + WiFi.SSID() + "  " + WiFi.localIP().toString()).c_str(), 70, 1, RGB565_GREEN);
  } else {
    String ssid, pass;
    if (wifiSavedCreds(ssid, pass)) {
      drawCentered(("Saved: " + ssid + " (not connected)").c_str(), 70, 1, RGB565_YELLOW);
    } else {
      drawCentered("Not connected", 70, 1, RGB565_LIGHTGREY);
    }
  }
  gfx->fillRoundRect(MENU_BTN_X, 110, MENU_BTN_W, 64, 12, RGB565_NAVY);
  gfx->drawRoundRect(MENU_BTN_X, 110, MENU_BTN_W, 64, 12, RGB565_CYAN);
  drawCentered("Wi-Fi networks", 128, 2, RGB565_WHITE);
  drawCentered("Scan and connect", 156, 1, RGB565_LIGHTGREY);
  String ssid, pass;
  bool hasSaved = wifiSavedCreds(ssid, pass);
  gfx->fillRoundRect(MENU_BTN_X, 190, MENU_BTN_W, 64, 12, RGB565_NAVY);
  gfx->drawRoundRect(MENU_BTN_X, 190, MENU_BTN_W, 64, 12, hasSaved ? RGB565_CYAN : RGB565_DARKGREY);
  drawCentered("Forget saved Wi-Fi", 208, 2, hasSaved ? RGB565_WHITE : RGB565_DARKGREY);
  drawCentered(hasSaved ? ssid.c_str() : "Nothing saved", 236, 1, RGB565_LIGHTGREY);
  gfx->fillRoundRect(MENU_BTN_X, 270, MENU_BTN_W, 64, 12, RGB565_NAVY);
  gfx->drawRoundRect(MENU_BTN_X, 270, MENU_BTN_W, 64, 12, RGB565_CYAN);
  drawCentered("Sleep device", 288, 2, RGB565_WHITE);
  drawCentered("BOOT button wakes it", 316, 1, RGB565_LIGHTGREY);
  drawCentered("Long-press = menu", 420, 1, RGB565_DARKGREY);
}

void drawSleepConfirm() {
  settingsPage = SETTINGS_SLEEP;
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Sleep device?", 120, 3, RGB565_WHITE);
  drawCentered("Screen goes dark. Press the side", 180, 1, RGB565_LIGHTGREY);
  drawCentered("BOOT button to wake it up.", 200, 1, RGB565_LIGHTGREY);
  gfx->fillRoundRect(MENU_BTN_X, 250, MENU_BTN_W, 64, 12, RGB565_NAVY);
  gfx->drawRoundRect(MENU_BTN_X, 250, MENU_BTN_W, 64, 12, RGB565_CYAN);
  drawCentered("Sleep", 272, 2, RGB565_WHITE);
  gfx->fillRoundRect(MENU_BTN_X, 330, MENU_BTN_W, 64, 12, RGB565_NAVY);
  gfx->drawRoundRect(MENU_BTN_X, 330, MENU_BTN_W, 64, 12, RGB565_CYAN);
  drawCentered("Cancel", 352, 2, RGB565_WHITE);
}

void doSleep() {
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Sleeping...", 220, 2, RGB565_WHITE);
  drawCentered("Press BOOT to wake", 260, 1, RGB565_LIGHTGREY);
  delay(500);
  if (bleStarted) BLEDevice::getAdvertising()->stop();
  if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(true);
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);  // speaker amp off
  // Deep sleep: only the side BOOT button (GPIO0, active LOW) wakes it.
  // Saved Wi-Fi survives (NVS); the clock re-syncs via NTP on next entry
  // if a network is saved, else it restarts from compile time.
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

void drawWifiList() {
  settingsPage = SETTINGS_SCAN;
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Wi-Fi networks", 28, 2, RGB565_WHITE);
  if (scanCount == 0) {
    drawCentered("No networks found", 200, 2, RGB565_LIGHTGREY);
    drawCentered("Tap to rescan", 240, 1, RGB565_DARKGREY);
    return;
  }
  int start = scanPage * SCAN_PER_PAGE;
  int shown = min(SCAN_PER_PAGE, scanCount - start);
  for (int i = 0; i < shown; i++) {
    int idx = start + i;
    int y = 70 + i * 52;
    gfx->fillRoundRect(43, y, 380, 46, 8, RGB565_NAVY);
    String s = scanSsid[idx];
    if (s.length() > 22) s = s.substring(0, 21) + "~";
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(58, y + 14);
    gfx->print(s);
    int bars = scanRssi[idx] > -55 ? 4 : scanRssi[idx] > -65 ? 3 : scanRssi[idx] > -75 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
      int bh = 5 + b * 4;
      gfx->fillRect(368 + b * 9, y + 30 - bh, 6, bh, b < bars ? RGB565_GREEN : RGB565_DARKGREY);
    }
    if (!scanOpen[idx]) {  // little padlock for secured networks
      gfx->drawCircle(352, y + 16, 5, RGB565_YELLOW);
      gfx->fillRect(347, y + 16, 10, 9, RGB565_YELLOW);
    }
  }
  int pages = (scanCount + SCAN_PER_PAGE - 1) / SCAN_PER_PAGE;
  char pg[24];
  snprintf(pg, sizeof(pg), "Page %d of %d", scanPage + 1, pages);
  drawCentered(pg, 400, 1, RGB565_LIGHTGREY);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(58, 424);
  gfx->print("< Prev");
  gfx->setCursor(330, 424);
  gfx->print("Next >");
  drawCentered("Long-press = menu", 452, 1, RGB565_DARKGREY);
}

void wifiStartScan() {
  settingsPage = SETTINGS_SCAN;
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Scanning...", 220, 2, RGB565_WHITE);
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, false);
  scanCount = 0;
  for (int i = 0; i < n && scanCount < SCAN_MAX; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    int found = -1;
    for (int j = 0; j < scanCount; j++) {
      if (scanSsid[j] == s) { found = j; break; }
    }
    if (found >= 0) {
      if (rssi > scanRssi[found]) { scanRssi[found] = rssi; scanOpen[found] = open; }
    } else {
      scanSsid[scanCount] = s;
      scanRssi[scanCount] = rssi;
      scanOpen[scanCount] = open;
      scanCount++;
    }
  }
  for (int i = 0; i < scanCount; i++) {  // strongest first
    for (int j = i + 1; j < scanCount; j++) {
      if (scanRssi[j] > scanRssi[i]) {
        String ts = scanSsid[i]; scanSsid[i] = scanSsid[j]; scanSsid[j] = ts;
        int tr = scanRssi[i]; scanRssi[i] = scanRssi[j]; scanRssi[j] = tr;
        bool to = scanOpen[i]; scanOpen[i] = scanOpen[j]; scanOpen[j] = to;
      }
    }
  }
  scanPage = 0;
  drawWifiList();
}

void drawWifiStatus(const char *title, const char *sub) {
  settingsPage = SETTINGS_STATUS;
  gfx->fillScreen(RGB565_BLACK);
  drawCentered(title, 200, 2, RGB565_WHITE);
  if (sub) drawCentered(sub, 240, 1, RGB565_LIGHTGREY);
  drawCentered("Tap to continue", 300, 1, RGB565_DARKGREY);
}

void wifiConnectTo(const String &ssid, const String &pass) {
  drawWifiStatus(("Connecting to " + ssid).c_str(), "This takes a few seconds");
  WiFi.mode(WIFI_STA);
  if (pass.length() > 0) WiFi.begin(ssid.c_str(), pass.c_str());
  else WiFi.begin(ssid.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    wifiSaveCreds(ssid, pass);
    drawWifiStatus("Connected!", WiFi.localIP().toString().c_str());
  } else {
    WiFi.disconnect(true);
    drawWifiStatus("Could not connect", "Check the password and try again");
  }
}

// ---- On-screen keyboard ----------------------------------------------------
// Rows: digits/symbols, QWERTY, ASDF, ZXCV+backspace, and a bottom row with
// the symbol toggle, shift, space, Done and Cancel. Geometry lives in
// kbKeyRect so drawing and hit-testing always agree.

#define KB_X 23
#define KB_W 420
#define KB_GAP 4
#define KB_ROW_Y0 132
#define KB_ROW_H 54

// NOTE: kbKeyRect returns its rectangle through plain int pointers, not a
// struct, and kbHit/enterMode likewise take plain ints. Arduino IDE
// auto-generates function prototypes before the sketch's own type
// definitions, so a function parameter of a sketch-defined type (struct,
// enum) fails to compile there even though plain C++ accepts it.
const char *kbRowString(int row) {
  if (row == 0) return kbSymbols ? "!@#$%^&*()" : "1234567890";
  if (row == 1) return "QWERTYUIOP";
  if (row == 2) return "ASDFGHJKL";
  return "ZXCVBNM";
}

bool kbKeyRect(int row, int col, int *kx, int *ky, int *kw) {
  int y = KB_ROW_Y0 + row * KB_ROW_H;
  if (row <= 2) {
    const char *s = kbRowString(row);
    int n = strlen(s);
    if (col < 0 || col >= n) return false;
    int keyW = (KB_W - (n - 1) * KB_GAP) / n;
    int totalW = n * keyW + (n - 1) * KB_GAP;
    *kx = KB_X + (KB_W - totalW) / 2 + col * (keyW + KB_GAP);
    *ky = y;
    *kw = keyW;
    return true;
  }
  if (row == 3) {
    if (col < 0 || col > 7) return false;
    int unitW = (KB_W - 7 * KB_GAP) / 9;
    if (col < 7) {
      *kx = KB_X + col * (unitW + KB_GAP);
      *kw = unitW;
    } else {
      *kx = KB_X + 7 * (unitW + KB_GAP);
      *kw = 2 * unitW + KB_GAP;
    }
    *ky = y;
    return true;
  }
  if (row == 4) {
    static const int widths[5] = {60, 60, 136, 68, 68};
    if (col < 0 || col > 4) return false;
    int x = KB_X + 6;
    for (int i = 0; i < col; i++) x += widths[i] + KB_GAP;
    *kx = x;
    *ky = y;
    *kw = widths[col];
    return true;
  }
  return false;
}

void kbDrawKey(int x, int y, int w, const char *label, bool active) {
  int h = KB_ROW_H - 8;
  gfx->fillRoundRect(x, y, w, h, 6, active ? RGB565_CYAN : RGB565_NAVY);
  gfx->setTextSize(2);
  gfx->setTextColor(active ? RGB565_BLACK : RGB565_WHITE);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(x + (w - (int)tw) / 2 - x1, y + (h - (int)th) / 2 - y1);
  gfx->print(label);
}

void drawKeyboard() {
  settingsPage = SETTINGS_KEYBOARD;
  gfx->fillScreen(RGB565_BLACK);
  String title = "Password for " + kbSsid;
  if (title.length() > 30) title = title.substring(0, 29) + "~";
  drawCentered(title.c_str(), 28, 2, RGB565_WHITE);
  String masked = "";
  for (int i = 0; i < kbPassLen; i++) masked += "*";
  if (masked.length() > 34) masked = "..." + masked.substring(masked.length() - 31);
  gfx->drawRoundRect(43, 62, 380, 40, 8, RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(58, 74);
  gfx->print(masked.length() ? masked : "(empty)");
  int kx, ky, kw;
  char label[2] = {' ', '\0'};
  for (int row = 0; row <= 2; row++) {
    const char *s = kbRowString(row);
    for (int col = 0; col < (int)strlen(s); col++) {
      if (!kbKeyRect(row, col, &kx, &ky, &kw)) continue;
      char c = s[col];
      if (row > 0 && !kbShift) c = tolower(c);
      label[0] = c;
      kbDrawKey(kx, ky, kw, label, false);
    }
  }
  for (int col = 0; col < 7; col++) {
    if (!kbKeyRect(3, col, &kx, &ky, &kw)) continue;
    char c = kbRowString(3)[col];
    if (!kbShift) c = tolower(c);
    label[0] = c;
    kbDrawKey(kx, ky, kw, label, false);
  }
  if (kbKeyRect(3, 7, &kx, &ky, &kw)) kbDrawKey(kx, ky, kw, "Bksp", false);
  static const char *bottom[5] = {nullptr, "Shift", "Space", "Done", "Cancel"};
  for (int col = 0; col < 5; col++) {
    if (!kbKeyRect(4, col, &kx, &ky, &kw)) continue;
    const char *lbl = bottom[col];
    bool active = false;
    if (col == 0) lbl = kbSymbols ? "123" : "#+=";
    if (col == 1) active = kbShift;
    if (col == 0) active = kbSymbols;
    kbDrawKey(kx, ky, kw, lbl, active);
  }
}

// Action codes are plain ints (see the note above kbKeyRect).
enum KbAction { KB_NONE, KB_CHAR, KB_BKSP, KB_SHIFT, KB_SYMBOLS, KB_SPACE, KB_DONE, KB_CANCEL };

bool kbHit(int x, int y, int *action, char *ch) {
  *action = KB_NONE;
  for (int row = 0; row <= 4; row++) {
    int ry = KB_ROW_Y0 + row * KB_ROW_H;
    if (y < ry || y >= ry + KB_ROW_H - 8) continue;
    for (int col = 0; col < 10; col++) {
      int kx, ky, kw;
      if (!kbKeyRect(row, col, &kx, &ky, &kw)) continue;
      if (x < kx || x >= kx + kw) continue;
      if (row <= 2) {
        *ch = kbRowString(row)[col];
        if (row > 0 && !kbShift) *ch = tolower(*ch);
        *action = KB_CHAR;
        return true;
      }
      if (row == 3) {
        if (col < 7) {
          *ch = kbRowString(3)[col];
          if (!kbShift) *ch = tolower(*ch);
          *action = KB_CHAR;
        } else {
          *action = KB_BKSP;
        }
        return true;
      }
      if (row == 4) {
        *action = (col == 0) ? KB_SYMBOLS : (col == 1) ? KB_SHIFT : (col == 2) ? KB_SPACE : (col == 3) ? KB_DONE : KB_CANCEL;
        return true;
      }
    }
  }
  return false;
}

// ---- Mode switching --------------------------------------------------------

void enterMode(int m) {  // int, not Mode: see the note above kbKeyRect
  mode = (Mode)m;
  switch (m) {
    case MODE_MENU:
      drawMenu();
      break;
    case MODE_FLIPBOOK:
      flipPage = 0;
      drawFlipPage();
      break;
    case MODE_QR:
      qrIndex = 0;
      drawQrPage(qrIndex);
      break;
    case MODE_BADGE:
      drawBadge();
      break;
    case MODE_CLICKER:
      if (!bleStarted) clickerBegin();
      else BLEDevice::startAdvertising();  // re-advertise on re-entry
      drawClicker();
      break;
    case MODE_CLOCK:
      clockTryNtp();
      drawClock();
      lastClockDraw = millis();
      break;
    case MODE_SETTINGS:
      drawSettingsMenu();
      break;
  }
  Serial.print("Mode: ");
  Serial.println((int)m);
}

void exitToMenu() {
  // Stop advertising when leaving clicker mode, but keep the BLE stack up
  // (deinit/re-init across mode switches is flaky; advertising resumes on
  // re-entry). A connected laptop stays connected but gets no keys outside
  // clicker mode, because keys are only sent from that mode.
  if (bleStarted) BLEDevice::getAdvertising()->stop();
  enterMode(MODE_MENU);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // Clock timezone + fallback time base (compile time, advanced by millis).
  setenv("TZ", CLOCK_TZ, 1);
  tzset();
  clockInitFromCompileTime();

  Wire.begin(IIC_SDA, IIC_SCL);

  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(30);
  digitalWrite(TP_RST, HIGH);
  delay(50);

  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(SCREEN_W, SCREEN_H);
    touch.setMirrorXY(true, true);
    Serial.println("Touch online");
  } else {
    Serial.println("Touch NOT online - button + auto-advance still work");
  }

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(255);  // booth lighting: full brightness

  enterMode(MODE_MENU);
}

void loop() {
  static bool tracking = false;
  static int16_t startX = 0, startY = 0, lastX = 0, lastY = 0;
  static unsigned long downAt = 0, lastInteraction = 0, lastFrame = 0;
  static bool buttonWasPressed = false;

  unsigned long now = millis();

  // --- BOOT button: next / toggle, depending on mode ---
  bool buttonPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (buttonPressed && !buttonWasPressed) {
    delay(30);  // debounce
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      switch (mode) {
        case MODE_MENU: {  // BOOT scrolls the menu down, wrapping to the top
          int maxScroll = MENU_COUNT - MENU_VISIBLE;
          if (maxScroll < 0) maxScroll = 0;
          menuScroll = (menuScroll >= maxScroll) ? 0 : menuScroll + 1;
          drawMenu();
          break;
        }
        case MODE_FLIPBOOK: nextFlipPage(); break;
        case MODE_QR: nextQr(); break;
        case MODE_BADGE:
          badgeAnim = !badgeAnim;
          drawBadge();
          break;
        case MODE_CLICKER:
          clickerSendKey(HID_KEY_RIGHT_ARROW);
          drawClicker();
          break;
        case MODE_CLOCK:
          clockView = (clockView + 1) % 3;  // analog -> digital -> flip
          drawClock();
          break;
        case MODE_SETTINGS:
          // BOOT = back to the settings menu from scan/keyboard/status.
          if (settingsPage != SETTINGS_MENU) drawSettingsMenu();
          break;
        default: break;
      }
      lastInteraction = now;
    }
  }
  buttonWasPressed = buttonPressed;

  // --- Touch ---
  if (touchOk) {
    uint8_t touched = touch.getPoint(touchX, touchY, touch.getSupportTouchPoint());
    if (touched > 0) {
      if (!tracking) {
        tracking = true;
        startX = touchX[0];
        startY = touchY[0];
        downAt = now;
      }
      lastX = touchX[0];
      lastY = touchY[0];
    } else if (tracking) {
      tracking = false;
      unsigned long held = now - downAt;
      int16_t dx = lastX - startX;
      int16_t dy = lastY - startY;
      if (held >= LONG_PRESS_MS) {
        // Long-press: back to menu from any mode.
        if (mode != MODE_MENU) exitToMenu();
      } else if (mode == MODE_MENU) {
        if (dy <= -50) menuScrollBy(1);       // swipe up = scroll down
        else if (dy >= 50) menuScrollBy(-1);  // swipe down = scroll up
        else {
          int hit = menuHitTest(startX, startY);
          if (hit == MENU_FLIPBOOK) enterMode(MODE_FLIPBOOK);
          else if (hit == MENU_QR) enterMode(MODE_QR);
          else if (hit == MENU_BADGE) enterMode(MODE_BADGE);
          else if (hit == MENU_CLICKER) enterMode(MODE_CLICKER);
          else if (hit == MENU_CLOCK) enterMode(MODE_CLOCK);
          else if (hit == MENU_SETTINGS) enterMode(MODE_SETTINGS);
        }
      } else if (mode == MODE_FLIPBOOK) {
        if (dx <= -50) nextFlipPage();
        else if (dx >= 50) prevFlipPage();
        else if (startX < SCREEN_W / 3) prevFlipPage();
        else nextFlipPage();
      } else if (mode == MODE_QR) {
        if (dx >= 50) prevQr();
        else nextQr();
      } else if (mode == MODE_BADGE) {
        badgeAnim = !badgeAnim;
        drawBadge();
      } else if (mode == MODE_CLICKER) {
        if (startX < SCREEN_W / 3) clickerSendKey(HID_KEY_LEFT_ARROW);
        else clickerSendKey(HID_KEY_RIGHT_ARROW);
        drawClicker();  // refresh connection status
      } else if (mode == MODE_CLOCK) {
        if (clockView == CLOCK_FLIP && startX >= 360 && startY < 100) {
          flipSoundOn = !flipSoundOn;  // top-right corner toggles sound
          struct tm t;
          clockNow(&t);
          drawFlipClock(&t);
        } else {
          clockView = (clockView + 1) % 3;  // analog -> digital -> flip
          drawClock();
        }
        lastClockDraw = now;
      } else if (mode == MODE_SETTINGS) {
        if (settingsPage == SETTINGS_MENU) {
          if (startY >= 110 && startY < 174 && startX >= MENU_BTN_X && startX < MENU_BTN_X + MENU_BTN_W) {
            wifiStartScan();
          } else if (startY >= 190 && startY < 254 && startX >= MENU_BTN_X && startX < MENU_BTN_X + MENU_BTN_W) {
            String s, p;
            if (wifiSavedCreds(s, p)) {
              wifiForgetCreds();
              drawSettingsMenu();
            }
          } else if (startY >= 270 && startY < 334 && startX >= MENU_BTN_X && startX < MENU_BTN_X + MENU_BTN_W) {
            drawSleepConfirm();
          }
        } else if (settingsPage == SETTINGS_SCAN) {
          if (scanCount == 0) {
            wifiStartScan();
          } else if (startY >= 415 && startY < 450) {
            int pages = (scanCount + SCAN_PER_PAGE - 1) / SCAN_PER_PAGE;
            if (startX < SCREEN_W / 2 && scanPage > 0) {
              scanPage--;
              drawWifiList();
            } else if (startX >= SCREEN_W / 2 && scanPage < pages - 1) {
              scanPage++;
              drawWifiList();
            }
          } else if (startY >= 70 && startY < 70 + SCAN_PER_PAGE * 52 && startX >= 43 && startX < 423) {
            int row = (startY - 70) / 52;
            int idx = scanPage * SCAN_PER_PAGE + row;
            if (row < SCAN_PER_PAGE && idx < scanCount) {
              kbSsid = scanSsid[idx];
              if (scanOpen[idx]) {
                wifiConnectTo(kbSsid, "");
              } else {
                kbPassLen = 0;
                kbPass[0] = '\0';
                kbShift = false;
                kbSymbols = false;
                drawKeyboard();
              }
            }
          }
        } else if (settingsPage == SETTINGS_KEYBOARD) {
          int act;
          char ch = '\0';
          if (kbHit(startX, startY, &act, &ch)) {
            if (act == KB_CHAR && kbPassLen < 63) {
              kbPass[kbPassLen++] = ch;
              kbPass[kbPassLen] = '\0';
              drawKeyboard();
            } else if (act == KB_SPACE && kbPassLen < 63) {
              kbPass[kbPassLen++] = ' ';
              kbPass[kbPassLen] = '\0';
              drawKeyboard();
            } else if (act == KB_BKSP && kbPassLen > 0) {
              kbPass[--kbPassLen] = '\0';
              drawKeyboard();
            } else if (act == KB_SHIFT) {
              kbShift = !kbShift;
              drawKeyboard();
            } else if (act == KB_SYMBOLS) {
              kbSymbols = !kbSymbols;
              drawKeyboard();
            } else if (act == KB_DONE) {
              wifiConnectTo(kbSsid, String(kbPass));
            } else if (act == KB_CANCEL) {
              drawWifiList();
            }
          }
        } else if (settingsPage == SETTINGS_STATUS) {
          drawSettingsMenu();
        } else if (settingsPage == SETTINGS_SLEEP) {
          if (startY >= 250 && startY < 314 && startX >= MENU_BTN_X && startX < MENU_BTN_X + MENU_BTN_W) {
            doSleep();
          } else if (startY >= 330 && startY < 394 && startX >= MENU_BTN_X && startX < MENU_BTN_X + MENU_BTN_W) {
            drawSettingsMenu();
          }
        }
      }
      lastInteraction = now;
    }
  }

  // --- Flipbook auto-advance (kiosk mode) ---
  if (mode == MODE_FLIPBOOK && AUTO_ADVANCE_MS > 0 && !tracking &&
      (now - lastInteraction > AUTO_ADVANCE_MS)) {
    nextFlipPage();
    lastInteraction = now;
  }

  // --- Badge animation tick ---
  if (mode == MODE_BADGE && badgeAnim && (now - lastFrame >= 33)) {
    drawBadgeAnimFrame();
    lastFrame = now;
  }

  // --- Clock tick: checked once a second, but analog/digital only redraw
  // when the minute changes (no seconds on display = no flicker).
  if (mode == MODE_CLOCK && (now - lastClockDraw >= 1000)) {
    struct tm t;
    clockNow(&t);
    if (clockView == CLOCK_FLIP) {
      flipClockTick(&t);  // animates + clacks only when the minute changes
    } else if (t.tm_min != lastClockMinute) {
      drawClock();
    }
    lastClockDraw = now;
  }

  delay(20);
}
