#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "DHTesp.h"

const int PIN_DHT = 15;
const int PIN_LDR = 34;
const int PIN_MOISTURE = 35;
const int PIN_FLOAT_SW = 4;
const int PIN_ESTOP = 21;

const int PIN_RELAY_MIST = 2;
const int PIN_RELAY_FAN = 13;
const int PIN_RELAY_SPRAY = 14;
const int PIN_SIREN_LED = 12;

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_SERVER = "broker.hivemq.com";
const int MQTT_PORT = 1883;

const char* DEVICE_ID = "MUSHROOM_FARM_01";
const char* TOPIC_TELEMETRY = "mushroom_farm/MUSHROOM_FARM_01/telemetry";
const char* TOPIC_CONTROL   = "mushroom_farm/MUSHROOM_FARM_01/control";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
DHTesp dhtSensor;

bool isAutoMode = true;
bool isEmergencyStop = false;
bool mistPumpState = false;
bool fanState = false;
bool sprayState = false;

unsigned long lastTelemetryTime = 0;
const unsigned long TELEMETRY_INTERVAL = 2000;

void setupWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected!");
}

void triggerEmergencyStop() {
  isEmergencyStop = true;
  mistPumpState = false;
  fanState = false;
  sprayState = false;
  applyActuatorOutputs();
  digitalWrite(PIN_SIREN_LED, HIGH);
  Serial.println("!!! EMERGENCY STOP ACTIVATED !!!");
}

void resetEmergencyStop() {
  isEmergencyStop = false;
  digitalWrite(PIN_SIREN_LED, LOW);
  Serial.println("Emergency Stop Reset.");
}

void applyActuatorOutputs() {
  digitalWrite(PIN_RELAY_MIST, mistPumpState ? HIGH : LOW);
  digitalWrite(PIN_RELAY_FAN, fanState ? HIGH : LOW);
  digitalWrite(PIN_RELAY_SPRAY, sprayState ? HIGH : LOW);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.printf("MQTT In: %s\n", message.c_str());

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, message)) return;

  if (doc.containsKey("estop")) {
    if (doc["estop"] == true) triggerEmergencyStop();
    else resetEmergencyStop();
  }

  if (doc.containsKey("mode") && !isEmergencyStop) {
    isAutoMode = (doc["mode"] == "AUTO");
  }

  if (!isAutoMode && !isEmergencyStop) {
    if (doc.containsKey("mist_pump")) mistPumpState = doc["mist_pump"];
    if (doc.containsKey("fan")) fanState = doc["fan"];
    if (doc.containsKey("spray")) sprayState = doc["spray"];
    applyActuatorOutputs();
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting MQTT...");
    String clientId = "ESP32_Mushroom_" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected");
      mqttClient.subscribe(TOPIC_CONTROL);
    } else {
      delay(2000);
    }
  }
}

void runClosedLoopAutomation(float temp, float humidity, int moisturePercent, bool isWaterFull) {
  if ((humidity < 80.0 || moisturePercent < 60) && isWaterFull) {
    mistPumpState = true;
  } else if (humidity >= 88.0 || !isWaterFull) {
    mistPumpState = false;
  }

  if (temp > 28.0) fanState = true;
  else if (temp < 25.0) fanState = false;

  if (temp > 32.0 && isWaterFull) sprayState = true;
  else if (temp <= 29.0) sprayState = false;

  applyActuatorOutputs();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_FLOAT_SW, INPUT_PULLUP);
  pinMode(PIN_ESTOP, INPUT_PULLUP);
  pinMode(PIN_RELAY_MIST, OUTPUT);
  pinMode(PIN_RELAY_FAN, OUTPUT);
  pinMode(PIN_RELAY_SPRAY, OUTPUT);
  pinMode(PIN_SIREN_LED, OUTPUT);

  applyActuatorOutputs();
  digitalWrite(PIN_SIREN_LED, LOW);

  dhtSensor.setup(PIN_DHT, DHTesp::DHT22);
  setupWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  if (digitalRead(PIN_ESTOP) == LOW && !isEmergencyStop) {
    triggerEmergencyStop();
    delay(200);
  }

  TempAndHumidity dhtData = dhtSensor.getTempAndHumidity();
  float temperature = dhtData.temperature;
  float humidity = dhtData.humidity;

  int ldrPercent = map(analogRead(PIN_LDR), 0, 4095, 0, 100);
  int moisturePercent = map(analogRead(PIN_MOISTURE), 0, 4095, 0, 100);
  bool isWaterFull = (digitalRead(PIN_FLOAT_SW) == HIGH);

  if (isAutoMode && !isEmergencyStop) {
    runClosedLoopAutomation(temperature, humidity, moisturePercent, isWaterFull);
  }

  unsigned long now = millis();
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL) {
    lastTelemetryTime = now;

    StaticJsonDocument<300> doc;
    doc["device_id"] = DEVICE_ID;
    doc["temperature"] = serialized(String(temperature, 1));
    doc["humidity"] = serialized(String(humidity, 1));
    doc["moisture"] = moisturePercent;
    doc["light_level"] = ldrPercent;
    doc["water_full"] = isWaterFull;
    doc["mode"] = isAutoMode ? "AUTO" : "MANUAL";
    doc["estop"] = isEmergencyStop;
    doc["mist_pump"] = mistPumpState;
    doc["fan"] = fanState;
    doc["spray"] = sprayState;

    char buffer[350];
    serializeJson(doc, buffer);
    mqttClient.publish(TOPIC_TELEMETRY, buffer);
    Serial.println("Telemetry: " + String(buffer));
  }
}