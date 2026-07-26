# Wiring Reference — ESP32-C3 Super Mini Smartwatch

## Display (GC9A01 240x240 round SPI)

| Display Pin | ESP32-C3 GPIO |
|---|---|
| SCL (CLK) | GPIO 2 |
| SDA (MOSI) | GPIO 4 |
| CS | GPIO 5 |
| DC | GPIO 1 |
| RST | GPIO 0 |
| VCC | 3.3V |
| GND | GND |
| BLK | 3.3V (backlight always on — REQUIRED or screen stays blank) |

## I2C Bus (shared by MAX30102 + MPU6050)

| Sensor Pin | ESP32-C3 GPIO |
|---|---|
| SDA | GPIO 6 |
| SCL | GPIO 7 |

Avoid GPIO8/GPIO9 for I2C — they are strapping pins and can prevent boot if pulled low during startup.

### MAX30102 (Heart Rate / HRV)
- VIN → 3.3V
- GND → GND
- SDA → GPIO 6
- SCL → GPIO 7
- I2C Address: 0x57

### MPU6050 (Accelerometer / Gyroscope / Tilt)
- VCC → 3.3V (NOT 5V)
- GND → GND
- SDA → GPIO 6 (same wire as MAX30102)
- SCL → GPIO 7 (same wire as MAX30102)
- AD0 → GND (sets address to 0x68)
- I2C Address: 0x68

## Required Libraries (Arduino IDE Library Manager)
- Arduino_GFX_Library (moononournation)
- SparkFun MAX3010x Pulse and Proximity Sensor Library
- ESP32 BLE Arduino (bundled with ESP32 board package)

## Board Selection
- Board: "ESP32C3 Dev Module" or "Nologo ESP32C3 Super Mini"

## Uploading (if COM port errors occur)
1. Hold BOOT button
2. Press and release RST
3. Release BOOT
4. Click Upload

## I2C Scanner (run first to verify wiring)
```cpp
#include <Wire.h>
void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(6, 7);
  Serial.println("Scanning I2C...");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("Found device at 0x");
      Serial.println(addr, HEX);
    }
  }
}
void loop() {}
```
Expect to see 0x57 (MAX30102) and 0x68 (MPU6050).
