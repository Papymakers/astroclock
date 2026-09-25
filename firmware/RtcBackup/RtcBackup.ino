// RTC_CLOCK_OLED — module RTC de secours pour AstroClock
// Carte : ESP32-C3 DevKitM-1 + DS3231 + OLED SSD1306 128x64
// Boîtier DIN RS PRO 105x90x65 (OLED monté inversé : setRotation(2))
//
// Rôle :
//  - publie l'heure du DS3231 sur "rtcClock" toutes les 5 s ;
//    le script serveur la republie en "astroClock" quand Internet est absent ;
//  - se recale sur "utcClock", publié par le serveur uniquement quand Internet est présent.

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "RTClib.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>       // ArduinoJson 7.x
#include "secrets.h"           // WIFI_SSID, WIFI_PASSWORD (voir secrets.h.example)

const char* version = "Last version 24/09/2026 - RTC_CLOCK_OLED_mqtt3";

// ---------- WiFi & MQTT ----------
const char* mqtt_server    = "192.168.1.20";
const uint16_t mqtt_port   = 1883;
const char* mqtt_client_id = "ESP32_RTC_Master";   // identifiant broker (unique !)
const char* TOPIC_RTC_OUT  = "rtcClock";
const char* TOPIC_TIME_IN  = "utcClock";

WiFiClient   espClient;
PubSubClient RtcClient(espClient);
RTC_DS3231   rtc;

// ---------- OLED ----------
constexpr uint8_t OLED_WIDTH  = 128;
constexpr uint8_t OLED_HEIGHT = 64;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);  // -1 : pas de pin reset

// ---------- TIMERS ----------
unsigned long previousMillis       = 0;
const unsigned long displayInterval = 1000;     // 1 s — rafraîchissement OLED
const unsigned long publishInterval = 5000;     // 5 s — publication rtcClock
unsigned long previousPublish      = 0;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 2000;   // 2 s entre tentatives MQTT
unsigned long lastUpdateMillis     = 0;         // dernier utcClock valide reçu
const unsigned long updateTimeout  = 180000;    // 3 min sans utcClock → utc_ok = false
const unsigned long displayTimeout = 15000;     // 15 s avant extinction OLED
unsigned long lastDisplayActivity  = 0;
unsigned long lastAttempt          = 0;
const unsigned long retryDelay     = 10000;     // 10 s entre tentatives WiFi
unsigned long wifiLostSince        = 0;
const unsigned long wifiRebootTimeout = 5UL * 60UL * 1000UL;  // reboot si WiFi absent 5 min
const long resyncThreshold         = 2;         // recale la RTC si écart >= 2 s

// ---------- HARDWARE ----------
constexpr uint8_t RGB_LED = 8;
constexpr uint8_t nSDA    = 1;    // I2C SDA — GPIO1
constexpr uint8_t nSCL    = 10;   // I2C SCL — GPIO10
                                  // OLED SSD1306 (0x3C) et DS3231 (0x68) sur ce bus

// ---------- FLAGS ----------
bool oled_ok     = false;
bool displayIsOn = true;
bool utc_ok      = false;
bool rtc_ok      = false;
bool WifiStatus  = false;

static bool          ledState  = false;
static unsigned long lastBlink = 0;

// ---------- BOUTONS ----------
// Champs modifiés dans l'interruption : volatile
struct Button {
  const uint8_t          PIN;
  volatile bool          pressed;
  volatile unsigned long lastPressTime;
  volatile unsigned long pressStartTime;
};

Button swReset   = {7, false, 0, 0};   // appui long 5 s → redémarrage
Button swDisplay = {6, false, 0, 0};   // réveil de l'OLED

void IRAM_ATTR handleButton(Button* btn) {
  unsigned long now = millis();
  if (now - btn->lastPressTime > 250) {
    btn->pressed        = true;
    btn->lastPressTime  = now;
    btn->pressStartTime = now;
  }
}
void IRAM_ATTR isrSwReset()   { handleButton(&swReset); }
void IRAM_ATTR isrSwDisplay() { handleButton(&swDisplay); }

// ---------- WiFi : jamais bloquant ----------
void handleWiFi() {
  WifiStatus = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  if (WifiStatus) {
    wifiLostSince = 0;
    return;
  }

  if (wifiLostSince == 0) wifiLostSince = now ? now : 1;

  if (now - lastAttempt >= retryDelay) {
    lastAttempt = now;
    Serial.println("[WiFi] perdu -> nouvelle tentative");
    WiFi.disconnect();                  // remet la pile STA dans un état propre
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // Le DS3231 garde l'heure pendant le redémarrage
  if (now - wifiLostSince > wifiRebootTimeout) {
    Serial.println("[WiFi] absent depuis 5 min -> redemarrage");
    delay(100);
    ESP.restart();
  }
}

// ---------- RTC ----------
bool validDateTime(int y, int mo, int d, int h, int mi, int s) {
  return y >= 2024 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
         h >= 0 && h < 24 && mi >= 0 && mi < 60 && s >= 0 && s < 60;
}

// ---------- MQTT callback ----------
// utcClock : {"hours":..,"minutes":..,"seconds":..,"day":..,"month":..,"year":..}
void callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_TIME_IN) != 0) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("⚠️ utcClock : JSON invalide (%s)\n", error.c_str());
    utc_ok = false;
    return;
  }

  int year   = doc["year"]    | 0;
  int month  = doc["month"]   | 0;
  int day    = doc["day"]     | 0;
  int hour   = doc["hours"]   | -1;
  int minute = doc["minutes"] | -1;
  int second = doc["seconds"] | -1;

  if (!validDateTime(year, month, day, hour, minute, second)) {
    Serial.println("⚠️ utcClock : valeurs absentes ou hors plage, ignore");
    return;
  }

  utc_ok = true;
  lastUpdateMillis = millis();

  if (!rtc_ok) return;

  DateTime ref(year, month, day, hour, minute, second);
  long diff = (long)ref.unixtime() - (long)rtc.now().unixtime();
  if (labs(diff) >= resyncThreshold) {
    rtc.adjust(ref);
    Serial.printf("RTC recalee sur utcClock, ecart %ld s\n", diff);
  }
}

void checkUtcTimeout() {
  if (utc_ok && (millis() - lastUpdateMillis > updateTimeout)) {
    utc_ok = false;
    Serial.println("⚠️ UTC timeout — RTC seule");
  }
}

// ---------- MQTT : jamais bloquant ----------
void reconnect() {
  if (RtcClient.connected()) return;

  unsigned long now = millis();
  if (now - lastReconnectAttempt < reconnectInterval) return;
  lastReconnectAttempt = now;

  if (RtcClient.connect(mqtt_client_id)) {
    RtcClient.subscribe(TOPIC_TIME_IN);
    Serial.println("[MQTT] Connected, subscribed utcClock");
  } else {
    Serial.printf("[MQTT] Connect failed, rc=%d\n", RtcClient.state());
  }
}

void publishRtc(const DateTime& now) {
  if (!rtc_ok || !RtcClient.connected()) return;

  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d %02d/%02d/%04d",
           now.hour(), now.minute(), now.second(),
           now.day(), now.month(), now.year());
  RtcClient.publish(TOPIC_RTC_OUT, buffer);
  Serial.printf("[MQTT] Published rtcClock: %s\n", buffer);
}

// ---------- Affichage Serial ----------
void printDateTime(const DateTime& now) {
  Serial.printf("%02d:%02d:%02d %02d/%02d/%04d\n",
                now.hour(), now.minute(), now.second(),
                now.day(), now.month(), now.year());
}

// ---------- Affichage OLED ----------
void Screen_display(const DateTime& now) {
  char timeStr[12];
  char dateStr[16];
  if (rtc_ok) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
             now.hour(), now.minute(), now.second());
    snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
             now.day(), now.month(), now.year());
  } else {
    strcpy(timeStr, "--:--:--");
    strcpy(dateStr, "--/--/----");
  }

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);

  display.setCursor(2, 0);
  display.print("RTC:");
  display.print(rtc_ok ? "OK" : "ERR");

  display.setCursor(2, 16);
  display.print("UTC:");
  display.print(utc_ok ? "OK" : "ERR");

  display.setCursor(2, 32);
  display.print(timeStr);

  display.setCursor(2, 48);
  display.print(dateStr);

  display.display();
}

// ---------- Bouton Reset : appui maintenu 5 s → redémarrage ----------
void handleResetButton() {
  if (!swReset.pressed) return;
  unsigned long now = millis();

  // Relâché avant 5 s (au-delà de 50 ms d'anti-rebond) : on annule
  if (now - swReset.pressStartTime > 50 && digitalRead(swReset.PIN) == HIGH) {
    swReset.pressed = false;
    ledState = false;
    rgbLedWrite(RGB_LED, 0, 0, 0);
    return;
  }

  // Maintenu : la LED clignote en rouge
  if (now - lastBlink > 200) {
    lastBlink = now;
    ledState = !ledState;
    rgbLedWrite(RGB_LED, ledState ? 10 : 0, 0, 0);
  }

  if (now - swReset.pressStartTime >= 5000) {
    Serial.println("Reset button 5s → restarting");
    rgbLedWrite(RGB_LED, 0, 0, 0);
    delay(100);
    ESP.restart();
  }
}

// ---------- Bouton Display + extinction automatique ----------
void handleDisplay() {
  if (!oled_ok) return;

  if (swDisplay.pressed) {
    swDisplay.pressed   = false;
    lastDisplayActivity = millis();
    if (!displayIsOn) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      displayIsOn = true;
      previousMillis = 0;          // redessin immédiat au prochain tour
    }
  }

  if (displayIsOn && (millis() - lastDisplayActivity > displayTimeout)) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayIsOn = false;
    Serial.println("OLED off");
  }
}

// ==========================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(version);

  Wire.begin(nSDA, nSCL, 100000);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);       // pas d'écriture en flash à chaque begin()
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);  // conservé, handleWiFi() sert de filet de sécurité
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastAttempt = millis();

  RtcClient.setServer(mqtt_server, mqtt_port);
  RtcClient.setCallback(callback);
  RtcClient.setSocketTimeout(5);

  // Init RTC
  rtc_ok = rtc.begin();
  if (!rtc_ok) {
    Serial.println("Couldn't find RTC");
  } else {
    Serial.println("RTC found");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power → compile time fallback");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  delay(200);
  oled_ok = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oled_ok) {
    Serial.println("OLED not found!");
  } else {
    display.setRotation(2);  // montage inversé dans le boîtier DIN
    display.clearDisplay();
    display.display();
  }

  pinMode(swReset.PIN,   INPUT_PULLUP);
  pinMode(swDisplay.PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(swReset.PIN),   isrSwReset,   FALLING);
  attachInterrupt(digitalPinToInterrupt(swDisplay.PIN), isrSwDisplay, FALLING);

  lastDisplayActivity = millis();
}

// ==========================================================
void loop() {
  handleWiFi();

  // Le réseau ne bloque ni l'affichage ni la lecture du DS3231
  if (WifiStatus) {
    reconnect();
    RtcClient.loop();
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= displayInterval) {
    previousMillis = currentMillis;
    DateTime now = rtc_ok ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);  // une seule lecture

    if (oled_ok && displayIsOn) {
      Screen_display(now);
    } else if (!oled_ok) {
      printDateTime(now);
    }

    if (currentMillis - previousPublish >= publishInterval) {
      previousPublish = currentMillis;
      publishRtc(now);
    }
  }

  checkUtcTimeout();
  handleResetButton();
  handleDisplay();

  delay(10);
}
