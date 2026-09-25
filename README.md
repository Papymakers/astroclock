# AstroClock

*[English version](README.en.md)*

Horloges murales connectées sur écran **ESP32-2432S028** (« Cheap Yellow Display », 2,8" 320×240), alimentées par MQTT.

Chaque afficheur montre :

- l'heure (HH:MM, grands chiffres) ;
- la date (jour de la semaine, jour, mois, en français) ;
- la température extérieure, avec une couleur selon la plage de température ;
- un symbole WiFi (état de la connexion réseau) et son nom « Client N » (état MQTT).

L'heure et la date sont diffusées par un script Python (`server/astro_clock.py`). En cas de perte d'Internet, un module RTC de secours (`firmware/RtcBackup`) lui fournit l'heure. La température provient d'une station météo Ecowitt, via un flux MQTT produit en amont (hors de ce dépôt).

## Architecture

```
                  ┌──────────────────────┐
  Internet ──────►│  server/astro_clock  │── astroClock (60 s) ──┐
  (heure système) │  (Python, Flask)     │── astroClock/moon ────┤
                  │                      │                       │      ┌──────────────┐
  Module RTC ◄────│── utcClock ──────────│                       ├─────►│   Broker     │
  (secours)  ────►│── rtcClock ──────────│                       │      │  Mosquitto   │
                  └──────────────────────┘                       │      │              │
                                                                 │      └──────┬───────┘
  Station Ecowitt ── (chaîne amont) ── ecowittDatas ─────────────┘             │
                                                                               ▼
                                                       Client 6, Client 7, … (ESP32-2432S028)
```

## Contenu du dépôt

```
astroclock/
├── firmware/AstroClock/
│   ├── AstroClock.ino          programme modèle commun à toutes les horloges
│   └── secrets.h.example       modèle des identifiants WiFi
├── firmware/RtcBackup/
│   ├── RtcBackup.ino           module RTC de secours (ESP32-C3 + DS3231 + OLED)
│   └── secrets.h.example
├── server/
│   ├── astro_clock.py          diffusion de l'heure, des données astro et page web
│   └── requirements.txt
├── README.md
└── README.en.md
```

## Contrat MQTT

C'est la partie à respecter si tu modifies l'un des deux côtés, ou si tu branches une autre source de données.

Chaque horloge souscrit à **deux topics** :

```cpp
mqttClient.subscribe("ecowittDatas");
mqttClient.subscribe("astroClock/#");
```

### `astroClock` — heure et date

Publié par `server/astro_clock.py` toutes les 60 s, avec `retain=True`. Une horloge qui démarre reçoit donc immédiatement le dernier message.

Le script publie un objet JSON complet, mais **l'horloge n'utilise que les champs suivants** :

| Clé       | Type   | Utilisation par l'horloge                         |
|-----------|--------|---------------------------------------------------|
| `hours`   | entier | heure, 0–23 — resynchronise l'horloge locale       |
| `minutes` | entier | minutes, 0–59                                     |
| `day`     | entier | jour du mois, 1–31                                |
| `month`   | entier | mois, 1–12                                        |
| `weekday` | entier | jour de la semaine, **1 = lundi … 7 = dimanche** (ISO) |

`sunriseHour` et `sunriseMin` sont lus mais pas affichés. Les autres champs (`sunsetHour`, `sunsetMin`, `daylightDeltaStr`, `daylightChangeSinceSolsticeStr`, `moonphase`) sont ignorés par les horloges ; ils servent à la page web du serveur.

Exemple de message :

```json
{"hours": 14, "minutes": 32, "day": 24, "month": 9, "weekday": 4,
 "sunriseHour": 7, "sunriseMin": 49, "sunsetHour": 19, "sunsetMin": 45,
 "daylightDeltaStr": "-3mn", "daylightChangeSinceSolsticeStr": "-4h07mn",
 "moonphase": 12.34}
```

Les valeurs hors plage sont rejetées : une heure invalide ne resynchronise pas l'horloge, une date invalide n'est pas affichée.

### `astroClock/moon` — phase de lune

Publié toutes les heures (`retain=True`). Il est reçu par les horloges à cause du joker `#`, mais il est **ignoré**.

### `ecowittDatas` — température extérieure

Produit en amont, à partir des données de la station Ecowitt (hors de ce dépôt). Le message contient de nombreuses mesures, mais **l'horloge ne lit que la clé `tempExt`** :

| Clé       | Type    | Utilisation par l'horloge          |
|-----------|---------|------------------------------------|
| `tempExt` | flottant | température extérieure en °C      |

Toute autre clé est ignorée. Si `tempExt` est absente, la température affichée n'est pas modifiée.

### Topics non utilisés par les horloges

| Topic       | Sens                  | Rôle |
|-------------|-----------------------|------|
| `utcClock`  | serveur → module RTC  | heure de référence en JSON (`hours`, `minutes`, `seconds`, `day`, `month`, `year`). Publiée toutes les minutes **uniquement quand Internet est présent**, **sans** `retain` |
| `rtcClock`  | module RTC → serveur  | heure du DS3231 au format texte `HH:MM:SS DD/MM/YYYY`, toutes les 5 s |

### Taille des messages

Le message `astroClock` fait environ 230 octets de JSON, soit près de 245 octets avec l'en-tête MQTT. La bibliothèque PubSubClient **ignore silencieusement** tout message plus grand que son tampon, qui fait 256 octets par défaut. Le firmware porte donc ce tampon à 512 octets (`mqttClient.setBufferSize(512)`). Le document JSON (ArduinoJson 7) s'adapte automatiquement à la taille du message.

Si tu ajoutes des champs au message `astroClock`, ou si le message `ecowittDatas` dépasse ~500 octets, augmente la taille du tampon.

## Matériel

- **Carte** : ESP32-2432S028 (écran TFT 2,8" 320×240, contrôleur ILI9341 sur la plupart des versions)
- **Broches utilisées par le firmware** :

| Broche | Rôle |
|--------|------|
| GPIO 22 | rétroéclairage en PWM (`BACKLIGHT_PIN`) — **nécessite la modification ci-dessous** |
| GPIO 34 | photorésistance (`LDR_PIN`) — réservé, non utilisé |
| GPIO 26 | haut-parleur (`SPEAKER_PIN`) — fonction `beep()` disponible, non appelée |

### Modification du rétroéclairage

D'origine, le CYD commande le transistor du rétroéclairage par **GPIO 21**. Cette broche assure deux fonctions sur la carte et ne permet pas de moduler l'intensité de l'écran.

Pour obtenir la variation de luminosité jour/nuit :

1. **Couper la piste** qui relie la gate du transistor de rétroéclairage à GPIO 21.
2. **Relier la gate à GPIO 22** par un fil.

Le firmware pilote alors le rétroéclairage en PWM sur GPIO 22 (5 kHz, 8 bits).

Pour plus de détails, schémas à l'appui, voir le dépôt CYD-Heating-Remote-2zones / hardware.

**Sans cette modification**, l'horloge fonctionne, mais sans variation de luminosité. Il faut alors laisser TFT_eSPI allumer l'écran en permanence en définissant dans sa configuration :


```cpp
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH
```

## Firmware

### Prérequis

- **Core Arduino ESP32 ≥ 3.0** (le code utilise `ledcAttach()`, qui n'existe pas en 2.x)
- **TFT_eSPI ≥ 2.5.0** (`drawArc()`, `fillSmoothCircle()`, `drawWideLine()`)
- **PubSubClient**
- **ArduinoJson 7.x**

### Configuration de TFT_eSPI

TFT_eSPI doit être configurée pour l'ESP32-2432S028. Les polices suivantes doivent être chargées, car le firmware les utilise :

```cpp
#define LOAD_GLCD   // police 1 : "Client N"
#define LOAD_FONT4  // police 4 : date et température
#define LOAD_FONT8  // police 8 : heure
```

Avec la modification du rétroéclairage, le firmware le pilote lui-même en PWM sur `BACKLIGHT_PIN` : **ne pas définir `TFT_BL`** dans la configuration de TFT_eSPI.

### Installer un nouvel afficheur

1. Copier `firmware/AstroClock/secrets.h.example` en `secrets.h` et renseigner le SSID et le mot de passe WiFi.
2. Dans `AstroClock.ino`, changer **uniquement** la ligne :
   ```cpp
   #define CLIENT_NUM 7
   ```
   Elle détermine à la fois le texte affiché (`Client 7`) et l'identifiant envoyé au broker (`clock7`).
3. Vérifier l'adresse du broker (`mqtt_server`), puis flasher.

> ⚠️ **Chaque afficheur doit avoir un `CLIENT_NUM` différent.** Deux clients MQTT avec le même identifiant s'éjectent mutuellement du broker : ils affichent « MQTT OK » en boucle dans le moniteur série, mais ne reçoivent jamais l'heure.

Le moniteur série (115200 bauds) affiche la version au démarrage, puis les tentatives de connexion WiFi et MQTT.

## Comportement de l'afficheur

### Indicateurs

| Élément | Vert | Rouge / barré |
|---------|------|---------------|
| Symbole WiFi (haut droite) | WiFi connecté | gris barré de rouge : WiFi perdu |
| « Client N » (bas droite) | MQTT connecté **et** heure reçue depuis moins de 3 min | broker injoignable, ou plus de message `astroClock` depuis 3 min |

Au démarrage, « Client N » est rouge, puis passe au vert dès la réception du premier message `astroClock`. L'heure, la date et la température ne s'affichent qu'à la réception de leur première donnée valide.

### Jour et nuit

Entre 8 h et 22 h, le rétroéclairage est à 175/255 et les textes sont en couleur. La nuit, le rétroéclairage passe à 50/255 et tout s'affiche en vert foncé. Ces valeurs sont réglables par les constantes `BRIGHT_DAY`, `BRIGHT_NIGHT`, `DAY_START_HOUR` et `DAY_END_HOUR`.

Couleurs de la température (en journée) :

| Plage | Couleur |
|-------|---------|
| < 0,1 °C | bleu pâle |
| 0,1 à 10 °C | bleu foncé |
| 10 à 20 °C | vert |
| 20 à 30 °C | orange |
| ≥ 30 °C | rouge |

### En cas de coupure

- **WiFi** : nouvelle tentative toutes les 10 s. Si le WiFi est absent depuis 5 min, l'ESP32 redémarre.
- **MQTT** : nouvelle tentative toutes les 5 s, uniquement quand le WiFi est présent.
- **Heure** : entre deux messages, l'horloge avance localement grâce à `millis()`. L'affichage continue de tourner pendant une coupure et se recale au premier message reçu.

La gestion réseau n'est jamais bloquante : l'affichage reste à jour pendant les coupures.

## Serveur (`server/astro_clock.py`)

### Rôle

- publie `astroClock` toutes les minutes (heure, date, lever et coucher du soleil, durée du jour) ;
- publie `astroClock/moon` toutes les heures ;
- vérifie l'accès Internet (connexion vers `8.8.8.8:53`) :
  - s'il est présent, publie aussi `utcClock` pour recaler le module RTC de secours ;
  - s'il est absent, republie chaque message `rtcClock` reçu comme message `astroClock` ;
- sert une page web et deux points d'accès REST sur le port 5000 :
  - `/` — page horloge/astro (fichier `templates/index.html`) ;
  - `/astroclock` — données astro en JSON ;
  - `/rtcclock` — heure locale en JSON.

### Configuration

En tête du script : adresse et port du broker (`MQTT_BROKER`, `MQTT_PORT`), noms des topics, et lieu d'observation (`LOCATION` : nom, pays, fuseau horaire, latitude, longitude) pour le calcul des heures de lever et de coucher du soleil.

### Installation

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python astro_clock.py
```

## Module RTC de secours (`firmware/RtcBackup`)

### Rôle

Quand Internet est coupé, le serveur ne peut plus garantir que son heure système est juste. Le module RTC prend alors le relais :

1. **Internet présent** : le serveur publie `utcClock` toutes les minutes. Le module recale son DS3231 si l'écart dépasse 2 s.
2. **Internet absent** : le serveur ne publie plus `utcClock`. Le module continue de publier l'heure de son DS3231 sur `rtcClock` toutes les 5 s, et le serveur la republie en `astroClock` pour les horloges.

Le module n'est pas une horloge maître : les horloges ne l'écoutent jamais directement, tout passe par le serveur.

### Matériel

- ESP32-C3 DevKitM-1, en boîtier rail DIN RS PRO 105×90×65
- DS3231 (adresse `0x68`) et OLED SSD1306 128×64 (adresse `0x3C`) sur le même bus I2C : SDA = GPIO 1, SCL = GPIO 10. L'OLED est monté à l'envers dans le boîtier (`setRotation(2)`)
- Bouton **GPIO 7** : appui maintenu 5 s → redémarrage (la LED clignote en rouge pendant l'appui ; relâcher avant 5 s annule)
- Bouton **GPIO 6** : rallume l'OLED
- LED RGB sur GPIO 8

### Écran

| Ligne | Contenu |
|-------|---------|
| 1 | `RTC:OK` / `RTC:ERR` — DS3231 détecté ou non |
| 2 | `UTC:OK` / `UTC:ERR` — message `utcClock` valide reçu depuis moins de 3 min |
| 3 | heure du DS3231 |
| 4 | date du DS3231 |

L'OLED s'éteint après 15 s sans appui sur le bouton GPIO 6. En mode secours (Internet coupé), `UTC:ERR` est donc l'état normal.

### Bibliothèques

Core Arduino ESP32 ≥ 3.0 (`rgbLedWrite()`), Adafruit RTClib, Adafruit SSD1306, Adafruit GFX, PubSubClient, ArduinoJson 7.x.

### Mise en service

1. Copier `secrets.h.example` en `secrets.h` et renseigner le WiFi.
2. Flasher. Si le DS3231 a perdu l'alimentation, il est réglé sur l'heure de compilation, puis recalé par `utcClock` dès la première minute.

L'identifiant MQTT est `ESP32_RTC_Master` : il ne doit être utilisé par aucun autre appareil.

### Migration depuis une installation existante

Les anciennes versions du serveur publiaient `utcClock` avec `retain=True`. Supprimer une fois le message conservé sur le broker, sinon le module se recalerait au démarrage sur une heure périmée :

```bash
mosquitto_pub -h 192.168.1.20 -t utcClock -r -n
```

## Limites connues

- **Message conservé périmé** : `astroClock` est publié avec `retain=True`. Si le script serveur est arrêté, une horloge qui redémarre reçoit le dernier message conservé et affiche une heure fausse. « Client N » passe au rouge au bout de 3 min, ce qui signale le problème.
- **Changement de date pendant une coupure** : l'horloge locale fait avancer l'heure, mais pas la date. La date se met à jour au premier message reçu.
- **Mode secours RTC** : en l'absence d'Internet, les champs astro (lever et coucher du soleil, phase de lune) sont publiés à zéro. Les horloges ne sont pas concernées, puisqu'elles n'affichent que l'heure et la date.
- **`utcClock` contient l'heure locale** (fuseau `Europe/Paris`), pas l'heure UTC, malgré son nom. Le DS3231 stocke donc l'heure locale. Si Internet reste coupé pendant un changement d'heure été/hiver, le module garde l'ancienne heure jusqu'au retour d'Internet.
- **Critère « Internet présent »** : le serveur teste l'accès à `8.8.8.8:53`, pas la synchronisation NTP de sa propre horloge. Sur une machine sans horloge matérielle (Raspberry Pi 4 par exemple), vérifier la synchronisation NTP serait plus sûr.
