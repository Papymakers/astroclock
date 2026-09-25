# AstroClock

*[Version française](README.md)*

Connected wall clocks on the **ESP32-2432S028** board ("Cheap Yellow Display", 2.8" 320×240), fed over MQTT.

Each display shows:

- the time (HH:MM, large digits);
- the date (weekday, day, month, in French);
- the outdoor temperature, colour-coded by range;
- a WiFi symbol (network state) and its name "Client N" (MQTT state).

Time and date are broadcast by a Python script (`server/astro_clock.py`). When Internet access is lost, a backup RTC module (`firmware/RtcBackup`) supplies the time. The temperature comes from an Ecowitt weather station, through an MQTT feed produced upstream (outside this repository).

## Architecture

```
                  ┌──────────────────────┐
  Internet ──────►│  server/astro_clock  │── astroClock (60 s) ──┐
  (system time)   │  (Python, Flask)     │── astroClock/moon ────┤
                  │                      │                       │      ┌──────────────┐
  RTC module ◄────│── utcClock ──────────│                       ├─────►│   Broker     │
  (backup)   ────►│── rtcClock ──────────│                       │      │  Mosquitto   │
                  └──────────────────────┘                       │      │              │
                                                                 │      └──────┬───────┘
  Ecowitt station ── (upstream chain) ── ecowittDatas ───────────┘             │
                                                                               ▼
                                                       Client 6, Client 7, … (ESP32-2432S028)
```

## Repository layout

```
astroclock/
├── firmware/AstroClock/
│   ├── AstroClock.ino          template sketch shared by all clocks
│   └── secrets.h.example       WiFi credentials template
├── firmware/RtcBackup/
│   ├── RtcBackup.ino           backup RTC module (ESP32-C3 + DS3231 + OLED)
│   └── secrets.h.example
├── server/
│   ├── astro_clock.py          time/astro broadcaster and web page
│   └── requirements.txt
├── README.md
└── README.en.md
```

## MQTT contract

This is what must stay consistent if you change either side, or plug in another data source.

Each clock subscribes to **two topics**:

```cpp
mqttClient.subscribe("ecowittDatas");
mqttClient.subscribe("astroClock/#");
```

### `astroClock` — time and date

Published by `server/astro_clock.py` every 60 s, with `retain=True`, so a clock that boots receives the last message immediately.

The script publishes a full JSON object, but **the clock only uses these fields**:

| Key       | Type    | Use by the clock                                   |
|-----------|---------|----------------------------------------------------|
| `hours`   | integer | hour, 0–23 — resynchronises the local clock        |
| `minutes` | integer | minutes, 0–59                                      |
| `day`     | integer | day of month, 1–31                                 |
| `month`   | integer | month, 1–12                                        |
| `weekday` | integer | day of week, **1 = Monday … 7 = Sunday** (ISO)     |

`sunriseHour` and `sunriseMin` are parsed but not displayed. The other fields (`sunsetHour`, `sunsetMin`, `daylightDeltaStr`, `daylightChangeSinceSolsticeStr`, `moonphase`) are ignored by the clocks; they feed the server's web page.

Example message:

```json
{"hours": 14, "minutes": 32, "day": 24, "month": 9, "weekday": 4,
 "sunriseHour": 7, "sunriseMin": 49, "sunsetHour": 19, "sunsetMin": 45,
 "daylightDeltaStr": "-3mn", "daylightChangeSinceSolsticeStr": "-4h07mn",
 "moonphase": 12.34}
```

Out-of-range values are rejected: an invalid time does not resynchronise the clock, and an invalid date is not displayed.

### `astroClock/moon` — moon phase

Published hourly (`retain=True`). The clocks receive it because of the `#` wildcard, but **ignore it**.

### `ecowittDatas` — outdoor temperature

Produced upstream from the Ecowitt station data (outside this repository). The message carries many readings, but **the clock only reads the `tempExt` key**:

| Key       | Type  | Use by the clock              |
|-----------|-------|-------------------------------|
| `tempExt` | float | outdoor temperature in °C     |

Any other key is ignored. If `tempExt` is missing, the displayed temperature is left unchanged.

### Topics not used by the clocks

| Topic      | Direction              | Purpose |
|------------|------------------------|---------|
| `utcClock` | server → RTC module    | reference time as JSON (`hours`, `minutes`, `seconds`, `day`, `month`, `year`). Published every minute **only when Internet is up**, **without** `retain` |
| `rtcClock` | RTC module → server    | DS3231 time as text `HH:MM:SS DD/MM/YYYY`, every 5 s |

### Message size

The `astroClock` message is about 230 bytes of JSON, close to 245 bytes with the MQTT header. The PubSubClient library **silently drops** any message larger than its buffer, which is 256 bytes by default. The firmware therefore raises the buffer to 512 bytes (`mqttClient.setBufferSize(512)`). The JSON document (ArduinoJson 7) adapts automatically to the message size.

If you add fields to `astroClock`, or if `ecowittDatas` grows beyond ~500 bytes, increase the buffer size.

## Hardware

- **Board**: ESP32-2432S028 (2.8" 320×240 TFT, ILI9341 controller on most versions)
- **Pins used by the firmware**:

| Pin | Purpose |
|-----|---------|
| GPIO 22 | PWM backlight (`BACKLIGHT_PIN`) — **requires the modification below** |
| GPIO 34 | light sensor (`LDR_PIN`) — reserved, unused |
| GPIO 26 | speaker (`SPEAKER_PIN`) — `beep()` available, not called |

### Backlight modification

Out of the box, the CYD drives the backlight transistor from **GPIO 21**. This pin serves two functions on the board and cannot be used to dim the screen.

To get day/night dimming:

1. **Cut the trace** connecting the backlight transistor gate to GPIO 21.
2. **Wire the gate to GPIO 22.**

The firmware then drives the backlight as PWM on GPIO 22 (5 kHz, 8-bit).

For full details and schematics, see the [CYD-Heating-Remote-2zones / hardware](https://github.com/Papymakers/CYD-Heating-Remote-2zones/tree/main/hardware) repository.

**Without this modification**, the clock works, but without dimming. In that case, let TFT_eSPI switch the backlight on permanently by defining in its configuration:

```cpp
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH
```

## Firmware

### Requirements

- **Arduino ESP32 core ≥ 3.0** (the code uses `ledcAttach()`, which does not exist in 2.x)
- **TFT_eSPI ≥ 2.5.0** (`drawArc()`, `fillSmoothCircle()`, `drawWideLine()`)
- **PubSubClient**
- **ArduinoJson 7.x**

### TFT_eSPI configuration

TFT_eSPI must be configured for the ESP32-2432S028. The following fonts must be loaded, as the firmware uses them:

```cpp
#define LOAD_GLCD   // font 1: "Client N"
#define LOAD_FONT4  // font 4: date and temperature
#define LOAD_FONT8  // font 8: time
```

With the backlight modification, the firmware drives it itself as PWM on `BACKLIGHT_PIN`: **do not define `TFT_BL`** in the TFT_eSPI configuration.

### Setting up a new display

1. Copy `firmware/AstroClock/secrets.h.example` to `secrets.h` and fill in the WiFi SSID and password.
2. In `AstroClock.ino`, change **only** this line:
   ```cpp
   #define CLIENT_NUM 7
   ```
   It sets both the displayed label (`Client 7`) and the identifier sent to the broker (`clock7`).
3. Check the broker address (`mqtt_server`), then flash.

> ⚠️ **Each display must have a different `CLIENT_NUM`.** Two MQTT clients with the same identifier keep kicking each other off the broker: the serial monitor shows "MQTT OK" over and over, but the time never arrives.

The serial monitor (115200 baud) prints the version at boot, then the WiFi and MQTT connection attempts.

## Display behaviour

### Indicators

| Element | Green | Red / crossed out |
|---------|-------|-------------------|
| WiFi symbol (top right) | WiFi connected | grey crossed in red: WiFi lost |
| "Client N" (bottom right) | MQTT connected **and** time received within the last 3 min | broker unreachable, or no `astroClock` message for 3 min |

At boot, "Client N" is red, then turns green on the first `astroClock` message. Time, date and temperature only appear once their first valid value has been received.

### Day and night

Between 8:00 and 22:00, the backlight is at 175/255 and texts are coloured. At night, the backlight drops to 50/255 and everything is shown in dark green. These values are set by the `BRIGHT_DAY`, `BRIGHT_NIGHT`, `DAY_START_HOUR` and `DAY_END_HOUR` constants.

Temperature colours (daytime):

| Range | Colour |
|-------|--------|
| < 0.1 °C | pale blue |
| 0.1 to 10 °C | dark blue |
| 10 to 20 °C | green |
| 20 to 30 °C | orange |
| ≥ 30 °C | red |

### During an outage

- **WiFi**: retried every 10 s. If WiFi has been gone for 5 min, the ESP32 reboots.
- **MQTT**: retried every 5 s, only while WiFi is up.
- **Time**: between two messages, the clock advances locally using `millis()`. The display keeps running during an outage and resynchronises on the first message received.

Network handling never blocks: the display stays up to date during outages.

## Server (`server/astro_clock.py`)

### Purpose

- publishes `astroClock` every minute (time, date, sunrise and sunset, day length);
- publishes `astroClock/moon` hourly;
- checks Internet access (connection to `8.8.8.8:53`):
  - when it is up, also publishes `utcClock` to resynchronise the backup RTC module;
  - when it is down, republishes each `rtcClock` message received as an `astroClock` message;
- serves a web page and two REST endpoints on port 5000:
  - `/` — clock/astro page (file `templates/index.html`);
  - `/astroclock` — astro data as JSON;
  - `/rtcclock` — local time as JSON.

### Configuration

At the top of the script: broker address and port (`MQTT_BROKER`, `MQTT_PORT`), topic names, and observation location (`LOCATION`: name, country, time zone, latitude, longitude) used to compute sunrise and sunset.

### Installation

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python astro_clock.py
```

## Backup RTC module (`firmware/RtcBackup`)

### Purpose

When Internet access is lost, the server can no longer guarantee that its system time is correct. The RTC module then takes over:

1. **Internet up**: the server publishes `utcClock` every minute. The module resynchronises its DS3231 if the drift exceeds 2 s.
2. **Internet down**: the server stops publishing `utcClock`. The module keeps publishing its DS3231 time on `rtcClock` every 5 s, and the server republishes it as `astroClock` for the clocks.

The module is not a master clock: the clocks never listen to it directly, everything goes through the server.

### Hardware

- ESP32-C3 DevKitM-1, in an RS PRO 105×90×65 DIN-rail enclosure
- DS3231 (address `0x68`) and SSD1306 128×64 OLED (address `0x3C`) on the same I2C bus: SDA = GPIO 1, SCL = GPIO 10. The OLED is mounted upside down in the enclosure (`setRotation(2)`)
- Button **GPIO 7**: hold 5 s → reboot (the LED blinks red while held; releasing before 5 s cancels)
- Button **GPIO 6**: wakes the OLED
- RGB LED on GPIO 8

### Screen

| Line | Content |
|------|---------|
| 1 | `RTC:OK` / `RTC:ERR` — DS3231 detected or not |
| 2 | `UTC:OK` / `UTC:ERR` — valid `utcClock` message received within the last 3 min |
| 3 | DS3231 time |
| 4 | DS3231 date |

The OLED switches off after 15 s without a press on the GPIO 6 button. In backup mode (Internet down), `UTC:ERR` is therefore the normal state.

### Libraries

Arduino ESP32 core ≥ 3.0 (`rgbLedWrite()`), Adafruit RTClib, Adafruit SSD1306, Adafruit GFX, PubSubClient, ArduinoJson 7.x.

### Setup

1. Copy `secrets.h.example` to `secrets.h` and fill in the WiFi credentials.
2. Flash. If the DS3231 lost power, it is set to the compile time, then resynchronised by `utcClock` within the first minute.

The MQTT identifier is `ESP32_RTC_Master`: no other device may use it.

### Migrating an existing installation

Older versions of the server published `utcClock` with `retain=True`. Clear the retained message from the broker once, otherwise the module would resynchronise to a stale time at boot:

```bash
mosquitto_pub -h 192.168.1.20 -t utcClock -r -n
```

## Known limitations

- **Stale retained message**: `astroClock` is published with `retain=True`. If the server script is stopped, a clock that reboots receives the last retained message and shows a wrong time. "Client N" turns red after 3 min, which flags the problem.
- **Date change during an outage**: the local clock advances the time, but not the date. The date updates on the first message received.
- **RTC backup mode**: without Internet, the astro fields (sunrise, sunset, moon phase) are published as zeros. The clocks are unaffected, since they only show time and date.
- **`utcClock` carries local time** (`Europe/Paris` time zone), not UTC, despite its name. The DS3231 therefore stores local time. If Internet stays down across a daylight-saving change, the module keeps the old time until Internet returns.
- **"Internet up" criterion**: the server tests access to `8.8.8.8:53`, not the NTP synchronisation of its own clock. On a machine without a hardware clock (a Raspberry Pi 4, for instance), checking NTP synchronisation would be safer.
