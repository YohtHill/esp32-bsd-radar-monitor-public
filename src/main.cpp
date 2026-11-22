/*
  MS60-1211S80M-BSD + ESP32 UART reader

  Wiring:
    Radar VCC  -> ESP32 5V/VIN or 3V3, depending on your module board
    Radar GND  -> ESP32 GND
    Radar TX   -> ESP32 GPIO16 / D16
    Radar RX   -> ESP32 GPIO17 / D17
    Collision sensor DO -> ESP32 GPIO27 / D27
    Collision sensor VCC -> ESP32 3V3, GND -> ESP32 GND
    Buzzer module I/O -> ESP32 GPIO25 / D25
    Buzzer module VCC -> ESP32 3V3, GND -> ESP32 GND

  AT6010 HCI protocol:
    UART: 921600 8N1 by default
    BSD active report: 5A LEN 07 <obj_num u16> <reserved u16>
                       <range s8> <angle s8> <speed s8> <obj_id s8>... CHECK
*/

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <BluetoothSerial.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Wire.h>

static constexpr int RADAR_RX_PIN = 16;  // ESP32 UART RX, connect to radar TX
static constexpr int RADAR_TX_PIN = 17;  // ESP32 UART TX, connect to radar RX
static constexpr int LCD_SCL_PIN = 22;   // OLED SCL
static constexpr int LCD_SDA_PIN = 21;   // OLED SDA
static constexpr int COLLISION_PIN = 27; // Collision/vibration sensor digital output
static constexpr int BUZZER_PIN = 25;    // Active-low buzzer module I/O
static constexpr int TFT_SCLK_PIN = 18;  // New SPI TFT SCL/SCK
static constexpr int TFT_MOSI_PIN = 23;  // New SPI TFT SDA/MOSI
static constexpr int TFT_CS_PIN = 13;    // New SPI TFT CS
static constexpr int TFT_DC_PIN = 14;    // New SPI TFT DC
static constexpr int TFT_RST_PIN = 26;   // New SPI TFT RST
static constexpr int TFT_BL_PIN = 32;    // New SPI TFT BL, or connect BL directly to 3V3
static constexpr int TFT_WIDTH = 240;    // ST7789 module width
static constexpr int TFT_HEIGHT = 240;   // ST7789 module height

static constexpr uint32_t RADAR_BAUD = 921600;
static constexpr uint32_t LCD_I2C_CLOCK = 400000;
static constexpr const char *BT_DEVICE_NAME = "ESP32_BSD_MONITOR";
static constexpr bool COLLISION_ACTIVE_LOW = true;
static constexpr bool BUZZER_ACTIVE_LOW = true;
static constexpr bool RADAR_NEGATIVE_SPEED_IS_APPROACHING = true;

static constexpr size_t RX_BUFFER_SIZE = 256;
static constexpr size_t USB_LINE_SIZE = 128;
static constexpr uint8_t MAX_TARGETS = 8;
static constexpr uint32_t LCD_REFRESH_MS = 250;
static constexpr uint32_t SECOND_TFT_REFRESH_MS = 1000;
static constexpr uint32_t TARGET_PAGE_MS = 1200;
static constexpr uint32_t BSD_FRAME_STALE_MS = 2000;
static constexpr uint8_t OLED_TARGETS_PER_PAGE = 4;
static constexpr uint32_t AUTO_COMMAND_INTERVAL_MS = 1500;
static constexpr uint32_t NOISE_LOG_INTERVAL_MS = 500;
static constexpr uint32_t LINK_STATUS_INTERVAL_MS = 5000;
static constexpr uint32_t COLLISION_DEBOUNCE_MS = 25;
static constexpr uint32_t COLLISION_LATCH_MS = 3000;
static constexpr uint32_t APP_TX_INTERVAL_MS = 250;
static constexpr uint32_t FALSE_ALARM_RETURN_MS = 2000;
static constexpr uint32_t BUZZER_WARNING_INTERVAL_MS = 500;
static constexpr uint32_t BUZZER_COLLISION_INTERVAL_MS = 100;
static constexpr float RADAR_NEAR_DISTANCE_M = 8.0f;
static constexpr float RADAR_CLOSE_UNRELIABLE_DISTANCE_M = 4.0f;
static constexpr float RADAR_APPROACH_SPEED_MPS = 1.0f;
static constexpr float RADAR_FAST_APPROACH_SPEED_MPS = 2.0f;

static uint8_t rxBuffer[RX_BUFFER_SIZE];
static char usbLine[USB_LINE_SIZE];
static char btLine[USB_LINE_SIZE];
static size_t rxLen = 0;
static size_t usbLineLen = 0;
static size_t btLineLen = 0;
static uint32_t currentRadarBaud = RADAR_BAUD;
static uint32_t lastLcdMs = 0;
static uint32_t lastSecondTftMs = 0;
static uint32_t lastAutoProbeMs = 0;
static uint32_t lastNoiseLogMs = 0;
static uint32_t lastLinkStatusMs = 0;
static uint32_t lastAppTxMs = 0;
static uint32_t stateChangedMs = 0;
static uint32_t latestFrameMs = 0;
static uint32_t rxByteCount = 0;
static uint32_t noiseByteCount = 0;
static uint32_t validFrameCount = 0;
static size_t lastRawLen = 0;
static uint8_t lastAutoProbeCommand = 0;
static uint8_t lastFrameHeader = 0;
static uint8_t lastHciCommand = 0;
static uint8_t lastHciStatus = 0;
static bool hasLatestFrame = false;
static bool hasLastHciStatus = false;
static bool radarLocked = false;
static bool sentOpenAfterLock = false;
static bool autoProbeEnabled = true;
static bool bluetoothStarted = false;
static uint8_t autoProbeCommandIndex = 0;
static bool collisionRawActive = false;
static bool collisionStableActive = false;
static bool buzzerOutputOn = false;
static bool buzzerMuted = false;
static bool secondTftStarted = false;
static uint32_t collisionRawChangedMs = 0;
static uint32_t collisionLastHitMs = 0;
static uint32_t collisionCount = 0;
static uint32_t lastBuzzerToggleMs = 0;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,
    /* reset= */ U8X8_PIN_NONE,
    /* clock= */ LCD_SCL_PIN,
    /* data=  */ LCD_SDA_PIN
);

Adafruit_ST7789 secondTft(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

struct RadarTarget {
  uint8_t targetId;
  float distanceM;
  float speedMps;
  float angleDeg;
};

struct RadarFrame {
  uint8_t targetCount;
  RadarTarget targets[MAX_TARGETS];
};

enum FusionRisk : uint8_t {
  RISK_OK,
  RISK_TRACK,
  RISK_NEAR,
  RISK_WARN,
  RISK_HIT
};

static constexpr uint8_t ZONE_LEFT = 0;
static constexpr uint8_t ZONE_CENTER = 1;
static constexpr uint8_t ZONE_RIGHT = 2;
static constexpr uint8_t ZONE_COUNT = 3;
static constexpr uint8_t ZONE_NONE = 0xFF;

struct RadarZoneSummary {
  uint8_t targetCount;
  uint8_t level;
  FusionRisk risk;
  const RadarTarget *bestTarget;
};

enum SystemState : uint8_t {
  STATE_SAFE,
  STATE_WARNING,
  STATE_COLLISION_CONFIRM,
  STATE_ALARMING,
  STATE_FALSE_ALARM
};

static RadarFrame latestFrame{};
static SystemState systemState = STATE_SAFE;

BluetoothSerial SerialBT;

static void drawTextLine(int x, int y, const char *text) {
  u8g2.drawStr(x, y, text);
}

static uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

static bool hasFreshBsdFrame() {
  return hasLatestFrame && latestFrameMs != 0 &&
         millis() - latestFrameMs <= BSD_FRAME_STALE_MS;
}

static bool readCollisionActive() {
  const int level = digitalRead(COLLISION_PIN);
  return COLLISION_ACTIVE_LOW ? level == LOW : level == HIGH;
}

static bool collisionRecentlyHit() {
  return collisionLastHitMs != 0 &&
         millis() - collisionLastHitMs <= COLLISION_LATCH_MS;
}

static const char *collisionText() {
  if (collisionRecentlyHit()) {
    return "HIT";
  }
  return collisionStableActive ? "ON" : "OK";
}

static bool radarTargetApproaching(const RadarTarget &target, float speedMps) {
  if (RADAR_NEGATIVE_SPEED_IS_APPROACHING) {
    return target.speedMps <= -speedMps;
  }
  return target.speedMps >= speedMps;
}

static FusionRisk fusionRiskForTarget(const RadarTarget *target) {
  if (collisionRecentlyHit() || collisionStableActive) {
    return RISK_HIT;
  }
  if (target == nullptr) {
    return RISK_OK;
  }

  const bool approaching = radarTargetApproaching(*target, RADAR_APPROACH_SPEED_MPS);
  const bool fastApproaching = radarTargetApproaching(*target, RADAR_FAST_APPROACH_SPEED_MPS);

  if (target->distanceM <= RADAR_CLOSE_UNRELIABLE_DISTANCE_M) {
    return RISK_NEAR;
  }
  if ((target->distanceM <= RADAR_NEAR_DISTANCE_M && approaching) ||
      fastApproaching) {
    return RISK_WARN;
  }
  if (target->distanceM <= RADAR_NEAR_DISTANCE_M || approaching) {
    return RISK_NEAR;
  }
  return RISK_TRACK;
}

static FusionRisk higherRisk(FusionRisk a, FusionRisk b) {
  return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

static FusionRisk fusionRiskForFrame(const RadarFrame &frame) {
  FusionRisk risk = fusionRiskForTarget(nullptr);
  for (uint8_t i = 0; i < frame.targetCount; ++i) {
    risk = higherRisk(risk, fusionRiskForTarget(&frame.targets[i]));
  }
  return risk;
}

static const RadarTarget *bestTargetForFrame(const RadarFrame &frame) {
  if (frame.targetCount == 0) {
    return nullptr;
  }

  const RadarTarget *best = &frame.targets[0];
  FusionRisk bestRisk = fusionRiskForTarget(best);
  for (uint8_t i = 1; i < frame.targetCount; ++i) {
    const RadarTarget *candidate = &frame.targets[i];
    const FusionRisk candidateRisk = fusionRiskForTarget(candidate);
    if (static_cast<uint8_t>(candidateRisk) > static_cast<uint8_t>(bestRisk) ||
        (candidateRisk == bestRisk && candidate->distanceM < best->distanceM)) {
      best = candidate;
      bestRisk = candidateRisk;
    }
  }
  return best;
}

static const char *riskText(FusionRisk risk) {
  switch (risk) {
    case RISK_HIT:
      return "HIT";
    case RISK_WARN:
      return "WARN";
    case RISK_NEAR:
      return "NEAR";
    case RISK_TRACK:
      return "TRACK";
    case RISK_OK:
    default:
      return "OK";
  }
}

static const char *systemStateText(SystemState state) {
  switch (state) {
    case STATE_WARNING:
      return "WARNING";
    case STATE_COLLISION_CONFIRM:
      return "COLLISION_CONFIRM";
    case STATE_ALARMING:
      return "ALARMING";
    case STATE_FALSE_ALARM:
      return "FALSE_ALARM";
    case STATE_SAFE:
    default:
      return "SAFE";
  }
}

static const char *zoneShortText(uint8_t zone) {
  switch (zone) {
    case ZONE_LEFT:
      return "L";
    case ZONE_CENTER:
      return "C";
    case ZONE_RIGHT:
      return "R";
    case ZONE_NONE:
    default:
      return "N";
  }
}

static const char *zoneLabelText(uint8_t zone) {
  switch (zone) {
    case ZONE_LEFT:
      return "LEFT";
    case ZONE_CENTER:
      return "CENTER";
    case ZONE_RIGHT:
      return "RIGHT";
    case ZONE_NONE:
    default:
      return "NONE";
  }
}

static uint8_t targetZoneIndex(const RadarTarget *target) {
  if (target == nullptr) {
    return ZONE_NONE;
  }
  if (target->angleDeg >= -60.0f && target->angleDeg < -15.0f) {
    return ZONE_LEFT;
  }
  if (target->angleDeg >= -15.0f && target->angleDeg <= 15.0f) {
    return ZONE_CENTER;
  }
  if (target->angleDeg > 15.0f && target->angleDeg <= 60.0f) {
    return ZONE_RIGHT;
  }
  return ZONE_NONE;
}

static const char *zoneText(const RadarTarget *target) {
  return zoneShortText(targetZoneIndex(target));
}

static uint8_t dangerLevel(FusionRisk risk) {
  switch (risk) {
    case RISK_HIT:
      return 5;
    case RISK_WARN:
      return 4;
    case RISK_NEAR:
      return 2;
    case RISK_TRACK:
      return 1;
    case RISK_OK:
    default:
      return 0;
  }
}

static void setSystemState(SystemState state) {
  if (systemState == state) {
    return;
  }
  systemState = state;
  buzzerMuted = false;
  stateChangedMs = millis();
  Serial.print("STATE ");
  Serial.println(systemStateText(systemState));
}

static void updateSystemStateFromSensors() {
  if (systemState == STATE_ALARMING || systemState == STATE_COLLISION_CONFIRM) {
    return;
  }
  if (systemState == STATE_FALSE_ALARM) {
    if (millis() - stateChangedMs >= FALSE_ALARM_RETURN_MS) {
      setSystemState(hasFreshBsdFrame() && latestFrame.targetCount > 0 ? STATE_WARNING
                                                                       : STATE_SAFE);
    }
    return;
  }
  if (collisionRecentlyHit() || collisionStableActive) {
    setSystemState(STATE_COLLISION_CONFIRM);
    return;
  }
  setSystemState(hasFreshBsdFrame() && latestFrame.targetCount > 0 ? STATE_WARNING
                                                                   : STATE_SAFE);
}

static FusionRisk currentFusionRisk() {
  return hasFreshBsdFrame() ? fusionRiskForFrame(latestFrame)
                            : fusionRiskForTarget(nullptr);
}

static void setBuzzer(bool on) {
  buzzerOutputOn = on;
  const int activeLevel = BUZZER_ACTIVE_LOW ? LOW : HIGH;
  const int inactiveLevel = BUZZER_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(BUZZER_PIN, on ? activeLevel : inactiveLevel);
}

static void updateBuzzer() {
  if (buzzerMuted) {
    if (buzzerOutputOn) {
      setBuzzer(false);
    }
    return;
  }

  uint32_t intervalMs = 0;
  if (systemState == STATE_COLLISION_CONFIRM) {
    intervalMs = BUZZER_COLLISION_INTERVAL_MS;
  } else if (systemState == STATE_WARNING && currentFusionRisk() == RISK_WARN) {
    intervalMs = BUZZER_WARNING_INTERVAL_MS;
  }

  if (intervalMs == 0) {
    if (buzzerOutputOn) {
      setBuzzer(false);
    }
    return;
  }

  const uint32_t now = millis();
  if (now - lastBuzzerToggleMs >= intervalMs) {
    lastBuzzerToggleMs = now;
    setBuzzer(!buzzerOutputOn);
  }
}

static void printBytesHex(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (data[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print(data[i], HEX);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
}

static bool oledSafeJudgement() {
  return !collisionRecentlyHit() && !collisionStableActive &&
         (!hasFreshBsdFrame() || latestFrame.targetCount == 0);
}

static uint8_t displayTargetPageStart() {
  if (!hasFreshBsdFrame() || latestFrame.targetCount == 0) {
    return 0;
  }

  const uint8_t pageCount = static_cast<uint8_t>(
      (latestFrame.targetCount + OLED_TARGETS_PER_PAGE - 1) /
      OLED_TARGETS_PER_PAGE);
  const uint8_t page = static_cast<uint8_t>((millis() / TARGET_PAGE_MS) % pageCount);
  return static_cast<uint8_t>(page * OLED_TARGETS_PER_PAGE);
}

static void refreshLcd(bool force);
static void refreshSecondTft(bool force);
static void sendAppStatus(bool force);

static void updateCollisionSensor() {
  const bool rawActive = readCollisionActive();
  const uint32_t now = millis();

  if (rawActive != collisionRawActive) {
    collisionRawActive = rawActive;
    collisionRawChangedMs = now;
  }

  if (now - collisionRawChangedMs < COLLISION_DEBOUNCE_MS ||
      rawActive == collisionStableActive) {
    return;
  }

  collisionStableActive = rawActive;
  if (collisionStableActive) {
    collisionLastHitMs = now;
    ++collisionCount;
    setSystemState(STATE_COLLISION_CONFIRM);
    Serial.print("COLLISION hit count=");
    Serial.println(collisionCount);
  } else {
    Serial.println("COLLISION released");
  }
  refreshLcd(true);
  sendAppStatus(true);
}

static void drawRadarPage() {
  char line[32];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  const bool freshBsd = hasFreshBsdFrame();
  const uint8_t targetCount = freshBsd ? latestFrame.targetCount : 0;
  const bool safe = oledSafeJudgement();

  if (targetCount > OLED_TARGETS_PER_PAGE) {
    const uint8_t pageCount = static_cast<uint8_t>(
        (targetCount + OLED_TARGETS_PER_PAGE - 1) / OLED_TARGETS_PER_PAGE);
    const uint8_t page = static_cast<uint8_t>(
        displayTargetPageStart() / OLED_TARGETS_PER_PAGE + 1);
    snprintf(line, sizeof(line), "SAFE:%s T:%u P:%u/%u",
             safe ? "YES" : "NO", targetCount, page, pageCount);
  } else {
    snprintf(line, sizeof(line), "SAFE:%s T:%u", safe ? "YES" : "NO", targetCount);
  }
  drawTextLine(0, 9, line);
  u8g2.drawHLine(0, 11, 128);

  if (targetCount == 0) {
    drawTextLine(0, 27, "No radar targets");
    drawTextLine(0, 43, "D: -- m");
    drawTextLine(0, 59, "V: -- m/s");
    u8g2.sendBuffer();
    return;
  }

  const uint8_t start = displayTargetPageStart();
  for (uint8_t row = 0; row < OLED_TARGETS_PER_PAGE; ++row) {
    const uint8_t targetIndex = static_cast<uint8_t>(start + row);
    if (targetIndex >= targetCount) {
      break;
    }

    const RadarTarget &target = latestFrame.targets[targetIndex];
    snprintf(line, sizeof(line), "%u %s D:%2.0fm V:%3.0fm/s",
             targetIndex + 1, zoneText(&target),
             target.distanceM, target.speedMps);
    drawTextLine(0, static_cast<int>(24 + row * 12), line);
  }

  u8g2.sendBuffer();
}

static void refreshLcd(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - lastLcdMs < LCD_REFRESH_MS) {
    return;
  }

  drawRadarPage();
  lastLcdMs = now;
}

static void drawSecondTftBootPage() {
  secondTft.fillScreen(ST77XX_BLACK);
  secondTft.fillRect(0, 0, 40, 12, ST77XX_RED);
  secondTft.fillRect(40, 0, 40, 12, ST77XX_GREEN);
  secondTft.fillRect(80, 0, 40, 12, ST77XX_BLUE);
  secondTft.fillRect(120, 0, 40, 12, ST77XX_WHITE);
  secondTft.setTextWrap(false);
  secondTft.setTextSize(1);
  secondTft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  secondTft.setCursor(4, 22);
  secondTft.print("SECOND TFT OK");
  secondTft.setCursor(4, 36);
  secondTft.print("SPI ST7789 TEST");
  secondTft.setCursor(4, 50);
  secondTft.print("SCK18 MOSI23");
  secondTft.setCursor(4, 64);
  secondTft.print("CS13 DC14 RST26");
  secondTft.setCursor(4, 78);
  secondTft.print("BL32 or 3V3");
}

static uint16_t tftRgb(uint8_t r, uint8_t g, uint8_t b) {
  return secondTft.color565(r, g, b);
}

static uint16_t tftLevelColor(uint8_t level) {
  switch (level) {
    case 5:
      return ST77XX_RED;
    case 4:
      return ST77XX_ORANGE;
    case 3:
      return ST77XX_YELLOW;
    case 2:
    case 1:
      return ST77XX_CYAN;
    case 0:
    default:
      return tftRgb(96, 104, 112);
  }
}

static uint8_t distanceDangerLevel(const RadarTarget *target) {
  if (target == nullptr) {
    return 0;
  }

  float distance = target->distanceM;
  if (distance < 0.0f) {
    distance = -distance;
  }

  if (distance <= 0.5f) {
    return 5;
  }
  if (distance <= 1.0f) {
    return 4;
  }
  if (distance <= 1.5f) {
    return 3;
  }
  if (distance <= 2.0f) {
    return 2;
  }
  if (distance <= 3.0f) {
    return 1;
  }
  return 0;
}

static uint8_t targetDangerLevel(const RadarTarget *target) {
  if (target == nullptr) {
    return 0;
  }

  const uint8_t distanceLevel = distanceDangerLevel(target);
  const uint8_t riskLevel = dangerLevel(fusionRiskForTarget(target));
  return distanceLevel >= riskLevel ? distanceLevel : riskLevel;
}

static uint8_t statusDangerLevel(const RadarTarget *target, FusionRisk risk) {
  if (systemState == STATE_COLLISION_CONFIRM || systemState == STATE_ALARMING ||
      risk == RISK_HIT) {
    return 5;
  }
  if (systemState == STATE_FALSE_ALARM) {
    return 0;
  }
  return targetDangerLevel(target);
}

static void clearZoneSummaries(RadarZoneSummary zones[ZONE_COUNT]) {
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    zones[i].targetCount = 0;
    zones[i].level = 0;
    zones[i].risk = RISK_OK;
    zones[i].bestTarget = nullptr;
  }
}

static void summarizeFrameZones(const RadarFrame &frame,
                                RadarZoneSummary zones[ZONE_COUNT]) {
  clearZoneSummaries(zones);

  for (uint8_t i = 0; i < frame.targetCount; ++i) {
    const RadarTarget *target = &frame.targets[i];
    const uint8_t zone = targetZoneIndex(target);
    if (zone >= ZONE_COUNT) {
      continue;
    }

    RadarZoneSummary &summary = zones[zone];
    ++summary.targetCount;

    const FusionRisk targetRisk = fusionRiskForTarget(target);
    const uint8_t targetLevel = targetDangerLevel(target);
    if (summary.bestTarget == nullptr ||
        static_cast<uint8_t>(targetRisk) > static_cast<uint8_t>(summary.risk) ||
        (targetRisk == summary.risk &&
         target->distanceM < summary.bestTarget->distanceM)) {
      summary.bestTarget = target;
      summary.risk = targetRisk;
    }
    if (targetLevel > summary.level) {
      summary.level = targetLevel;
    }
  }
}

static uint8_t activeZoneMask(const RadarZoneSummary zones[ZONE_COUNT]) {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    if (zones[i].targetCount > 0) {
      mask = static_cast<uint8_t>(mask | (1U << i));
    }
  }
  return mask;
}

static void tftSetAsciiText(uint8_t size, uint16_t color, uint16_t bg = ST77XX_BLACK) {
  secondTft.setTextWrap(false);
  secondTft.setTextSize(size);
  secondTft.setTextColor(color, bg);
}

static void tftPrintAt(int16_t x, int16_t y, const char *text,
                       uint8_t size, uint16_t color,
                       uint16_t bg = ST77XX_BLACK) {
  tftSetAsciiText(size, color, bg);
  secondTft.setCursor(x, y);
  secondTft.print(text);
}

static void tftPrintCentered(int16_t y, const char *text, uint8_t size,
                             uint16_t color, uint16_t bg = ST77XX_BLACK) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  tftSetAsciiText(size, color, bg);
  secondTft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>((secondTft.width() - w) / 2);
  secondTft.setCursor(x < 0 ? 0 : x, y);
  secondTft.print(text);
}

static void tftPrintCenteredInBox(int16_t boxX, int16_t boxW, int16_t y,
                                  const char *text, uint8_t size,
                                  uint16_t color, uint16_t bg) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t textW = 0;
  uint16_t textH = 0;
  tftSetAsciiText(size, color, bg);
  secondTft.getTextBounds(text, 0, y, &x1, &y1, &textW, &textH);
  const int16_t x = static_cast<int16_t>(boxX + (boxW - textW) / 2 - x1);
  secondTft.setCursor(x < boxX ? boxX : x, y);
  secondTft.print(text);
}

static void tftDrawHeader(const char *title, uint16_t color) {
  secondTft.fillRect(0, 0, secondTft.width(), 34, color);
  tftPrintCentered(9, title, 2, ST77XX_BLACK, color);
}

static void tftDrawZoneCard(int16_t x, int16_t y, int16_t w, int16_t h,
                            const char *label, const char *state,
                            bool active, uint16_t activeColor) {
  const uint16_t bg = active ? activeColor : tftRgb(28, 32, 38);
  const uint16_t border = active ? ST77XX_WHITE : tftRgb(72, 78, 86);
  const uint16_t text = active ? ST77XX_BLACK : tftRgb(210, 216, 222);

  secondTft.fillRoundRect(x, y, w, h, 6, bg);
  secondTft.drawRoundRect(x, y, w, h, 6, border);
  tftPrintCenteredInBox(x, w, y + 11, label, 1, text, bg);
  tftPrintCenteredInBox(x, w, y + 36, state, 1, text, bg);
}

static void tftZoneStateText(const RadarZoneSummary &zone,
                             char *text, size_t textSize) {
  if (zone.targetCount == 0) {
    snprintf(text, textSize, "SAFE");
    return;
  }
  snprintf(text, textSize, "%s x%u", riskText(zone.risk), zone.targetCount);
}

static void tftDrawZones(const RadarZoneSummary zones[ZONE_COUNT]) {
  static constexpr int16_t zoneY = 96;
  static constexpr int16_t zoneW = 70;
  static constexpr int16_t zoneH = 64;
  static constexpr int16_t zoneX[ZONE_COUNT] = {8, 85, 162};
  char stateText[ZONE_COUNT][12];

  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    tftZoneStateText(zones[i], stateText[i], sizeof(stateText[i]));
    const bool active = zones[i].targetCount > 0;
    const uint16_t activeColor = tftLevelColor(zones[i].level);
    tftDrawZoneCard(zoneX[i], zoneY, zoneW, zoneH, zoneLabelText(i),
                    stateText[i], active, activeColor);
  }
}

static void drawSecondTftSafePage() {
  RadarZoneSummary zones[ZONE_COUNT];
  clearZoneSummaries(zones);

  secondTft.fillScreen(ST77XX_BLACK);
  tftDrawHeader("BSD MONITOR", tftRgb(40, 180, 110));
  tftPrintCentered(52, "SYSTEM SAFE", 2, ST77XX_GREEN);
  tftDrawZones(zones);
  tftPrintCentered(184, "Monitoring blind spots", 1, tftRgb(180, 188, 196));
  tftPrintCentered(202, "No dangerous target", 1, tftRgb(116, 220, 150));
}

static void drawSecondTftWarningPage(const RadarZoneSummary zones[ZONE_COUNT],
                                     uint8_t level, uint8_t totalTargets) {
  const uint16_t levelColor = tftLevelColor(level);
  char line[32];

  secondTft.fillScreen(ST77XX_BLACK);
  tftDrawHeader("BSD WARNING", levelColor);
  tftPrintCentered(45, "Objects detected", 1, ST77XX_WHITE);
  snprintf(line, sizeof(line), "Danger Level: %u/5", level);
  tftPrintCentered(63, line, 2, levelColor);
  tftDrawZones(zones);

  snprintf(line, sizeof(line), "L:%u  C:%u  R:%u",
           zones[ZONE_LEFT].targetCount,
           zones[ZONE_CENTER].targetCount,
           zones[ZONE_RIGHT].targetCount);
  tftPrintCentered(184, line, 1, ST77XX_WHITE);
  snprintf(line, sizeof(line), "TOTAL TARGETS: %u", totalTargets);
  tftPrintCentered(204, line, 1, tftRgb(210, 216, 222));
}

static void drawSecondTftCollisionPage() {
  secondTft.fillScreen(ST77XX_RED);
  tftPrintCentered(42, "COLLISION", 3, ST77XX_WHITE, ST77XX_RED);
  tftPrintCentered(78, "DETECTED", 3, ST77XX_WHITE, ST77XX_RED);
  secondTft.fillRoundRect(18, 130, 204, 74, 8, ST77XX_BLACK);
  tftPrintCentered(146, "CHECK APP", 2, ST77XX_WHITE, ST77XX_BLACK);
  tftPrintCentered(172, "TO CONFIRM", 2, ST77XX_WHITE, ST77XX_BLACK);
}

static void drawSecondTftAlarmingPage() {
  secondTft.fillScreen(ST77XX_BLACK);
  tftDrawHeader("EMERGENCY", ST77XX_RED);
  tftPrintCentered(56, "ALARMING", 3, ST77XX_RED);
  tftPrintCentered(108, "ALARMING...", 2, ST77XX_WHITE);
  tftPrintCentered(136, "Emergency reporting", 1, ST77XX_WHITE);
  tftPrintCentered(154, "simulated", 1, ST77XX_WHITE);
  secondTft.fillRoundRect(36, 190, 168, 28, 6, tftRgb(30, 120, 92));
  tftPrintCentered(199, "Buzzer Muted", 1, ST77XX_WHITE, tftRgb(30, 120, 92));
}

static void drawSecondTftFalseAlarmPage() {
  secondTft.fillScreen(ST77XX_BLACK);
  tftDrawHeader("FALSE ALARM", ST77XX_YELLOW);
  tftPrintCentered(66, "Buzzer Muted", 2, ST77XX_WHITE);
  tftPrintCentered(104, "Returning...", 2, ST77XX_YELLOW);
  secondTft.drawRoundRect(28, 154, 184, 42, 8, ST77XX_YELLOW);
  tftPrintCentered(168, "Monitoring resumes", 1, ST77XX_WHITE);
}

static void initSecondTft() {
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  secondTft.init(TFT_WIDTH, TFT_HEIGHT);
  secondTft.setRotation(1);
  secondTft.setSPISpeed(27000000);
  secondTftStarted = true;
  drawSecondTftBootPage();
}

static void refreshSecondTft(bool force = false) {
  if (!secondTftStarted) {
    return;
  }

  const uint32_t now = millis();
  if (!force && now - lastSecondTftMs < SECOND_TFT_REFRESH_MS) {
    return;
  }
  const bool hasTarget = hasFreshBsdFrame() && latestFrame.targetCount > 0;
  const RadarTarget *target = hasTarget ? bestTargetForFrame(latestFrame) : nullptr;
  const FusionRisk risk = hasTarget ? fusionRiskForFrame(latestFrame)
                                    : fusionRiskForTarget(nullptr);
  const uint8_t level = statusDangerLevel(target, risk);
  RadarZoneSummary zones[ZONE_COUNT];
  if (hasTarget) {
    summarizeFrameZones(latestFrame, zones);
  } else {
    clearZoneSummaries(zones);
  }
  const uint8_t zoneMask = activeZoneMask(zones);

  static bool hasLastSnapshot = false;
  static SystemState lastState = STATE_SAFE;
  static uint8_t lastZoneMask = 0xFF;
  static uint8_t lastLevel = 255;
  static uint8_t lastTargetCount = 255;
  static uint8_t lastZoneCounts[ZONE_COUNT] = {255, 255, 255};
  static bool lastMuted = false;
  static bool lastBuzzerOn = false;

  lastSecondTftMs = now;
  bool sameZoneCounts = true;
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    if (lastZoneCounts[i] != zones[i].targetCount) {
      sameZoneCounts = false;
      break;
    }
  }

  if (!force && hasLastSnapshot && lastState == systemState &&
      lastZoneMask == zoneMask && lastLevel == level &&
      lastTargetCount == (hasTarget ? latestFrame.targetCount : 0) &&
      sameZoneCounts &&
      lastMuted == buzzerMuted && lastBuzzerOn == buzzerOutputOn) {
    return;
  }

  switch (systemState) {
    case STATE_COLLISION_CONFIRM:
      drawSecondTftCollisionPage();
      break;
    case STATE_ALARMING:
      drawSecondTftAlarmingPage();
      break;
    case STATE_FALSE_ALARM:
      drawSecondTftFalseAlarmPage();
      break;
    case STATE_WARNING:
      drawSecondTftWarningPage(zones, level,
                               hasTarget ? latestFrame.targetCount : 0);
      break;
    case STATE_SAFE:
    default:
      drawSecondTftSafePage();
      break;
  }

  hasLastSnapshot = true;
  lastState = systemState;
  lastZoneMask = zoneMask;
  lastLevel = level;
  lastTargetCount = hasTarget ? latestFrame.targetCount : 0;
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    lastZoneCounts[i] = zones[i].targetCount;
  }
  lastMuted = buzzerMuted;
  lastBuzzerOn = buzzerOutputOn;
}

static void writeAppStatus(Print &out) {
  const bool hasTarget = hasFreshBsdFrame() && latestFrame.targetCount > 0;
  const RadarTarget *target = hasTarget ? bestTargetForFrame(latestFrame) : nullptr;
  const FusionRisk risk = hasTarget ? fusionRiskForFrame(latestFrame)
                                    : fusionRiskForTarget(nullptr);
  const uint8_t level = statusDangerLevel(target, risk);

  out.print("STATE:");
  out.print(systemStateText(systemState));
  out.print(",RISK:");
  out.print(riskText(risk));
  out.print(",ZONE:");
  out.print(zoneText(target));
  out.print(",DIST:");
  out.print(target != nullptr ? target->distanceM : 0.0f, 2);
  out.print(",SPEED:");
  out.print(target != nullptr ? target->speedMps * 3.6f : 0.0f, 1);
  out.print(",ANGLE:");
  out.print(target != nullptr ? target->angleDeg : 0.0f, 0);
  out.print(",LEVEL:");
  out.print(level);
  out.print(",CNT:");
  out.print(hasTarget ? latestFrame.targetCount : 0);
  out.print(",ID:");
  out.print(target != nullptr ? target->targetId : 0);
  if (hasTarget) {
    for (uint8_t i = 0; i < latestFrame.targetCount; ++i) {
      const RadarTarget &t = latestFrame.targets[i];
      out.print(",T");
      out.print(i);
      out.print("_ID:");
      out.print(t.targetId);
      out.print(",T");
      out.print(i);
      out.print("_ZONE:");
      out.print(zoneText(&t));
      out.print(",T");
      out.print(i);
      out.print("_DIST:");
      out.print(t.distanceM, 2);
      out.print(",T");
      out.print(i);
      out.print("_SPEED:");
      out.print(t.speedMps * 3.6f, 1);
      out.print(",T");
      out.print(i);
      out.print("_ANGLE:");
      out.print(t.angleDeg, 0);
      out.print(",T");
      out.print(i);
      out.print("_LEVEL:");
      out.print(targetDangerLevel(&t));
    }
  }
  out.print(",COLLISION:");
  out.print(collisionText());
  out.print(",C_COUNT:");
  out.print(collisionCount);
  out.print(",BUZZER:");
  out.print(buzzerMuted ? "MUTED" : (buzzerOutputOn ? "ON" : "OFF"));
  if (systemState == STATE_COLLISION_CONFIRM) {
    out.print(",MSG:CONFIRM_REQUIRED");
  } else if (systemState == STATE_ALARMING) {
    out.print(",MSG:EMERGENCY_REPORTING");
  } else if (systemState == STATE_FALSE_ALARM) {
    out.print(",MSG:FALSE_ALARM_CONFIRMED");
  }
  out.println();
}

static void sendAppStatus(bool force = false) {
  if (!bluetoothStarted || !SerialBT.hasClient()) {
    return;
  }

  const uint32_t now = millis();
  if (!force && now - lastAppTxMs < APP_TX_INTERVAL_MS) {
    return;
  }

  writeAppStatus(SerialBT);
  lastAppTxMs = now;
}

static void printHex(const uint8_t *data, size_t len) {
  Serial.print("RAW len=");
  Serial.print(len);
  Serial.print(" hex=");
  printBytesHex(data, len);
  Serial.println();
}

static void printTxBytes(uint8_t command, const uint8_t *params, uint8_t paramLen,
                         uint16_t check) {
  Serial.print("TX bytes=");
  Serial.print("58 ");
  if (command < 0x10) {
    Serial.print('0');
  }
  Serial.print(command, HEX);
  Serial.print(' ');
  if (paramLen < 0x10) {
    Serial.print('0');
  }
  Serial.print(paramLen, HEX);
  for (uint8_t i = 0; i < paramLen; ++i) {
    Serial.print(' ');
    if (params[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print(params[i], HEX);
  }
  Serial.print(' ');
  const uint8_t checkLow = static_cast<uint8_t>(check & 0xFF);
  const uint8_t checkHigh = static_cast<uint8_t>((check >> 8) & 0xFF);
  if (checkLow < 0x10) {
    Serial.print('0');
  }
  Serial.print(checkLow, HEX);
  Serial.print(' ');
  if (checkHigh < 0x10) {
    Serial.print('0');
  }
  Serial.println(checkHigh, HEX);
}

static void printRadarRxIdleCheck() {
  pinMode(RADAR_RX_PIN, INPUT_PULLDOWN);
  delay(10);

  uint8_t highCount = 0;
  for (uint8_t i = 0; i < 20; ++i) {
    if (digitalRead(RADAR_RX_PIN) == HIGH) {
      ++highCount;
    }
    delay(2);
  }

  Serial.print("Radar TX->ESP RX idle check GPIO");
  Serial.print(RADAR_RX_PIN);
  Serial.print(": HIGH ");
  Serial.print(highCount);
  Serial.println("/20");
  if (highCount == 0) {
    Serial.println("Warning: GPIO16 is LOW with pulldown; radar TX may be disconnected, unpowered, or not idle-high.");
  }
}

static void beginRadarUart(uint32_t baud) {
  Serial2.end();
  delay(20);
  currentRadarBaud = baud;
  rxLen = 0;
  lastRawLen = 0;
  Serial2.setRxBufferSize(1024);
  Serial2.begin(currentRadarBaud, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
}

static void sendHciCommand(uint8_t command, const uint8_t *params, uint8_t paramLen) {
  uint16_t check = 0x58;
  check += command;
  check += paramLen;
  for (uint8_t i = 0; i < paramLen; ++i) {
    check += params[i];
  }

  Serial2.write(0x58);
  Serial2.write(command);
  Serial2.write(paramLen);
  if (paramLen > 0) {
    Serial2.write(params, paramLen);
  }
  Serial2.write(static_cast<uint8_t>(check & 0xFF));
  Serial2.write(static_cast<uint8_t>((check >> 8) & 0xFF));
  Serial2.flush();

  Serial.print("Radar TX HCI cmd=0x");
  if (command < 0x10) {
    Serial.print('0');
  }
  Serial.print(command, HEX);
  Serial.print(" len=");
  Serial.print(paramLen);
  Serial.print(" check=0x");
  if (check < 0x1000) {
    Serial.print('0');
  }
  if (check < 0x0100) {
    Serial.print('0');
  }
  if (check < 0x0010) {
    Serial.print('0');
  }
  Serial.println(check, HEX);
  printTxBytes(command, params, paramLen, check);
}

static void openRadarSensing() {
  static constexpr uint8_t enable[] = {0x01};
  sendHciCommand(0xD1, enable, sizeof(enable));
}

static void getRadarVersion() {
  sendHciCommand(0xFE, nullptr, 0);
}

static void getRadarSensingState() {
  sendHciCommand(0xD0, nullptr, 0);
}

static void runAutoProbe() {
  if (!autoProbeEnabled || radarLocked) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastAutoProbeMs < AUTO_COMMAND_INTERVAL_MS) {
    return;
  }
  lastAutoProbeMs = now;

  switch (autoProbeCommandIndex) {
    case 0:
      lastAutoProbeCommand = 0xFE;
      getRadarVersion();
      break;
    case 1:
      lastAutoProbeCommand = 0xD0;
      getRadarSensingState();
      break;
    default:
      lastAutoProbeCommand = 0xD1;
      openRadarSensing();
      break;
  }

  autoProbeCommandIndex = static_cast<uint8_t>((autoProbeCommandIndex + 1) % 3);
}

static void printCommandHelp() {
  Serial.println("USB commands:");
  Serial.println("  L                  loopback test, short D17 to D16 first");
  Serial.println("  B 115200           set radar UART baud");
  Serial.println("  H A5 5A 01 02      send HEX bytes to radar UART");
  Serial.println("  T text             send ASCII text + CRLF to radar UART");
  Serial.println("  O                  send HCI open radar sensing command");
  Serial.println("  C                  clear collision latch/count");
  Serial.println("  MUTE               mute buzzer without leaving current app state");
  Serial.println("Auto init: fixed 921600, sends FE/D0/D1 until a valid 0x59/0x5A frame is verified.");
  Serial.println("Display: fused risk + BSD TYPE 0x07 fields: count/id/range/speed/angle.");
}

static void runUartLoopbackTest() {
  static constexpr uint8_t testPattern[] = {
      0x55, 0xAA, 0x11, 0x22, 0x33, 0x44, 0x5A, 0xA5
  };
  uint8_t received[sizeof(testPattern)]{};
  size_t receivedLen = 0;

  while (Serial2.available() > 0) {
    Serial2.read();
  }

  Serial.println();
  Serial.print("UART loopback test: short D17(GPIO");
  Serial.print(RADAR_TX_PIN);
  Serial.print(") to D16(GPIO");
  Serial.print(RADAR_RX_PIN);
  Serial.println("), then send L.");
  Serial.print("TX pattern: ");
  printBytesHex(testPattern, sizeof(testPattern));
  Serial.println();

  Serial2.write(testPattern, sizeof(testPattern));
  Serial2.flush();

  const uint32_t startMs = millis();
  while (millis() - startMs < 200 && receivedLen < sizeof(received)) {
    if (Serial2.available() > 0) {
      const int value = Serial2.read();
      if (value >= 0) {
        received[receivedLen++] = static_cast<uint8_t>(value);
      }
    }
  }

  Serial.print("RX len=");
  Serial.print(receivedLen);
  Serial.print(" hex=");
  printBytesHex(received, receivedLen);
  Serial.println();

  if (receivedLen == sizeof(testPattern) &&
      memcmp(received, testPattern, sizeof(testPattern)) == 0) {
    Serial.println("Loopback PASS: ESP32 D16/D17 UART path works.");
  } else {
    Serial.println("Loopback FAIL: check D16/D17 pin positions or jumper contact.");
  }
}

static char *skipSpaces(char *text) {
  while (*text == ' ' || *text == '\t') {
    ++text;
  }
  return text;
}

static void sendHexCommand(char *args) {
  uint8_t bytes[64];
  size_t byteCount = 0;
  char *cursor = skipSpaces(args);

  while (*cursor != '\0' && byteCount < sizeof(bytes)) {
    char *end = nullptr;
    const unsigned long value = strtoul(cursor, &end, 16);
    if (end == cursor || value > 0xFF) {
      Serial.println("HEX parse error. Example: H A5 5A 01 02");
      return;
    }

    bytes[byteCount++] = static_cast<uint8_t>(value);
    cursor = skipSpaces(end);
    if (*cursor == ',' || *cursor == ';') {
      ++cursor;
      cursor = skipSpaces(cursor);
    }
  }

  if (byteCount == 0) {
    Serial.println("No HEX bytes to send.");
    return;
  }

  Serial.print("Radar TX HEX len=");
  Serial.print(byteCount);
  Serial.print(" hex=");
  printBytesHex(bytes, byteCount);
  Serial.println();

  Serial2.write(bytes, byteCount);
  Serial2.flush();
}

static void sendTextCommand(char *args) {
  char *text = skipSpaces(args);
  if (*text == '\0') {
    Serial.println("No text to send.");
    return;
  }

  Serial.print("Radar TX text: ");
  Serial.println(text);
  Serial2.print(text);
  Serial2.print("\r\n");
  Serial2.flush();
}

static bool equalsIgnoreCase(const char *left, const char *right) {
  while (*left != '\0' && *right != '\0') {
    if (toupper(static_cast<unsigned char>(*left)) !=
        toupper(static_cast<unsigned char>(*right))) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static void trimTrailingSpaces(char *text) {
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                     text[len - 1] == '\r' || text[len - 1] == '\n')) {
    text[--len] = '\0';
  }
}

static bool handleAppCommand(char *line) {
  char *command = skipSpaces(line);
  trimTrailingSpaces(command);
  if (*command == '\0') {
    return true;
  }

  if (equalsIgnoreCase(command, "ALARM")) {
    setSystemState(STATE_ALARMING);
    collisionLastHitMs = 0;
    buzzerMuted = true;
    setBuzzer(false);
    Serial.println("APP CMD ALARM");
    refreshLcd(true);
    sendAppStatus(true);
    return true;
  }

  if (equalsIgnoreCase(command, "FALSE")) {
    setSystemState(STATE_FALSE_ALARM);
    collisionLastHitMs = 0;
    buzzerMuted = true;
    setBuzzer(false);
    Serial.println("APP CMD FALSE");
    refreshLcd(true);
    sendAppStatus(true);
    return true;
  }

  if (equalsIgnoreCase(command, "RESET")) {
    collisionLastHitMs = 0;
    buzzerMuted = false;
    setBuzzer(false);
    setSystemState(hasFreshBsdFrame() && latestFrame.targetCount > 0 ? STATE_WARNING
                                                                     : STATE_SAFE);
    Serial.println("APP CMD RESET");
    refreshLcd(true);
    sendAppStatus(true);
    return true;
  }

  if (equalsIgnoreCase(command, "MUTE")) {
    buzzerMuted = true;
    setBuzzer(false);
    Serial.println("APP CMD MUTE");
    refreshLcd(true);
    sendAppStatus(true);
    return true;
  }

  return false;
}

static void handleUsbCommand(char *line) {
  char *command = skipSpaces(line);
  if (*command == '\0') {
    return;
  }

  if (handleAppCommand(command)) {
    return;
  }

  const char op = toupper(static_cast<unsigned char>(*command));
  char *args = command + 1;

  switch (op) {
    case '?':
      printCommandHelp();
      break;

    case 'L':
      runUartLoopbackTest();
      rxLen = 0;
      break;

    case 'B': {
      const uint32_t baud = strtoul(skipSpaces(args), nullptr, 10);
      if (baud == 0) {
        Serial.println("Invalid baud. Example: B 115200");
        break;
      }

      beginRadarUart(baud);
      autoProbeEnabled = false;
      radarLocked = false;
      sentOpenAfterLock = false;
      hasLatestFrame = false;
      Serial.print("Radar UART changed to ");
      Serial.print(currentRadarBaud);
      Serial.println(" 8N1");
      break;
    }

    case 'H':
      sendHexCommand(args);
      break;

    case 'T':
      sendTextCommand(args);
      break;

    case 'O':
      openRadarSensing();
      break;

    case 'C':
      collisionLastHitMs = 0;
      collisionCount = 0;
      setSystemState(hasFreshBsdFrame() && latestFrame.targetCount > 0 ? STATE_WARNING
                                                                       : STATE_SAFE);
      Serial.println("Collision latch/count cleared.");
      refreshLcd(true);
      sendAppStatus(true);
      break;

    default:
      Serial.println("Unknown command. Type ? for help.");
      break;
  }
}

static void readUsbCommands() {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) {
      continue;
    }

    const char ch = static_cast<char>(value);
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      usbLine[usbLineLen] = '\0';
      handleUsbCommand(usbLine);
      usbLineLen = 0;
      continue;
    }

    if (usbLineLen + 1 < USB_LINE_SIZE) {
      usbLine[usbLineLen++] = ch;
    } else {
      usbLineLen = 0;
      Serial.println("USB command too long; discarded.");
    }
  }
}

static void handleBluetoothCommand(char *line) {
  if (handleAppCommand(line)) {
    return;
  }

  char *command = skipSpaces(line);
  trimTrailingSpaces(command);
  if (equalsIgnoreCase(command, "?")) {
    writeAppStatus(SerialBT);
  } else {
    SerialBT.print("ERR:UNKNOWN_CMD ");
    SerialBT.println(command);
  }
}

static void readBluetoothCommands() {
  if (!bluetoothStarted || !SerialBT.hasClient()) {
    btLineLen = 0;
    return;
  }

  while (SerialBT.available() > 0) {
    const int value = SerialBT.read();
    if (value < 0) {
      continue;
    }

    const char ch = static_cast<char>(value);
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      btLine[btLineLen] = '\0';
      handleBluetoothCommand(btLine);
      btLineLen = 0;
      continue;
    }

    if (btLineLen + 1 < USB_LINE_SIZE) {
      btLine[btLineLen++] = ch;
    } else {
      btLineLen = 0;
      SerialBT.println("ERR:CMD_TOO_LONG");
    }
  }
}

static void printTargetsJson(const RadarFrame &frame) {
  const FusionRisk risk = fusionRiskForFrame(frame);

  Serial.print("{\"risk\":\"");
  Serial.print(riskText(risk));
  Serial.print("\",\"collision\":\"");
  Serial.print(collisionText());
  Serial.print("\",\"collision_count\":");
  Serial.print(collisionCount);
  Serial.print(",\"target_count\":");
  Serial.print(frame.targetCount);
  Serial.print(",\"targets\":[");

  for (uint8_t i = 0; i < frame.targetCount; ++i) {
    const RadarTarget &t = frame.targets[i];
    if (i > 0) {
      Serial.print(',');
    }

    Serial.print("{\"target_id\":");
    Serial.print(t.targetId);
    Serial.print(",\"distance_m\":");
    Serial.print(t.distanceM, 2);
    Serial.print(",\"speed_mps\":");
    Serial.print(t.speedMps, 2);
    Serial.print(",\"speed_kmph\":");
    Serial.print(t.speedMps * 3.6f, 2);
    Serial.print(",\"angle_deg\":");
    Serial.print(t.angleDeg, 1);
    Serial.print(",\"zone\":\"");
    Serial.print(zoneText(&t));
    Serial.print('"');
    Serial.print('}');
  }

  Serial.println("]}");
}

static uint8_t checksum8(const uint8_t *data, size_t len) {
  uint8_t check = 0;
  for (size_t i = 0; i < len; ++i) {
    check = static_cast<uint8_t>(check + data[i]);
  }
  return check;
}

static uint16_t checksum16(const uint8_t *data, size_t len) {
  uint16_t check = 0;
  for (size_t i = 0; i < len; ++i) {
    check = static_cast<uint16_t>(check + data[i]);
  }
  return check;
}

static void consumeRxBytes(size_t count) {
  if (count >= rxLen) {
    rxLen = 0;
    return;
  }

  memmove(rxBuffer, rxBuffer + count, rxLen - count);
  rxLen -= count;
}

static void logDroppedBytes(const uint8_t *data, size_t len, const char *reason) {
  if (len == 0) {
    return;
  }

  noiseByteCount += static_cast<uint32_t>(len);
  lastRawLen = len;

  const uint32_t now = millis();
  if (now - lastNoiseLogMs < NOISE_LOG_INTERVAL_MS) {
    return;
  }
  lastNoiseLogMs = now;

  Serial.print("UART drop len=");
  Serial.print(len);
  Serial.print(" baud=");
  Serial.print(currentRadarBaud);
  Serial.print(" reason=");
  Serial.print(reason);
  Serial.print(" hex=");
  const size_t shown = len < 24 ? len : 24;
  printBytesHex(data, shown);
  if (shown < len) {
    Serial.print(" ...");
  }
  Serial.println();
}

static void lockRadarProtocol(uint8_t frameHeader) {
  lastFrameHeader = frameHeader;
  if (radarLocked) {
    return;
  }

  radarLocked = true;
  autoProbeEnabled = false;
  Serial.print("Radar protocol locked at ");
  Serial.print(currentRadarBaud);
  Serial.print(" bps by frame 0x");
  Serial.println(frameHeader, HEX);
}

static bool parseBsdPayload(const uint8_t *payload, uint8_t payloadLen, RadarFrame &frame) {
  if (payloadLen < 5) {
    Serial.println("BSD report too short");
    return false;
  }

  const uint16_t reportedCount = readLe16(&payload[1]);
  if (reportedCount > MAX_TARGETS) {
    Serial.print("BSD object count too large: ");
    Serial.println(reportedCount);
    return false;
  }

  const uint8_t targetCount = static_cast<uint8_t>(reportedCount);
  const size_t expectedPayloadLen = 1 + 4 + static_cast<size_t>(targetCount) * 4;
  if (payloadLen < expectedPayloadLen) {
    Serial.println("BSD report object data truncated");
    return false;
  }

  frame = {};
  frame.targetCount = targetCount;
  for (uint8_t i = 0; i < targetCount; ++i) {
    const uint8_t *obj = &payload[5 + static_cast<size_t>(i) * 4];
    frame.targets[i].distanceM = static_cast<float>(static_cast<int8_t>(obj[0]));
    frame.targets[i].angleDeg = static_cast<float>(static_cast<int8_t>(obj[1]));
    frame.targets[i].speedMps = static_cast<float>(static_cast<int8_t>(obj[2]));
    frame.targets[i].targetId = obj[3];
  }

  return true;
}

static void clearLatestBsdFrame() {
  latestFrame = {};
  latestFrameMs = 0;
  hasLatestFrame = false;
}

static void handleHciFrame(const uint8_t *data, size_t len) {
  lastRawLen = len;
  ++validFrameCount;
  lockRadarProtocol(0x59);

  const uint8_t command = data[1];
  const uint8_t paramLen = data[2];
  const uint8_t *params = &data[3];
  lastHciCommand = command;
  hasLastHciStatus = paramLen > 0;
  if (hasLastHciStatus) {
    lastHciStatus = params[0];
  }

  Serial.print("HCI RX cmd=0x");
  if (command < 0x10) {
    Serial.print('0');
  }
  Serial.print(command, HEX);
  Serial.print(" len=");
  Serial.print(paramLen);
  Serial.print(" params=");
  printBytesHex(params, paramLen);
  Serial.println();
  printHex(data, len);

  const bool sensingAlreadyOn =
      command == 0xD0 && paramLen > 0 && params[0] == 0x01;
  const bool openCommandOk =
      command == 0xD1 && paramLen > 0 && params[0] == 0x00;
  if (sensingAlreadyOn || openCommandOk) {
    sentOpenAfterLock = true;
  }

  if (!sentOpenAfterLock && command != 0xD1) {
    openRadarSensing();
  }

  refreshLcd(true);
}

static void handleReportFrame(const uint8_t *data, size_t len) {
  lastRawLen = len;
  ++validFrameCount;
  lockRadarProtocol(0x5A);

  const uint8_t payloadLen = data[1];
  const uint8_t *payload = &data[2];
  const uint8_t type = payloadLen > 0 ? payload[0] : 0x00;

  Serial.print("REPORT RX type=0x");
  if (type < 0x10) {
    Serial.print('0');
  }
  Serial.print(type, HEX);
  Serial.print(" len=");
  Serial.println(payloadLen);
  printHex(data, len);

  if (type != 0x07) {
    Serial.println("Report ignored: not BSD TYPE 0x07");
    refreshLcd(true);
    return;
  }

  RadarFrame frame{};
  if (!parseBsdPayload(payload, payloadLen, frame)) {
    refreshLcd(true);
    return;
  }

  if (frame.targetCount == 0) {
    Serial.println("BSD target_count=0; clear displayed targets.");
    clearLatestBsdFrame();
    updateSystemStateFromSensors();
    sendAppStatus(true);
    refreshLcd(true);
    return;
  }

  latestFrame = frame;
  latestFrameMs = millis();
  hasLatestFrame = true;
  updateSystemStateFromSensors();
  printTargetsJson(frame);
  sendAppStatus(true);
  refreshLcd(true);
}

static void processRadarStream() {
  while (rxLen > 0) {
    size_t headerOffset = 0;
    while (headerOffset < rxLen &&
           rxBuffer[headerOffset] != 0x59 &&
           rxBuffer[headerOffset] != 0x5A) {
      ++headerOffset;
    }

    if (headerOffset > 0) {
      logDroppedBytes(rxBuffer, headerOffset, "no frame header");
      consumeRxBytes(headerOffset);
      continue;
    }

    if (rxBuffer[0] == 0x59) {
      if (rxLen < 3) {
        return;
      }

      const uint8_t paramLen = rxBuffer[2];
      const size_t frameLen = 5 + static_cast<size_t>(paramLen);
      if (frameLen > RX_BUFFER_SIZE) {
        logDroppedBytes(rxBuffer, 1, "oversize HCI frame");
        consumeRxBytes(1);
        continue;
      }
      if (rxLen < frameLen) {
        return;
      }

      const uint16_t calculated = checksum16(rxBuffer, frameLen - 2);
      const uint16_t received = readLe16(&rxBuffer[frameLen - 2]);
      if (calculated == received) {
        handleHciFrame(rxBuffer, frameLen);
        consumeRxBytes(frameLen);
      } else {
        logDroppedBytes(rxBuffer, 1, "bad HCI checksum");
        consumeRxBytes(1);
      }
      continue;
    }

    if (rxLen < 2) {
      return;
    }

    const uint8_t payloadLen = rxBuffer[1];
    const size_t frameLen = 3 + static_cast<size_t>(payloadLen);
    if (frameLen > RX_BUFFER_SIZE) {
      logDroppedBytes(rxBuffer, 1, "oversize report frame");
      consumeRxBytes(1);
      continue;
    }
    if (rxLen < frameLen) {
      return;
    }

    const uint8_t calculated = checksum8(rxBuffer, frameLen - 1);
    const uint8_t received = rxBuffer[frameLen - 1];
    if (calculated == received) {
      handleReportFrame(rxBuffer, frameLen);
      consumeRxBytes(frameLen);
    } else {
      logDroppedBytes(rxBuffer, 1, "bad report checksum");
      consumeRxBytes(1);
    }
  }
}

static void appendRadarByte(uint8_t value) {
  ++rxByteCount;
  if (rxLen >= RX_BUFFER_SIZE) {
    logDroppedBytes(rxBuffer, rxLen, "rx buffer overflow");
    rxLen = 0;
  }

  rxBuffer[rxLen++] = value;
  processRadarStream();
}

static void printLinkStatus() {
  const uint32_t now = millis();
  if (now - lastLinkStatusMs < LINK_STATUS_INTERVAL_MS) {
    return;
  }
  lastLinkStatusMs = now;

  Serial.print("LINK baud=");
  Serial.print(currentRadarBaud);
  Serial.print(" rxBytes=");
  Serial.print(rxByteCount);
  Serial.print(" validFrames=");
  Serial.print(validFrameCount);
  Serial.print(" noise=");
  Serial.print(noiseByteCount);
  Serial.print(" collision=");
  Serial.print(collisionText());
  Serial.print(" collisionCount=");
  Serial.print(collisionCount);
  Serial.print(" buzzer=");
  Serial.print(buzzerMuted ? "MUTED" : (buzzerOutputOn ? "ON" : "OFF"));
  Serial.print(" rxPin=");
  Serial.println(digitalRead(RADAR_RX_PIN) == HIGH ? "HIGH" : "LOW");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("MS60-1211S80M-BSD ESP32 UART reader");
  Serial.println("Parser: AT6010 HCI 0x59 replies and 0x5A BSD reports with checksum.");

  bluetoothStarted = SerialBT.begin(BT_DEVICE_NAME);
  Serial.print("Bluetooth SPP: ");
  Serial.print(bluetoothStarted ? "ON, name=" : "FAILED, name=");
  Serial.println(BT_DEVICE_NAME);

  pinMode(COLLISION_PIN, COLLISION_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  collisionRawActive = readCollisionActive();
  collisionStableActive = collisionRawActive;
  collisionRawChangedMs = millis();
  pinMode(BUZZER_PIN, OUTPUT);
  setBuzzer(false);

  u8g2.setBusClock(LCD_I2C_CLOCK);
  u8g2.begin();
  refreshLcd(true);
  initSecondTft();

  printRadarRxIdleCheck();
  beginRadarUart(RADAR_BAUD);
  Serial.print("Radar UART: ");
  Serial.print(currentRadarBaud);
  Serial.print(" 8N1, RX=");
  Serial.print(RADAR_RX_PIN);
  Serial.print(", TX=");
  Serial.println(RADAR_TX_PIN);
  Serial.print("OLED I2C: SCL=");
  Serial.print(LCD_SCL_PIN);
  Serial.print(", SDA=");
  Serial.print(LCD_SDA_PIN);
  Serial.print(", clock=");
  Serial.print(LCD_I2C_CLOCK / 1000);
  Serial.println(" kHz");
  Serial.print("Collision sensor: OUT=GPIO");
  Serial.print(COLLISION_PIN);
  Serial.print(", trigger=");
  Serial.println(COLLISION_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.print("Buzzer module: I/O=GPIO");
  Serial.print(BUZZER_PIN);
  Serial.print(", trigger=");
  Serial.println(BUZZER_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.print("Second TFT SPI: SCL/SCK=GPIO");
  Serial.print(TFT_SCLK_PIN);
  Serial.print(", SDA/MOSI=GPIO");
  Serial.print(TFT_MOSI_PIN);
  Serial.print(", CS=GPIO");
  Serial.print(TFT_CS_PIN);
  Serial.print(", DC=GPIO");
  Serial.print(TFT_DC_PIN);
  Serial.print(", RST=GPIO");
  Serial.print(TFT_RST_PIN);
  Serial.print(", BL=GPIO");
  Serial.println(TFT_BL_PIN);
  printCommandHelp();
}

void loop() {
  updateCollisionSensor();
  readUsbCommands();
  readBluetoothCommands();
  runAutoProbe();

  while (Serial2.available() > 0) {
    const int value = Serial2.read();
    if (value < 0) {
      continue;
    }

    appendRadarByte(static_cast<uint8_t>(value));
  }

  updateSystemStateFromSensors();
  updateBuzzer();
  printLinkStatus();
  sendAppStatus();
  refreshLcd();
  refreshSecondTft();
}
