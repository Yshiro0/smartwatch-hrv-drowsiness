#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "MAX30105.h"
#include "heartRate.h"

// ─── Colors ───────────────────────────────────────────────────────────────────
#define BLACK    0x0000
#define WHITE    0xFFFF
#define GRAY     0x8410
#define DARKGRAY 0x2104
#define CRIMSON  0xF82E
#define GREEN    0x07E6
#define AMBER    0xF7E0
#define CYAN     0x4F9F
#define PURPLE   0x981F
#define ORANGE   0xFC80

// ─── Display pins (ESP32-C3 Super Mini + GC9A01) ─────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32SPI(1, 5, 2, 4, -1);
Arduino_GFX    *gfx = new Arduino_GC9A01(bus, 0, 0, true);
#define CX 120

// ─── I2C (shared bus for MAX30102 + MPU6050) ─────────────────────────────────
#define SDA_PIN 6
#define SCL_PIN 7

// ─── MAX30102 ─────────────────────────────────────────────────────────────────
MAX30105 particleSensor;

#define RATE_SIZE 8
uint32_t rrIntervals[RATE_SIZE] = {};
byte     rateSpot    = 0;
long     lastBeat    = 0;
int      bpm         = 0;
bool     fingerOn    = false;

float sdnn = 0;

// ─── MPU6050 ──────────────────────────────────────────────────────────────────
#define MPU_ADDR 0x68
float tiltAngle = 0;
float accelMag  = 0;
unsigned long lastImuUpdate = 0;

void mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0);    // wake
  Wire.endTransmission();
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); Wire.write(0x08); // ±4g
  Wire.endTransmission();
}

bool mpuRead(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(MPU_ADDR, 6);
  if (Wire.available() < 6) return false;
  int16_t rx = (Wire.read()<<8)|Wire.read();
  int16_t ry = (Wire.read()<<8)|Wire.read();
  int16_t rz = (Wire.read()<<8)|Wire.read();
  ax = rx / 8192.0f;
  ay = ry / 8192.0f;
  az = rz / 8192.0f;
  return true;
}

// ─── Calibration (3 minutes) ─────────────────────────────────────────────────
#define CALIB_MS 180000UL
bool  calibDone      = false;
unsigned long calibStart = 0;
float baselineSDNN   = 0;
float baselineTilt   = 0;
int   calibSamples   = 0;
float calibSDNNSum   = 0;
float calibTiltSum   = 0;

// ─── Drowsiness ───────────────────────────────────────────────────────────────
float drowsinessScore    = 0;
unsigned long lastDrowsyUpdate = 0;

// ─── BLE ──────────────────────────────────────────────────────────────────────
#define SERVICE_UUID       "12345678-1234-1234-1234-123456789abc"
#define CHAR_SENSOR_UUID   "12345678-1234-1234-1234-123456789abd"
#define CHAR_LOCATION_UUID "12345678-1234-1234-1234-123456789abe"

BLEServer         *pServer      = nullptr;
BLECharacteristic *pSensorChar  = nullptr;
BLECharacteristic *pLocationChar = nullptr;
bool deviceConnected = false;
bool oldConnected    = false;
unsigned long lastBLESend = 0;

float phoneLat = 0, phoneLng = 0, phoneSpeed = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s)    override { deviceConnected = true;  }
  void onDisconnect(BLEServer *s) override { deviceConnected = false; }
};

class LocationCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String val = c->getValue().c_str();
    int c1 = val.indexOf(',');
    int c2 = val.lastIndexOf(',');
    if (c1 > 0 && c2 > c1) {
      phoneLat   = val.substring(0, c1).toFloat();
      phoneLng   = val.substring(c1+1, c2).toFloat();
      phoneSpeed = val.substring(c2+1).toFloat();
    }
  }
};

// ─── ECG strip ────────────────────────────────────────────────────────────────
const int ECG_X0  = 14, ECG_X1 = 226;
const int ECG_Y0  = 120, ECG_H  = 22;
const int ECG_CY  = ECG_Y0 + ECG_H / 2;
const int ECG_AMP = 9;
int ecgX = 0;

// ─── Heart animation ──────────────────────────────────────────────────────────
bool heartBig = false;
unsigned long lastHeartPulse = 0;

// ─── Misc timing ──────────────────────────────────────────────────────────────
unsigned long lastDisplayUpdate = 0;

// ─────────────────────────────────────────────────────────────────────────────
// HRV — SDNN from RR intervals
// ─────────────────────────────────────────────────────────────────────────────
float calcSDNN() {
  int count = 0;
  for (int i = 0; i < RATE_SIZE; i++) if (rrIntervals[i] > 0) count++;
  if (count < 2) return 0;
  float mean = 0;
  for (int i = 0; i < RATE_SIZE; i++) mean += rrIntervals[i];
  mean /= count;
  float var = 0;
  for (int i = 0; i < count; i++) {
    float d = rrIntervals[i] - mean;
    var += d * d;
  }
  return sqrt(var / count);
}

// ─────────────────────────────────────────────────────────────────────────────
// Drowsiness score
// ─────────────────────────────────────────────────────────────────────────────
void computeDrowsiness() {
  if (!calibDone) { drowsinessScore = 0; return; }
  float s_hrv  = (baselineSDNN > 0 && sdnn < baselineSDNN)
                 ? ((baselineSDNN - sdnn) / baselineSDNN) * 50.0f : 0;
  float s_tilt = constrain((fabsf(tiltAngle - baselineTilt) / 15.0f) * 30.0f, 0, 30);
  float s_bpm  = (bpm > 0 && bpm < 65)
                 ? constrain(((65 - bpm) / 20.0f) * 20.0f, 0, 20) : 0;
  drowsinessScore = constrain(s_hrv + s_tilt + s_bpm, 0, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────
void drawHeart(int cx, int cy, int s, uint16_t col) {
  gfx->fillCircle(cx-s/2, cy-s/4, s/2, col);
  gfx->fillCircle(cx+s/2, cy-s/4, s/2, col);
  gfx->fillRect(cx-s+1, cy-s/4, (s-1)*2, s/2, col);
  gfx->fillTriangle(cx-s,cy+s/4, cx,cy+s+s/2, cx,cy+s/4, col);
  gfx->fillTriangle(cx+s,cy+s/4, cx,cy+s+s/2, cx,cy+s/4, col);
}

void drawStaticFrame() {
  gfx->setTextSize(1);
  gfx->setTextColor(GRAY);
  gfx->setCursor(62, 8);
  gfx->print("HEART  RATE");

  gfx->fillRoundRect(ECG_X0, ECG_Y0, ECG_X1-ECG_X0, ECG_H, 3, 0x02A0);

  gfx->fillRoundRect(8, 148, 106, 20, 4, 0x1001);
  gfx->setTextColor(PURPLE);
  gfx->setCursor(14, 153); gfx->print("HRV:");

  gfx->fillRoundRect(120, 148, 112, 20, 4, 0x2000);
  gfx->setTextColor(ORANGE);
  gfx->setCursor(126, 153); gfx->print("DROWSY:");

  gfx->fillRoundRect(8, 172, 110, 52, 4, 0x0010);
  gfx->setTextColor(CYAN);
  gfx->setCursor(46, 175); gfx->print("GPS");
  gfx->setTextColor(GRAY);
  gfx->setCursor(14, 190); gfx->print("LAT");
  gfx->setCursor(14, 202); gfx->print("LNG");
  gfx->setCursor(14, 214); gfx->print("SPD");

  gfx->fillRoundRect(122, 172, 110, 52, 4, 0x2000);
  gfx->setTextColor(AMBER);
  gfx->setCursor(152, 175); gfx->print("SPEED");
  gfx->setCursor(158, 214); gfx->print("KM/H");

  // BLE dot
  gfx->fillCircle(CX, 234, 4, DARKGRAY);
}

void drawBPM(int v) {
  gfx->fillRect(CX-40, 22, 80, 34, BLACK);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", v > 0 ? v : 0);
  int w = strlen(buf) * 6 * 4;
  gfx->setCursor(CX - w/2, 22);
  gfx->setTextColor(v > 0 ? WHITE : GRAY);
  gfx->setTextSize(4);
  gfx->print(buf);
}

void drawHeartAnim() {
  gfx->fillRect(CX-36, 58, 76, 20, BLACK);
  gfx->setTextColor(CRIMSON);
  gfx->setTextSize(1);
  gfx->setCursor(CX-34, 64);
  gfx->print("BPM");
  drawHeart(CX+20, 64, heartBig ? 9 : 6, CRIMSON);
}

void drawECGSample(long irRaw) {
  int screenX = ECG_X0 + ecgX;
  gfx->drawFastVLine(screenX, ECG_Y0, ECG_H, 0x02A0);
  int ahead = (screenX+3 < ECG_X1) ? screenX+3 : ECG_X0;
  gfx->drawFastVLine(ahead, ECG_Y0, ECG_H, DARKGRAY);

  long irClamped = constrain(irRaw, 50000, 150000);
  int y = constrain(ECG_CY - map(irClamped, 50000, 150000, -ECG_AMP, ECG_AMP),
                    ECG_Y0, ECG_Y0 + ECG_H - 1);
  gfx->drawFastVLine(screenX, y-1, 3, fingerOn ? GREEN : DARKGRAY);
  if (++ecgX >= ECG_X1 - ECG_X0) ecgX = 0;
}

void drawHRV(float v) {
  gfx->fillRect(50, 150, 60, 14, 0x1001);
  char buf[10];
  snprintf(buf, sizeof(buf), "%.0fms", v);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(52, 153);
  gfx->print(v > 0 ? buf : "--ms");
}

void drawDrowsy(float score) {
  gfx->fillRect(178, 150, 50, 14, 0x2000);
  char buf[8];
  snprintf(buf, sizeof(buf), "%.0f%%", score);
  uint16_t col = score > 70 ? CRIMSON : (score > 40 ? AMBER : GREEN);
  gfx->setTextColor(col);
  gfx->setTextSize(1);
  gfx->setCursor(180, 153);
  gfx->print(calibDone ? buf : "CAL..");
}

void drawGPS(float lat, float lng, float spd) {
  gfx->fillRect(40, 189, 74, 30, 0x0010);
  gfx->setTextSize(1);
  if (lat != 0 && lng != 0) {
    char la[12], ln[12];
    dtostrf(lat, 7, 4, la);
    dtostrf(lng, 7, 4, ln);
    gfx->setTextColor(WHITE);
    gfx->setCursor(40, 190); gfx->print(la);
    gfx->setCursor(40, 202); gfx->print(ln);
  } else {
    gfx->setTextColor(GRAY);
    gfx->setCursor(40, 190); gfx->print("no phone");
    gfx->setCursor(40, 202); gfx->print("GPS yet");
  }
  gfx->fillRect(40, 213, 74, 8, 0x0010);
  gfx->setTextColor(AMBER);
  gfx->setCursor(40, 214);
  char sb[8]; snprintf(sb, sizeof(sb), "%.1f", spd);
  gfx->print(sb);
}

void drawSpeed(float spd) {
  gfx->fillRect(130, 190, 98, 22, 0x2000);
  char buf[6];
  snprintf(buf, sizeof(buf), "%.0f", spd);
  int sw = strlen(buf) * 6 * 2;
  gfx->setCursor(177 - sw/2, 192);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->print(buf);
}

void drawCalibBar(unsigned long now) {
  gfx->fillRoundRect(30, 229, 178, 10, 2, 0x1082);
  if (calibDone) {
    gfx->setTextColor(GREEN); gfx->setTextSize(1);
    gfx->setCursor(52, 230); gfx->print("CALIBRATED");
    return;
  }
  unsigned long elapsed = now - calibStart;
  int pct = constrain((int)((elapsed * 100UL) / CALIB_MS), 0, 100);
  if (pct > 0) gfx->fillRect(30, 229, pct * 178 / 100, 10, AMBER);
  char buf[20];
  snprintf(buf, sizeof(buf), "CAL %d%%", pct);
  gfx->setTextColor(BLACK); gfx->setTextSize(1);
  gfx->setCursor(34, 230); gfx->print(buf);
}

void drawBLEStatus(bool connected) {
  gfx->fillCircle(CX, 234, 4, connected ? GREEN : DARKGRAY);
}

void drawFingerStatus() {
  gfx->fillRect(CX-28, 78, 56, 8, BLACK);
  gfx->setTextSize(1);
  if (!fingerOn) {
    gfx->setTextColor(GRAY);
    gfx->setCursor(CX-28, 78);
    gfx->print("no finger");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Display
  gfx->begin();
  gfx->fillScreen(BLACK);
  delay(100);
  drawStaticFrame();
  drawBPM(0);
  drawHeartAnim();
  drawHRV(0);
  drawDrowsy(0);
  drawGPS(0, 0, 0);
  drawSpeed(0);

  // I2C — both sensors share same bus
  Wire.begin(SDA_PIN, SCL_PIN);

  // MPU6050
  mpuInit();
  Serial.println("MPU6050 OK");

  // MAX30102
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found!");
  } else {
    Serial.println("MAX30102 OK");
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
    particleSensor.setPulseAmplitudeGreen(0);
  }

  // BLE
  BLEDevice::init("SmartWatch");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pSensorChar = pService->createCharacteristic(
    CHAR_SENSOR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pSensorChar->addDescriptor(new BLE2902());

  pLocationChar = pService->createCharacteristic(
    CHAR_LOCATION_UUID, BLECharacteristic::PROPERTY_WRITE);
  pLocationChar->setCallbacks(new LocationCallbacks());

  pService->start();
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising as 'SmartWatch'");

  calibStart = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── BLE reconnect ─────────────────────────────────────────────────────────
  if (!deviceConnected && oldConnected) {
    delay(300);
    pServer->startAdvertising();
    oldConnected = false;
    drawBLEStatus(false);
  }
  if (deviceConnected && !oldConnected) {
    oldConnected = true;
    drawBLEStatus(true);
  }

  // ── MPU6050 @ 25Hz ────────────────────────────────────────────────────────
  if (now - lastImuUpdate >= 40) {
    lastImuUpdate = now;
    float ax, ay, az;
    if (mpuRead(ax, ay, az)) {
      float mag = sqrt(ax*ax + ay*ay + az*az);
      if (mag > 0) {
        tiltAngle = degrees(acos(constrain(az/mag, -1.0f, 1.0f)));
        accelMag  = fabsf(mag - 1.0f) * 9.81f;
      }
    }
  }

  // ── MAX30102 — read IR and detect beats ───────────────────────────────────
  long irValue = particleSensor.getIR();
  fingerOn = (irValue > 50000);

  if (fingerOn) {
    if (checkForBeat(irValue)) {
      long delta = now - lastBeat;
      lastBeat = now;

      if (delta > 400 && delta < 1500) {
        rrIntervals[rateSpot] = delta;
        rateSpot = (rateSpot + 1) % RATE_SIZE;

        float sum = 0; int cnt = 0;
        for (int i = 0; i < RATE_SIZE; i++) {
          if (rrIntervals[i] > 0) { sum += rrIntervals[i]; cnt++; }
        }
        if (cnt > 0) bpm = (int)(60000.0f / (sum / cnt));

        sdnn = calcSDNN();

        if (!calibDone && sdnn > 0) {
          calibSDNNSum += sdnn;
          calibTiltSum += tiltAngle;
          calibSamples++;
        }

        heartBig = true;
        lastHeartPulse = now;
      }
    }
  } else {
    bpm = 0;
    memset(rrIntervals, 0, sizeof(rrIntervals));
    rateSpot = 0;
    sdnn = 0;
  }

  // ── Calibration completion ────────────────────────────────────────────────
  if (!calibDone && now - calibStart >= CALIB_MS) {
    calibDone = true;
    if (calibSamples > 0) {
      baselineSDNN = calibSDNNSum / calibSamples;
      baselineTilt = calibTiltSum / calibSamples;
    }
    Serial.printf("Calibrated: SDNN=%.1f Tilt=%.1f\n", baselineSDNN, baselineTilt);
  }

  // ── Drowsiness @ 500ms ────────────────────────────────────────────────────
  if (now - lastDrowsyUpdate >= 500) {
    lastDrowsyUpdate = now;
    computeDrowsiness();
  }

  // ── Heart animation timing ────────────────────────────────────────────────
  if (bpm > 0 && now - lastHeartPulse >= (unsigned long)(60000 / bpm)) {
    lastHeartPulse = now;
    heartBig = !heartBig;
  }

  // ── BLE send @ 500ms ──────────────────────────────────────────────────────
  if (now - lastBLESend >= 500 && deviceConnected) {
    lastBLESend = now;
    char buf[100];
    snprintf(buf, sizeof(buf),
      "{\"b\":%d,\"h\":%.1f,\"d\":%.1f,\"t\":%.1f,\"c\":%d,\"f\":%d}",
      bpm, sdnn, drowsinessScore, tiltAngle, calibDone ? 1 : 0, fingerOn ? 1 : 0);
    pSensorChar->setValue(buf);
    pSensorChar->notify();
  }

  // ── Display update @ 500ms ────────────────────────────────────────────────
  if (now - lastDisplayUpdate >= 500) {
    lastDisplayUpdate = now;
    drawBPM(bpm);
    drawHRV(sdnn);
    drawDrowsy(drowsinessScore);
    drawGPS(phoneLat, phoneLng, phoneSpeed);
    drawSpeed(phoneSpeed);
    drawCalibBar(now);
    drawFingerStatus();
  }

  drawECGSample(irValue);
  drawHeartAnim();

  delay(18);
}
