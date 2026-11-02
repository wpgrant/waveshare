#include <Arduino.h>
#include <stdio.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "qrcodegen.h"
#include <esp_sleep.h>

const char *LINKEDIN_URL = "https://www.linkedin.com/in/billgrant";

#define SCREEN_W 466
#define SCREEN_H 466
#define BOOT_BUTTON_PIN 0
#define HOLD_TO_SLEEP_MS 1200
#define BATTERY_REFRESH_MS 10000
#define AXP2101_ADDRESS 0x34
#define AXP2101_CHIP_ID_REG 0x03
#define AXP2101_STATUS_REG 0x00
#define AXP2101_ADC_CHANNEL_CTRL_REG 0x30
#define AXP2101_BAT_DET_CTRL_REG 0x68
#define AXP2101_BAT_PERCENT_REG 0xA4
#define AXP2101_CHIP_ID 0x4A

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

bool batteryMonitorOk = false;

static uint8_t qrTemp[qrcodegen_BUFFER_LEN_MAX];
static uint8_t qrCode[qrcodegen_BUFFER_LEN_MAX];

//enum BadgeMode { MODE_PICTURE, MODE_TEXT, MODE_LINKEDIN, MODE_BATTERY };
enum BadgeMode { MODE_TEXT, MODE_LINKEDIN, MODE_BATTERY };
BadgeMode mode = MODE_TEXT;

bool readPmuRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(AXP2101_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AXP2101_ADDRESS, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

bool writePmuRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(AXP2101_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool enablePmuRegisterBit(uint8_t reg, uint8_t bit) {
  uint8_t value;
  if (!readPmuRegister(reg, value)) return false;
  return writePmuRegister(reg, value | (1U << bit));
}

bool readBatteryPercent(int &percent) {
  uint8_t status, value;
  if (!batteryMonitorOk || !readPmuRegister(AXP2101_STATUS_REG, status) ||
      !(status & (1U << 3)) ||
      !readPmuRegister(AXP2101_BAT_PERCENT_REG, value) || value > 100) {
    return false;
  }
  percent = value;
  return true;
}

void drawCentered(const char *text, int16_t y, uint8_t textSize, uint16_t color) {
  gfx->setTextSize(textSize);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((SCREEN_W - (int)w) / 2 - x1, y - y1);
  gfx->print(text);
}

void drawPicture() {
  gfx->fillScreen(0x169F);
  gfx->fillCircle(340, 125, 48, 0xFEA0);
  gfx->fillCircle(110, 142, 20, RGB565_WHITE);
  gfx->fillCircle(132, 135, 27, RGB565_WHITE);
  gfx->fillCircle(157, 145, 18, RGB565_WHITE);
  gfx->fillRect(108, 143, 52, 20, RGB565_WHITE);
  gfx->fillTriangle(0, 305, 155, 140, 320, 305, 0x3D5F);
  gfx->fillTriangle(120, 305, 300, 165, 466, 305, 0x2C76);
  gfx->fillTriangle(0, 295, 175, 205, 300, 295, 0x5DAB);
  gfx->fillTriangle(220, 295, 365, 190, 466, 280, 0x4D89);
  gfx->fillRect(0, 290, SCREEN_W, 176, 0x1D77);
  gfx->fillTriangle(0, 385, 145, 300, 275, 385, 0x2EA7);
  gfx->fillTriangle(205, 390, 355, 295, 466, 380, 0x2EA7);
  gfx->drawLine(0, 410, 200, 410, 0x5F1B);
  gfx->drawLine(260, 428, 466, 428, 0x5F1B);
  gfx->fillRoundRect(128, 420, 210, 30, 8, RGB565_BLACK);
  drawCentered("SAMPLE PICTURE", 428, 1, RGB565_WHITE);
}

void drawText() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, 205, RGB565_CYAN);
  gfx->drawCircle(SCREEN_W / 2, SCREEN_H / 2, 203, RGB565_BLUE);
  drawCentered("Got", 178, 6, RGB565_WHITE);
  drawCentered("evals?", 260, 5, RGB565_CYAN);
}

void drawLinkedInQr() {
  gfx->fillScreen(RGB565_WHITE);
  drawCentered("LinkedIn", 24, 2, RGB565_BLACK);

  bool ok = qrcodegen_encodeText(
    LINKEDIN_URL, qrTemp, qrCode,
    qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
    qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
  if (!ok) {
    Serial.println("Could not encode LinkedIn URL as a QR code");
    drawCentered("QR code could not be generated", 220, 1, RGB565_BLACK);
    return;
  }

  int size = qrcodegen_getSize(qrCode);
  int scale = min(10, 340 / size);
  int total = size * scale;
  int x0 = (SCREEN_W - total) / 2;
  int y0 = (SCREEN_H - total) / 2 - 4;
  gfx->fillRect(x0 - 8, y0 - 8, total + 16, total + 16, RGB565_WHITE);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcodegen_getModule(qrCode, x, y)) {
        gfx->fillRect(x0 + x * scale, y0 + y * scale, scale, scale, RGB565_BLACK);
      }
    }
  }
  drawCentered("Scan to connect", 430, 1, RGB565_DARKGREY);
}

void drawBattery() {
  gfx->fillScreen(RGB565_BLACK);
  drawCentered("Battery", 56, 3, RGB565_WHITE);

  int percent;
  if (!readBatteryPercent(percent)) {
    drawCentered("Battery level unavailable", 220, 2, RGB565_YELLOW);
    return;
  }

  gfx->drawRoundRect(113, 166, 230, 104, 16, RGB565_WHITE);
  gfx->fillRoundRect(343, 198, 18, 40, 6, RGB565_WHITE);
  if (percent > 0) {
    uint16_t color = percent <= 20 ? RGB565_RED : RGB565_GREEN;
    int fillWidth = (percent * 210) / 100;
    gfx->fillRoundRect(123, 176, fillWidth, 84, 10, color);
  }

  char label[8];
  snprintf(label, sizeof(label), "%d%%", percent);
  drawCentered(label, 320, 5, RGB565_WHITE);
}

void drawMode() {
  switch (mode) {
    //case MODE_PICTURE: drawPicture(); break;
    case MODE_TEXT: drawText(); break;
    case MODE_LINKEDIN: drawLinkedInQr(); break;
    case MODE_BATTERY: drawBattery(); break;
  }
}

void nextMode() {
  //mode = static_cast<BadgeMode>((mode + 1) % 4);
  mode = static_cast<BadgeMode>((mode + 1) % 3);
  drawMode();
}

void sleepOnBootButton() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->displayOff();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);

  Wire.begin(IIC_SDA, IIC_SCL);
  uint8_t chipId;
  batteryMonitorOk = readPmuRegister(AXP2101_CHIP_ID_REG, chipId) &&
      chipId == AXP2101_CHIP_ID &&
      enablePmuRegisterBit(AXP2101_ADC_CHANNEL_CTRL_REG, 0) &&
      enablePmuRegisterBit(AXP2101_BAT_DET_CTRL_REG, 0);
  if (!batteryMonitorOk) Serial.println("Battery monitor initialization failed");

  if (!gfx->begin()) {
    Serial.println("Display initialization failed");
    while (true) delay(1000);
  }
  gfx->displayOn();
  gfx->setBrightness(255);
  drawMode();
}

void loop() {
  static bool buttonWasPressed = false;
  static bool sleepPromptShown = false;
  static unsigned long pressedAt = 0;
  static unsigned long lastBatteryRefresh = 0;
  bool buttonPressed = digitalRead(BOOT_BUTTON_PIN) == LOW;

  if (buttonPressed && !buttonWasPressed) {
    delay(30);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      pressedAt = millis();
      sleepPromptShown = false;
      buttonWasPressed = true;
    }
  } else if (!buttonPressed && buttonWasPressed) {
    buttonWasPressed = false;
    if (millis() - pressedAt >= HOLD_TO_SLEEP_MS) {
      sleepOnBootButton();
    } else {
      nextMode();
    }
  }

  if (buttonPressed && buttonWasPressed &&
      !sleepPromptShown && millis() - pressedAt >= HOLD_TO_SLEEP_MS) {
    gfx->fillScreen(RGB565_BLACK);
    drawCentered("Release BOOT to sleep", 226, 2, RGB565_WHITE);
    sleepPromptShown = true;
  }

  if (mode == MODE_BATTERY && millis() - lastBatteryRefresh >= BATTERY_REFRESH_MS) {
    drawBattery();
    lastBatteryRefresh = millis();
  }

  delay(10);
}
