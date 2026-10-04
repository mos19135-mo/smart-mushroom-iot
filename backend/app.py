import json
import sqlite3
from flask import Flask, jsonify, request, send_from_directory
from flask_cors import CORS
import paho.mqtt.client as mqtt

app = Flask(__name__, static_folder="../frontend", static_url_path="")
CORS(app)

DB_PATH = "database.db"
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
TOPIC_TELEMETRY = "mushroom_farm/+/telemetry"
TOPIC_CONTROL_PREFIX = "mushroom_farm"

current_telemetry = {}

def init_db():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''CREATE TABLE IF NOT EXISTS telemetry (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT,
                    temperature REAL,
                    humidity REAL,
                    moisture INTEGER,
                    light_level INTEGER,
                    water_full INTEGER,
                    mode TEXT,
                    estop INTEGER,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                )''')
    conn.commit()
    conn.close()

def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT Broker with code {rc}")
    client.subscribe(TOPIC_TELEMETRY)

def on_message(client, userdata, msg):
    global current_telemetry
    try:
        data = json.loads(msg.payload.decode())
        device_id = data.get("device_id", "UNKNOWN")
        current_telemetry[device_id] = data

        conn = sqlite3.connect(DB_PATH)
        c = conn.cursor()
        c.execute('''INSERT INTO telemetry 
                     (device_id, temperature, humidity, moisture, light_level, water_full, mode, estop)
                     VALUES (?, ?, ?, ?, ?, ?, ?, ?)''',
                  (device_id, data.get("temperature"), data.get("humidity"),
                   data.get("moisture"), data.get("light_level"), 
                   1 if data.get("water_full") else 0,
                   data.get("mode"), 1 if data.get("estop") else 0))
        conn.commit()
        conn.close()
    except Exception as e:
        print("Error processing MQTT message:", e)

mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
try:
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_start()
except Exception as e:
    print("MQTT Connection Warning:", e)

@app.route("/")
def index():
    return send_from_directory("../frontend", "index.html")

@app.route("/api/telemetry", methods=["GET"])
def get_telemetry():
    device_id = request.args.get("device_id", "MUSHROOM_FARM_01")
    return jsonify(current_telemetry.get(device_id, {}))

@app.route("/api/control", methods=["POST"])
def send_control():
    body = request.json
    device_id = body.get("device_id", "MUSHROOM_FARM_01")
    topic = f"{TOPIC_CONTROL_PREFIX}/{device_id}/control"
    mqtt_client.publish(topic, json.dumps(body))
    return jsonify({"status": "command_published", "topic": topic, "payload": body})

if __name__ == "__main__":
    init_db()
    app.run(host="0.0.0.0", port=5000, debug=True)