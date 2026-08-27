# ESP32 Nidec Wind Fan Controller

An ESP32-based controller for a **4-wire 12 V Nidec PWM fan** that automatically adjusts fan speed from Internet wind-speed data, while also providing manual control, RPM monitoring, an OLED status display, OTA firmware updates, and a fallback Wi-Fi captive portal.

The project also includes a small OLED missile-interception animation that can be triggered from the web interface and after a successful wind-data update.

## Features

- Automatic fan-speed control based on wind speed
- Open-Meteo weather data refreshed every 1 minute
- Linear wind-to-PWM mapping
- Manual fan control from 0% to 100%
- Smooth PWM ramping rather than abrupt speed changes
- 25 kHz, 10-bit PWM output
- Fan RPM monitoring using the tachometer output
- SH1106 128x64 OLED display
- OLED anti-burn-in movement: the complete information panel bounces around the display
- Web dashboard showing:
  - wind speed
  - actual and target PWM
  - fan RPM
  - Wi-Fi RSSI
  - connection status
  - age of the latest successful weather reading
- Manual/Wind control-mode switch
- HTTP/JSON status updates without full-page refresh
- ElegantOTA web firmware updates
- OTA username/password support
- Fallback Wi-Fi access point and captive portal
- Wi-Fi network scanning from the captive portal
- Save new Wi-Fi SSID/password to ESP32 NVS
- Save & Reboot workflow for changing Wi-Fi without recompiling


## Hardware

The current sketch is configured for an **ESP32 DevKit-style board**.

| Function | GPIO |
| --- | ---: |
| Fan PWM | 33 |
| Fan tachometer | 32 |
| OLED SDA | 25 |
| OLED SCL | 26 |

The fan itself is powered from its appropriate **12 V supply**. Do not power the fan from an ESP32 GPIO.

The PWM control used by this project is intended for a 4-wire fan control input. A transistor/open-collector style interface is recommended between the ESP32 PWM GPIO and the fan PWM input.

Make sure the ESP32 and fan-control circuit share a common ground.

## Fan Connections

Typical 4-wire fan functions are:

| Signal | Purpose |
| --- | --- |
| +12 V | Fan power |
| GND | Ground |
| Tach | RPM pulse output |
| PWM | Fan speed command |

Wire colors can vary between fan models, so verify the pinout for your particular fan before connecting it.

The tachometer input may require an appropriate pull-up resistor depending on the fan.

## Required Arduino Libraries

Install these libraries through the Arduino IDE Library Manager:

- **U8g2**
- **ArduinoJson**
- **ElegantOTA**

The sketch also uses ESP32 core libraries such as:

- `WiFi`
- `WebServer`
- `HTTPClient`
- `DNSServer`
- `Preferences`
- `Wire`

## Configuration

Most settings are near the beginning of the sketch.

### Default Wi-Fi

```cpp
const char* DEFAULT_WIFI_SSID     = "T9";
const char* DEFAULT_WIFI_PASSWORD = "";
```

These are only the factory/default credentials. Once credentials are saved through the captive portal, the saved NVS values override them.

For a public GitHub repository, avoid committing your real Wi-Fi password.

### Fallback Access Point

```cpp
const char* AP_SSID     = "Nidec-Fan-Setup";
const char* AP_PASSWORD = "fancontrol";
```

If normal Wi-Fi cannot be established, the ESP32 creates this access point.

### OTA Authentication

```cpp
const char* OTA_USERNAME = "admin";
const char* OTA_PASSWORD = "";
```

Set a strong OTA password before deploying the controller.

Do not publish a real OTA password in a public repository.

### Location

Set the latitude and longitude used for weather retrieval:

```cpp
const double LATITUDE  = 19.2183;
const double LONGITUDE = 72.9781;
```

Replace these with the coordinates of the location whose wind speed should control the fan.

### Weather Refresh

```cpp
const unsigned long WEATHER_INTERVAL_MS =
    1UL * 60UL * 1000UL;
```

The controller currently requests weather data every **1 minute**.

## Wind-to-Fan Mapping

The default automatic mapping is:

```cpp
const float WIND_MIN_KMH = 0.0;
const float WIND_MAX_KMH = 50.0;

const float WIND_MIN_PWM_PERCENT = 10.0f;
const float WIND_MAX_PWM_PERCENT = 50.0f;
```

This gives an approximately linear relationship:

| Wind | Fan PWM |
| ---: | ---: |
| 0 km/h | 10% |
| 10 km/h | 18% |
| 20 km/h | 26% |
| 30 km/h | 34% |
| 40 km/h | 42% |
| 50 km/h | 50% |

Wind values above the configured maximum remain capped at the configured maximum PWM.

Manual mode is independent and allows the slider to reach **0%**, permitting a 0% PWM command to be sent to the fan.

Whether a particular fan physically stops at 0% depends on the fan's internal controller.

## Smooth PWM Control

The sketch uses 10-bit PWM:

```cpp
#define PWM_FREQ       25000
#define PWM_RESOLUTION 10
#define PWM_MAX        1023
```

Instead of immediately jumping to a new requested speed, the output ramps toward the target:

```cpp
const float PWM_RAMP_STEP_PERCENT = 0.25f;
const unsigned long PWM_RAMP_INTERVAL_MS = 50UL;
```

This produces smoother fan-speed transitions.

## RPM Monitoring

The fan tachometer is connected to:

```cpp
#define FAN_TACH_PIN 32
```

The current configuration assumes:

```cpp
#define TACH_PULSES_PER_REV 2
```

If your fan produces a different number of tach pulses per revolution, change this value or the displayed RPM will be incorrect.

## OLED Display

The sketch is configured for a **128x64 SH1106 OLED** using U8g2.

```cpp
#define SDA_PIN 25
#define SCL_PIN 26
```

The normal display shows information such as:

```text
Wind
PWM
RPM
Date / Time
```

To reduce the chance of OLED burn-in, the complete information block moves together around the screen and bounces from the display boundaries.

Startup and network messages are also displayed on the OLED.
