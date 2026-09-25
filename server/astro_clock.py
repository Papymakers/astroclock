from flask import Flask, jsonify, render_template
from flask_socketio import SocketIO
import paho.mqtt.client as mqtt
from datetime import datetime, timedelta
from astral import LocationInfo
from astral.sun import sun
from astral.moon import phase as moon_phase
import json
import time
import threading
import pytz
import socket
import re

# ==== Configuration ====
MQTT_BROKER = "192.168.1.2"
MQTT_PORT = 1883
MQTT_TOPIC_ASTRO = "astroClock"
MQTT_TOPIC_MOON = "astroClock/moon"
MQTT_TOPIC_RTC = "rtcClock"
MQTT_TOPIC_UTC = "utcClock"
LOCATION = LocationInfo("Lisieux", "France", "Europe/Paris", 49.14, 0.22)
moon_counter = 0
internet_ok = True   # mis à jour chaque minute par astro_publisher

# ==== Flask and WebSocket Setup ====
app = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

# ==== Page web (nouvelle page horloge/astro) ====
@app.route('/')
def index():
    print("[Flask] Serving index.html")
    return render_template('index.html')

# ==== MQTT Setup ====
# API de callbacks récente (paho-mqtt >= 2.0) — évite le warning de dépréciation
mqtt_client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
mqtt_connected = False
latest_rtc_data = None  # évite NameError si on_message est appelé avant toute assignation


def internet_available(host="8.8.8.8", port=53, timeout=3):
    """Check if internet is available by trying to connect to a known server."""
    # create_connection : timeout local à ce socket, fermé automatiquement
    # (setdefaulttimeout modifiait le timeout de tous les sockets du programme)
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def parse_rtc_string(rtc_str):
    """Convertit 'HH:MM:SS DD/MM/YYYY' (envoyé par l'ESP32 RTC) en JSON façon astroClock.
    Reconstruit le jour de la semaine à partir de la vraie date (et non de l'horloge
    du serveur), et fournit des champs neutres pour sunset/daylight/moonphase
    (non calculables sans repartir sur l'heure système)."""
    match = re.match(r"(\d{2}):(\d{2}):\d{2} (\d{2})/(\d{2})/(\d{4})", rtc_str)
    if not match:
        print("[RTC] Invalid format.")
        return None
    hh, mm, day, month, year = match.groups()
    try:
        dt = datetime(int(year), int(month), int(day))
        weekday = dt.isoweekday()
    except ValueError:
        weekday = 0
    return {
        "hours": int(hh),
        "minutes": int(mm),
        "day": int(day),
        "month": int(month),
        "weekday": weekday,
        "sunriseHour": 0,
        "sunriseMin": 0,
        "sunsetHour": 0,
        "sunsetMin": 0,
        "daylightDeltaStr": "n/a",
        "daylightChangeSinceSolsticeStr": "n/a",
        "moonphase": 0,
    }
def get_moon_label(p):
    if p < 1.75:  return "Nouvelle lune"
    if p < 7.0:   return "Croissant"
    if p < 8.75:  return "1er quartier"
    if p < 14.0:  return "Gibbeuse croiss."
    if p < 14.5:  return "Pleine lune"
    if p < 21.0:  return "Gibbeuse décr."
    if p < 22.75: return "Dernier quartier"
    return "Croissant décr."

def get_moon_symbol(p):
    if p < 1.75:  return "🌑"
    if p < 7.0:   return "🌒"
    if p < 8.75:  return "🌓"
    if p < 14.0:  return "🌔"
    if p < 14.5:  return "🌕"
    if p < 21.0:  return "🌖"
    if p < 22.75: return "🌗"
    return "🌘"

def forward_rtc_as_astro(rtc_payload):
    """Republie les données RTC comme donnée astroClock quand Internet est indisponible."""
    data = parse_rtc_string(rtc_payload)
    if data is None:
        return
    if mqtt_connected:
        mqtt_client.publish(MQTT_TOPIC_ASTRO, json.dumps(data), retain=True)
        print(f"[RTC→ASTRO] Forwarded RTC as astroClock: {data}")
    socketio.emit("astro_update", data)


def on_connect(client, userdata, flags, reason_code, properties=None):
    global mqtt_connected
    if reason_code == 0:
        print("[MQTT] Connected to broker!")
        mqtt_connected = True
        subscribe_topics()
    else:
        print(f"[MQTT] Failed to connect, code={reason_code}")
        mqtt_connected = False  # corrigé — le backup mettait True par erreur ici


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties=None):
    global mqtt_connected
    print("[MQTT] Disconnected!")
    mqtt_connected = False
    if reason_code != 0:
        print("[MQTT] Attempting to reconnect...")


def on_message(client, userdata, msg):
    global latest_rtc_data
    payload = msg.payload.decode()
    print(f"[MQTT] Message received on {msg.topic}: {payload}")

    if msg.topic == MQTT_TOPIC_RTC:
        latest_rtc_data = payload
        socketio.emit("rtc_update", {"rtc_time": payload})
        print(f"[ESP32 RTC] Updated: {payload}")

        # Si Internet est indisponible, on republie l'heure RTC comme donnée astro
        # Utilise l'état Internet mis en cache (testé une fois par minute) :
        # le module RTC publie toutes les 5 s, un test réseau à chaque message
        # bloquerait le thread MQTT jusqu'à 3 s quand Internet est coupé
        if not internet_ok:
            forward_rtc_as_astro(payload)


def subscribe_topics():
    if mqtt_connected:
        mqtt_client.subscribe(MQTT_TOPIC_RTC)
        print(f"[MQTT] Subscribed to {MQTT_TOPIC_RTC}")


mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message
mqtt_client.reconnect_delay_set(min_delay=1, max_delay=120)

try:
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_start()
    print("[MQTT] Initial connect called")
except Exception as e:
    print(f"[MQTT] Initial connection failed: {e}")


def mqtt_loop():
    global mqtt_connected
    while True:
        if not mqtt_connected:
            try:
                print("[MQTT] Not connected, attempting to reconnect...")
                mqtt_client.reconnect()
                print("[MQTT] Reconnected successfully")
            except Exception as e:
                print(f"[MQTT] Reconnect failed: {e}")
        time.sleep(5)
threading.Thread(target=mqtt_loop, daemon=True).start()

# ==== Utility Functions ====
def format_delta(minutes):
    sign = "+" if minutes >= 0 else "-"
    minutes = abs(minutes)
    hours = minutes // 60
    mins = minutes % 60
    if hours > 0:
        return f"{sign}{int(hours)}h{int(mins):02d}mn"
    else:
        return f"{sign}{int(mins)}mn"


def get_utc_data():
    tz = pytz.timezone(LOCATION.timezone)
    now = datetime.now(tz)
    return {
        "hours": now.hour,
        "minutes": now.minute,
        "seconds": now.second,
        "day": now.day,
        "month": now.month,
        "year": now.year,
    }

def get_astro_data():
    tz = pytz.timezone(LOCATION.timezone)
    now = datetime.now(tz)
    yesterday = now - timedelta(days=1)

    s_today = sun(LOCATION.observer, date=now.date(), tzinfo=tz)
    s_yesterday = sun(LOCATION.observer, date=yesterday.date(), tzinfo=tz)

    sunrise_today = s_today['sunrise']
    sunrise_yesterday = s_yesterday['sunrise']
    sunset_today = s_today['sunset']
    sunset_yesterday = s_yesterday['sunset']

    base = datetime(2000, 1, 1)
    dt_sunrise_today = datetime.combine(base, sunrise_today.time())
    dt_sunrise_yesterday = datetime.combine(base, sunrise_yesterday.time())
    dt_sunset_today = datetime.combine(base, sunset_today.time())
    dt_sunset_yesterday = datetime.combine(base, sunset_yesterday.time())

    sunrise_delta = (dt_sunrise_yesterday - dt_sunrise_today).total_seconds() / 60
    sunset_delta = (dt_sunset_today - dt_sunset_yesterday).total_seconds() / 60
    total_delta = sunrise_delta + sunset_delta

    year = now.year
    summer_solstice = datetime(year, 6, 21)
    winter_solstice = datetime(year, 12, 21)
    if now.date() >= summer_solstice.date():
        reference_date = summer_solstice
    else:
        reference_date = winter_solstice

    s_ref = sun(LOCATION.observer, date=reference_date.date(), tzinfo=tz)
    daylight_today = (sunset_today - sunrise_today).total_seconds() / 60
    daylight_ref = (s_ref['sunset'] - s_ref['sunrise']).total_seconds() / 60
    delta_since_solstice = daylight_today - daylight_ref
    delta_since_solstice_str = format_delta(delta_since_solstice)

    return {
        "hours": now.hour,
        "minutes": now.minute,
        "day": now.day,
        "month": now.month,
        "weekday": now.isoweekday(),
        "sunriseHour": sunrise_today.hour,
        "sunriseMin": sunrise_today.minute,
        "sunsetHour": sunset_today.hour,
        "sunsetMin": sunset_today.minute,
        "daylightDeltaStr": format_delta(total_delta),
        "daylightChangeSinceSolsticeStr": delta_since_solstice_str,
        "moonphase": round(moon_phase(now), 2),
#	"moonLabel": get_moon_label(round(moon_phase(now), 2)),
#	"moonSymbol": get_moon_symbol(round(moon_phase(now), 2))
    }

# ==== Get Moon phase data ====
def get_moon_data(now):
    p = round(moon_phase(now), 2)
    return {
        "moonphase": p,
        "moonLabel": get_moon_label(p),
        "moonSymbol": get_moon_symbol(p),
    }

# ==== Astro Clock Publisher ====
def astro_publisher():
     global moon_counter, internet_ok
     while True:
        try:
            if mqtt_connected:
                internet_ok = internet_available()
                if internet_ok:
                    # Heure de référence pour le module RTC de secours :
                    # publiée seulement si l'heure système est fiable (Internet présent),
                    # et non conservée, pour qu'un message périmé ne dérègle jamais la RTC
                    utc_data = get_utc_data()
                    mqtt_client.publish(MQTT_TOPIC_UTC, json.dumps(utc_data), retain=False)
                    print(f"[astro_publisher] Published UTC: {utc_data}")

                    astro_data = get_astro_data()
                    mqtt_client.publish(MQTT_TOPIC_ASTRO, json.dumps(astro_data), retain=True)
                    socketio.emit('astro_update', astro_data)
                    print(f"[astro_publisher] Published astro: {astro_data}")
                
                    # Publication lune toutes les 60 minutes
                    if moon_counter % 60 == 0:
                        tz = pytz.timezone(LOCATION.timezone)
                        now = datetime.now(tz)
                        moon_data = get_moon_data(now)
                        mqtt_client.publish(MQTT_TOPIC_MOON, json.dumps(moon_data), retain=True)
                        socketio.emit('moon_update', moon_data)
                        print(f"[astro_publisher] Published moon: {moon_data}")

                    moon_counter += 1 

                else:
                    # Le relais RTC -> astroClock est fait dans on_message, à la réception
                    # de chaque message rtcClock (heure fraîche, pas de doublon)
                    if latest_rtc_data:
                        print("[astro_publisher] Internet down, RTC backup active")
                    else:
                        print("[astro_publisher] Internet down, no RTC data yet")
            else:
                print("[astro_publisher] MQTT not connected, skipping publish")
        except Exception as e:
            print(f"[astro_publisher] Exception: {e}")
        # Publication calée sur le début de chaque minute (hh:mm:00) :
        # avec un simple sleep(60), la phase dépendait de l'heure de lancement
        # du script et dérivait de la durée de traitement à chaque tour
        time.sleep(60 - (time.time() % 60))


threading.Thread(target=astro_publisher, daemon=True).start()


# ==== REST Endpoint: Astro Data ====
@app.route('/astroclock', methods=['GET'])
def get_astroclock():
    try:
        astro_data = get_astro_data()
        print(f"[REST] Served /astroclock: {astro_data}")
        return jsonify(astro_data)
    except Exception as e:
        print(f"[REST] Exception in /astroclock: {e}")
        return jsonify({"error": "Failed to get astro data"}), 500


# ==== REST Endpoint: UTC/RTC Data ====
@app.route('/rtcclock', methods=['GET'])
def get_rtcclock():
    try:
        utc_data = get_utc_data()
        json_payload = json.dumps(utc_data)
        payload_size = len(json_payload.encode('utf-8'))
        print(f"[REST] Served /rtcclock: {utc_data} (Payload size: {payload_size} bytes)")
        return jsonify(utc_data)
    except Exception as e:
        print(f"[REST] Exception in /rtcclock: {e}")
        return jsonify({"error": "Failed to get UTC data"}), 500


# ==== Start Web Server ====
if __name__ == '__main__':
    print("[Flask] Starting Flask SocketIO server on 0.0.0.0:5000")
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True, use_reloader=False)
