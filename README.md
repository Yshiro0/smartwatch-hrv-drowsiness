# ESP32-C3 SmartWatch — HR / HRV / Drowsiness Monitor

A wrist-worn drowsiness-detection smartwatch built on the ESP32-C3, paired over BLE with a companion Android app that handles GPS tracking and alerts. Designed as a low-cost fatigue monitor for drivers, students, and shift workers.

![status](https://img.shields.io/badge/status-working-brightgreen) ![platform](https://img.shields.io/badge/firmware-Arduino%20%2F%20ESP32--C3-blue) ![app](https://img.shields.io/badge/app-Kotlin%20%2F%20Android-purple)

---

## Overview

The watch continuously reads heart rate, heart-rate variability (HRV), and wrist tilt, then computes a live **0–100 drowsiness score** on-device. Sensor data streams over Bluetooth Low Energy to a phone app every 500 ms; the phone owns GPS/speed and fires a vibration + alarm whenever the score crosses a drowsy threshold.

```
┌─────────────────────────┐        BLE (GATT)        ┌──────────────────────────┐
│   ESP32-C3 Watch         │ ───── sensor JSON ────►  │   Android App             │
│                          │        every 500ms        │                          │
│ • MAX30102 (HR/HRV)      │ ◄──── lat,lng,speed ────  │ • FusedLocationProvider   │
│ • MPU6050 (tilt)         │                            │ • Vibration + alarm       │
│ • GC9A01 round display   │                            │ • Live dashboard UI       │
└─────────────────────────┘                            └──────────────────────────┘
```

**Design decision:** GPS, vibration, and sound alerts live on the *phone*, not the watch. The watch's only job is to sense and stream — this keeps the firmware simple and avoids needing a GPS module or buzzer on the wrist unit.

---

## Features

- Real-time BPM display with animated heartbeat icon and live ECG/PPG-style waveform strip
- HRV (SDNN) computed from the last 8 RR intervals
- Wrist tilt angle from accelerometer, used as a head-drop / posture-change proxy
- Weighted, multi-signal drowsiness score (0–100) with a 3-minute personal calibration phase
- BLE GATT server on the watch — auto re-advertises on disconnect
- Android dashboard: BPM, HRV (with qualitative label), drowsiness %, tilt, GPS lat/lng, speed, calibration progress, alert history
- Drowsiness alert: vibration pattern + alarm tone + toast, with a 15-second cooldown to avoid alert spam
- Finger-presence detection to avoid false readings when the watch isn't worn

---

## Hardware

| Component | Part | Interface |
|---|---|---|
| MCU | ESP32-C3 Super Mini | — |
| Display | GC9A01, 240×240 round SPI | SPI |
| Heart rate / SpO₂ sensor | MAX30102 | I2C (0x57) |
| IMU (tilt) | MPU6050 | I2C (0x68) |

### Wiring

**Display (GC9A01):**

| Display Pin | ESP32-C3 GPIO |
|---|---|
| SCK | GPIO 2 |
| MOSI (SDA) | GPIO 4 |
| CS | GPIO 5 |
| DC | GPIO 1 |
| RST | GPIO 0 |
| BLK | 3.3V (**must be tied high — screen stays blank otherwise**) |

**I2C bus (shared by MAX30102 + MPU6050):**

| Signal | ESP32-C3 GPIO |
|---|---|
| SDA | GPIO 6 |
| SCL | GPIO 7 |

MPU6050's `AD0` pin is tied to GND (address `0x68`). GPIO8/GPIO9 are avoided for I2C since they're strapping pins that can prevent boot if pulled low at startup.

Run the I2C scanner in `firmware/WIRING.md` first — you should see both `0x57` and `0x68` before flashing the main sketch.

---

## Firmware

**Stack:** Arduino IDE (C++) · [Arduino_GFX_Library](https://github.com/moononournation/Arduino_GFX) · SparkFun MAX3010x library · raw I2C driver for MPU6050 · ESP32 BLE Arduino (GATT server)

**Board:** `ESP32C3 Dev Module` or `Nologo ESP32C3 Super Mini`

### Signal processing

- **Beat detection:** adaptive-threshold, rising-edge-only detection on the MAX30102 IR channel (SparkFun `checkForBeat`), gated to accept RR intervals between 400–1500 ms to reject noise
- **HRV (SDNN):** standard deviation of the last 8 RR intervals — the classic time-domain HRV metric
- **Tilt angle:** `θ = arccos(az / |a|)`, where `|a| = sqrt(ax² + ay² + az²)` — the angle between the wrist's Z-axis and vertical
- **Drowsiness score (0–100):** a weighted sum of three components, recomputed every 500 ms once calibration is done:
  - up to **50 pts** for HRV dropping below the personal baseline
  - up to **30 pts** for tilt deviating from the baseline posture (scaled over a 15° range)
  - up to **20 pts** for BPM dropping below 65 (scaled over a 20 bpm–65 bpm range)
- **Calibration:** a 3-minute warm-up on boot averages SDNN and tilt while the wearer is alert and seated, establishing `baselineSDNN` and `baselineTilt` for the scoring formulas above

A full derivation of these formulas, plus expected margin-of-error tables, is in [`docs/HRV_Tilt_Drowsiness_Reference.pdf`](docs/HRV_Tilt_Drowsiness_Reference.pdf).

### Required libraries (Arduino Library Manager)

- `Arduino_GFX_Library` (moononournation)
- `SparkFun MAX3010x Pulse and Proximity Sensor Library`
- `ESP32 BLE Arduino` (bundled with the ESP32 board package)

### Uploading

If the COM port doesn't show up or upload fails: hold **BOOT**, tap **RST**, release **BOOT**, then click Upload.

---

## BLE protocol

The watch advertises as `SmartWatch` and runs a single GATT service:

```
Service UUID:       12345678-1234-1234-1234-123456789abc
Sensor Char UUID:   12345678-1234-1234-1234-123456789abd   Notify · watch → phone
Location Char UUID: 12345678-1234-1234-1234-123456789abe   Write  · phone → watch
```

**Watch → phone** (JSON, sent every 500 ms):

```json
{"b":72,"h":42.1,"d":35.0,"t":8.2,"c":1,"f":1}
```

| Field | Meaning | Type |
|---|---|---|
| `b` | Heart rate (BPM) | int |
| `h` | HRV / SDNN (ms) | float |
| `d` | Drowsiness score (0–100) | float |
| `t` | Tilt angle (degrees) | float |
| `c` | Calibration complete (0/1) | int |
| `f` | Finger detected on sensor (0/1) | int |

**Phone → watch** (plain CSV string, sent on every location update):

```
<lat>,<lng>,<speed_kmh>
```

e.g. `14.083200,121.145600,42.3` — parsed on the watch by splitting on the first and last comma.

---

## Android app

**Stack:** Android Studio, Kotlin, BLE GATT client, Google Play Services `FusedLocationProviderClient`, `minSdk 26` / `targetSdk 34`

**What it does:**
- Scans for and connects to the `SmartWatch` BLE device, subscribes to sensor notifications
- Renders BPM, HRV (with a qualitative label — Very Low / Low / Moderate / Normal / Excellent), drowsiness % with color-coded state (AWAKE / ALERT / DROWSY), tilt, and calibration progress
- Streams GPS lat/lng/speed via `FusedLocationProviderClient` (1s interval, high accuracy) and writes it back to the watch's Location characteristic
- Fires a **vibration pattern + alarm ringtone + toast** when drowsiness score ≥ 70% *and* the finger sensor confirms the watch is being worn, with a 15-second cooldown between alerts
- Tracks alert count and last-alert timestamp for the session

**Permissions required:** `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT` (Android 12+), `ACCESS_FINE_LOCATION`, `ACCESS_COARSE_LOCATION`, `VIBRATE`.

---

## 3D-printed case & strap

<img src="3d-models/renders/case_iso.png" width="360"/> <img src="3d-models/renders/wristband_4links.png" width="360"/>

A printable enclosure and modular link wristband are included in [`3d-models/`](3d-models/), along with preview renders and approximate dimensions for each part. See [`3d-models/README.md`](3d-models/README.md) for details.

---

## Getting started

1. Wire up the hardware per the [Hardware](#hardware) section above.
2. Open `firmware/smartwatch.ino` in Arduino IDE, install the required libraries, select the ESP32-C3 board, and flash.
3. Confirm over Serial Monitor (115200 baud) that both `MAX30102 OK` and `MPU6050 OK` print, and that BLE begins advertising as `SmartWatch`.
4. Open `android_app/` in Android Studio, build, and install on a phone running Android 8.0 (API 26) or later.
5. Launch the app, grant Bluetooth + Location permissions, and tap **SCAN** to connect.
6. Wear the watch and hold still for the 3-minute calibration countdown shown on both the watch display and the app before relying on the drowsiness score.

---

## Repository structure

```
.
├── firmware/
│   ├── smartwatch.ino       # Full ESP32-C3 sketch — sensors, display, BLE, algorithms
│   └── WIRING.md            # Pin reference, library list, I2C scanner sketch
├── android_app/
│   └── app/
│       ├── src/main/java/.../MainActivity.kt   # BLE client, GPS, alerts, dashboard logic
│       ├── src/main/res/                        # Layouts, colors, drawables
│       └── build.gradle
├── docs/
│   └── HRV_Tilt_Drowsiness_Reference.pdf   # Formulas, algorithm derivation, error margins
└── 3d-models/
    ├── watchcase_new.3mf                              # Printable case assembly
    ├── prizma_wristband_144mm_plus4links.stl           # Wristband, larger size
    ├── prizma_wristband_144mm_plus2links.stl           # Wristband, smaller size
    └── renders/                                        # Preview images of the above
```

---

## Known limitations / roadmap

- Drowsiness weighting (50/30/20) was chosen heuristically, not validated against a labeled dataset — treat scores as a relative fatigue indicator, not a clinical measurement
- No on-watch alerting means the wearer relies entirely on the phone being nearby and connected
- Single BLE connection only — no reconnection to a specific phone if multiple are in range
- SDNN over only 8 RR intervals is a short window; longer buffers would give a more stable HRV estimate at the cost of slower response to changes

## License

Add a license of your choice here (MIT is a common default for hobby hardware/firmware projects like this).
