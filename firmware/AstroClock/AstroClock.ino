// AstroClock — afficheur horloge MQTT
// Carte : ESP32-2432S028 ("Cheap Yellow Display", 2,8" 320x240)
// Affiche : heure, date, température extérieure
//
// Pour un nouvel afficheur : changer CLIENT_NUM, créer secrets.h, flasher.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD (voir secrets.h.example)

// ---------------------------------------------------------------------------
// Identité de l'afficheur — seule ligne à modifier d'un afficheur à l'autre
// ---------------------------------------------------------------------------
#define CLIENT_NUM 6

String version = "0_AstroClock_client_" + String(CLIENT_NUM) + "c - 24/09/26";
String clientID = "Client " + String(CLIENT_NUM);        // texte affiché
String mqtt_client_id = "clock" + String(CLIENT_NUM);    // identifiant broker (unique !)

// ---------------------------------------------------------------------------
// Réseau
// ---------------------------------------------------------------------------
const char *mqtt_server = "192.168.1.20";
const uint16_t mqtt_port = 1883;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

const unsigned long wifiRetryInterval = 10000;               // 10 s entre tentatives WiFi
const unsigned long mqttRetryInterval = 5000;                // 5 s entre tentatives MQTT
const unsigned long wifiRebootTimeout = 5UL * 60UL * 1000UL; // reboot si WiFi absent 5 min
const unsigned long timeStaleTimeout = 180000UL;             // heure non reçue depuis 3 min

bool WifiStatus = false;
bool lastWifiStatus = true;          // forcé différent pour le premier affichage
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long wifiLostSince = 0;
int8_t lastMqttState = -1;           // -1 : force le premier affichage

// ---------------------------------------------------------------------------
// Matériel
// ---------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();

constexpr int LDR_PIN = 34;        // GPIO34 : ADC1 (entrée seule) — réservé
constexpr int BACKLIGHT_PIN = 22;  // PWM rétroéclairage
constexpr int PWM_FREQ = 5000;
constexpr int PWM_RESOLUTION = 8;  // 0–255
constexpr int SPEAKER_PIN = 26;    // réservé

constexpr uint8_t BRIGHT_DAY = 175;
constexpr uint8_t BRIGHT_NIGHT = 50;
constexpr int DAY_START_HOUR = 8;
constexpr int DAY_END_HOUR = 22;

// ---------------------------------------------------------------------------
// Affichage / données
// ---------------------------------------------------------------------------
String SunriseHour = "", SunriseMin = "";
String Day = "", Month = "", weekDay = "";
String lastDateStr = "";

const char *daysOfWeek[] = { "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche" };
const char *months[] = { "Janvier", "Février", "Mars", "Avril", "Mai", "Juin", "Juillet", "Aout", "Septembre", "Octobre", "Novembre", "Décembre" };

uint16_t textColor;
int previousWidth = 0;
bool dayTime = true;

uint8_t hh = 0, mm = 0;
byte omm = 99;
int16_t xpos = 0;
int16_t ypos = 0;

float tempExt = 0.0;
float lastTemp = 99.9;   // valeur invalide pour forcer le premier affichage
bool tempValid = false;

// Horloge locale : continue de tourner entre deux messages MQTT
bool timeValid = false;
uint16_t syncMinutes = 0;        // minutes depuis minuit lors de la dernière synchro
unsigned long syncMillis = 0;

// ---------------------------------------------------------------------------
// WiFi : jamais bloquant
// ---------------------------------------------------------------------------
void handleWiFi() {
  WifiStatus = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  if (WifiStatus) {
    wifiLostSince = 0;
    return;
  }

  if (wifiLostSince == 0) wifiLostSince = now ? now : 1;

  if (now - lastWifiAttempt >= wifiRetryInterval) {
    lastWifiAttempt = now;
    Serial.println("WiFi perdu -> nouvelle tentative");
    WiFi.disconnect();                 // remet la pile STA dans un état propre
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (now - wifiLostSince > wifiRebootTimeout) {
    Serial.println("WiFi absent depuis 5 min -> redemarrage");
    delay(100);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------
// MQTT : jamais bloquant, uniquement si le WiFi est là
// ---------------------------------------------------------------------------
void handleMQTT() {
  if (!WifiStatus) return;

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttAttempt < mqttRetryInterval) return;
  lastMqttAttempt = now;

  Serial.println("Connexion MQTT...");
  if (mqttClient.connect(mqtt_client_id.c_str())) {
    mqttClient.subscribe("ecowittDatas");
    mqttClient.subscribe("astroClock/#");
    Serial.println("MQTT OK");
  } else {
    Serial.printf("MQTT echec, rc=%d\n", mqttClient.state());
  }
}

// ---------------------------------------------------------------------------
// Réception MQTT
// ---------------------------------------------------------------------------
void callback(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;   // ArduinoJson 7 : taille gérée automatiquement

  // Pas d'écriture dans payload : deserializeJson reçoit la longueur
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.f_str());
    return;
  }

  String topicStr = topic;

  if (topicStr == "ecowittDatas") {
    if (!doc["tempExt"].isNull()) {
      tempExt = doc["tempExt"].as<float>();
      tempValid = true;
    }
  } else if (topicStr == "astroClock") {
    JsonObject obj = doc.as<JsonObject>();
    int newH = -1, newM = -1;

    for (JsonPair kv : obj) {
      String key = kv.key().c_str();
      if (key == "hours") newH = kv.value().as<String>().toInt();
      else if (key == "minutes") newM = kv.value().as<String>().toInt();
      else if (key == "sunriseHour") SunriseHour = kv.value().as<String>();
      else if (key == "sunriseMin") SunriseMin = kv.value().as<String>();
      else if (key == "day") Day = kv.value().as<String>();
      else if (key == "month") Month = kv.value().as<String>();
      else if (key == "weekday") weekDay = kv.value().as<String>();
    }

    // Resynchronise l'horloge locale si l'heure reçue est cohérente
    if (newH >= 0 && newH < 24 && newM >= 0 && newM < 60) {
      syncMinutes = newH * 60 + newM;
      syncMillis = millis();
      timeValid = true;
    }
  }
  // Les autres sous-topics (ex. astroClock/moon) sont ignorés
}

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------
void updateLocalTime() {
  if (!timeValid) return;
  unsigned long elapsedMin = (millis() - syncMillis) / 60000UL;
  uint16_t total = (syncMinutes + elapsedMin) % 1440;
  hh = total / 60;
  mm = total % 60;
}

void beep(int count, int duration) {
  for (int i = 0; i < count; i++) {
    tone(SPEAKER_PIN, 2000);
    delay(duration);
    noTone(SPEAKER_PIN);
    delay(200);
  }
}

String formatTime(String hour, String minute) {
  if (hour.length() == 1) hour = "0" + hour;
  if (minute.length() == 1) minute = "0" + minute;
  return hour + ":" + minute;
}

// ---------------------------------------------------------------------------
// Blocs d'affichage
// ---------------------------------------------------------------------------
void updateBrightness() {
  if (!timeValid) return;
  bool shouldBeDay = (hh >= DAY_START_HOUR && hh < DAY_END_HOUR);
  if (shouldBeDay == dayTime) return;

  dayTime = shouldBeDay;
  ledcWrite(BACKLIGHT_PIN, dayTime ? BRIGHT_DAY : BRIGHT_NIGHT);

  // Force le redessin avec les couleurs jour/nuit
  omm = 99;
  lastTemp = 99.9;
  lastDateStr = "";
}

// Symbole WiFi en haut à droite : 3 arcs + un point
// Connecté : vert foncé / Déconnecté : gris barré de rouge
void drawWifiSymbol(int16_t cx, int16_t cy, bool connected) {
  uint16_t col = connected ? TFT_DARKGREEN : TFT_DARKGREY;

  // Zone du symbole + débordement de la barre oblique (épaisseur + anticrénelage)
  tft.fillRect(cx - 18, cy - 19, 36, 24, TFT_BLACK);

  // drawArc : angle 0 = bas, sens horaire -> 135..225 = éventail vers le haut
  tft.drawArc(cx, cy, 17, 15, 135, 225, col, TFT_BLACK, true);
  tft.drawArc(cx, cy, 12, 10, 135, 225, col, TFT_BLACK, true);
  tft.drawArc(cx, cy,  7,  5, 135, 225, col, TFT_BLACK, true);
  tft.fillSmoothCircle(cx, cy, 2, col, TFT_BLACK);

  if (!connected) {
    tft.drawWideLine(cx - 12, cy - 16, cx + 12, cy + 1, 3, TFT_RED, TFT_BLACK);
  }
}

void drawWifiIndicator() {
  if (WifiStatus == lastWifiStatus) return;
  drawWifiSymbol(tft.width() - 20, 19, WifiStatus);
  lastWifiStatus = WifiStatus;
}

// "Client N" : vert si MQTT connecté ET heure reçue depuis moins de 3 min, sinon rouge
void drawClientId() {
  bool mqttOk = mqttClient.connected() && timeValid && (millis() - syncMillis < timeStaleTimeout);
  if (mqttOk == lastMqttState) return;

  tft.setTextColor(mqttOk ? TFT_DARKGREEN : TFT_RED, TFT_BLACK);
  xpos = tft.width() - tft.textWidth(clientID);
  ypos = tft.height() - 16;
  tft.drawString(clientID, xpos, ypos);
  lastMqttState = mqttOk;
}

void drawTime() {
  if (!timeValid || omm == mm) return;
  omm = mm;

  xpos = 40;
  ypos = 30;
  tft.setTextColor(dayTime ? TFT_GREEN : TFT_DARKGREEN, TFT_BLACK);

  if (hh < 10) xpos += tft.drawChar('0', xpos, ypos, 8);
  xpos += tft.drawNumber(hh, xpos, ypos, 8);
  xpos += tft.drawChar(':', xpos, ypos - 8, 8);
  if (mm < 10) xpos += tft.drawChar('0', xpos, ypos, 8);
  xpos += tft.drawNumber(mm, xpos, ypos, 8);
}

void drawDate() {
  int jourSem = weekDay.toInt();
  int jour = Day.toInt();
  int mois = Month.toInt();

  // Rien à afficher tant que les données ne sont pas valides
  if (jourSem < 1 || jourSem > 7 || mois < 1 || mois > 12 || jour < 1 || jour > 31) return;

  String dateStr = String(daysOfWeek[jourSem - 1]) + " " + String(jour) + " " + months[mois - 1];
  if (dateStr == lastDateStr) return;

  tft.fillRect(20, 160, 300, 30, TFT_BLACK);
  tft.setTextColor(dayTime ? TFT_ORANGE : TFT_DARKGREEN, TFT_BLACK);
  tft.drawString(dateStr, 20, 160, 4);
  lastDateStr = dateStr;
}

void drawTemperature() {
  if (!tempValid || fabs(tempExt - lastTemp) < 0.1) return;

  const int ytemp = 200;
  String tempStr = "Temp. Ext. : " + String(tempExt, 1) + " `C";
  tft.fillRect(20, ytemp, previousWidth, 30, TFT_BLACK);

  if (!dayTime) {
    textColor = TFT_DARKGREEN;
  } else if (tempExt < 0.1) {
    textColor = tft.color565(180, 210, 255);  // bleu pâle
  } else if (tempExt < 10.0) {
    textColor = tft.color565(70, 110, 160);   // bleu foncé
  } else if (tempExt < 20.0) {
    textColor = tft.color565(60, 200, 100);   // vert
  } else if (tempExt < 30.0) {
    textColor = tft.color565(255, 160, 0);    // orange
  } else {
    textColor = TFT_RED;
  }

  tft.setTextColor(textColor, TFT_BLACK);
  tft.drawString(tempStr, 20, ytemp, 4);
  previousWidth = tft.textWidth(tempStr, 4);
  lastTemp = tempExt;
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(10);
  Serial.println(version);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);      // pas d'écriture en flash à chaque begin()
  WiFi.setSleep(false);        // plus stable pour un appareil sur secteur
  WiFi.setAutoReconnect(true); // conservé, handleWiFi() sert de filet de sécurité
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();

  pinMode(SPEAKER_PIN, OUTPUT);
  ledcAttach(BACKLIGHT_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(BACKLIGHT_PIN, BRIGHT_DAY);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(512); // le message astroClock fait ~245 octets, limite par défaut 256
  mqttClient.setSocketTimeout(5);
}

void loop() {
  handleWiFi();
  handleMQTT();

  updateLocalTime();
  updateBrightness();

  drawWifiIndicator();
  drawClientId();
  drawTime();
  drawDate();
  drawTemperature();

  delay(10);
}
