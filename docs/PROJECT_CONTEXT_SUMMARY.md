# SmartWatch Project — Context Summary

Paste this as your first message in the new account to bring Claude up to speed.

## Project: ESP32-C3 Smartwatch (Heart Rate + HRV + Drowsiness + GPS)

### Hardware
- ESP32-C3 Super Mini (MCU)
- GC9A01 240×240 round SPI display — DC=GPIO1, CS=GPIO5, SCK=GPIO2, MOSI=GPIO4, RST=GPIO0, BLK→3.3V
- MAX30102 (heart rate/HRV) — I2C, SDA=GPIO6, SCL=GPIO7, addr 0x57
- MPU6050 (accel/gyro, tilt) — I2C same bus, addr 0x68

### Firmware stack
Arduino IDE, C++, Arduino_GFX_Library, SparkFun MAX3010x library,
raw I2C for MPU6050, ESP32 BLE Arduino (GATT server)

### Key algorithms implemented
- Median filter + IIR low-pass on heart signal
- Adaptive threshold beat detection (envelope tracking, rising-edge only)
- SDNN (HRV) from last 8 RR intervals
- Tilt angle via arccos(az/|a|) from accelerometer
- Weighted drowsiness score: HRV drop (50pts) + tilt deviation (30pts) + low BPM (20pts) = 0–100 score
- 3-minute calibration phase to set personal baseline SDNN/tilt

### Design decision
GPS, vibration, and sound alerts moved OFF the watch — phone handles these
via BLE instead (watch just streams sensor data, phone does alerts + GPS).

### Mobile app
Android Studio, Kotlin, BLE GATT client, Google Play Services
FusedLocationProviderClient for GPS, min SDK 26. App shows BPM/HRV/drowsiness/
tilt/GPS/speed dashboard and triggers vibrate+alarm when drowsiness >= 70%.

### BLE contract
```
Service UUID:       12345678-1234-1234-1234-123456789abc
Sensor Char UUID:   12345678-1234-1234-1234-123456789abd  (Notify, watch->phone)
Location Char UUID: 12345678-1234-1234-1234-123456789abe  (Write, phone->watch)
```

JSON payload sent from watch to phone every 500ms:
```json
{"b":72,"h":42.1,"d":35.0,"t":8.2,"c":1,"f":1}
```
- b = BPM
- h = HRV (SDNN in ms)
- d = drowsiness score (0-100)
- t = tilt angle (degrees)
- c = calibrated (0/1)
- f = finger detected (0/1)

### Deliverables already created (included in this export)
- `firmware/smartwatch.ino` — full working ESP32-C3 sketch
- `firmware/WIRING.md` — pin reference and required libraries
- `android_app/` — full Android Studio Kotlin project (MainActivity.kt,
  layouts, manifest, gradle files)
- `docs/HRV_Tilt_Drowsiness_Reference.pdf` — technical reference covering
  HRV formulas, tilt formula, drowsiness algorithm, and margin of error

### Current status
Full firmware sketch and full Android Kotlin app both written and working.
Last debugging issue was a blank display (fixed — was too-dark background
colors + wrong pins for wrong board assumption; corrected once confirmed
board is ESP32-C3 Super Mini, not a Waveshare board). Both MAX30102 and
MPU6050 sensors confirmed working via I2C scanner test.
