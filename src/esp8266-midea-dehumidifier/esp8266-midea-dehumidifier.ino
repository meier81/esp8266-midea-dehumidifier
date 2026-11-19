#include "types.h"
#include "wifi.h"

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <FS.h>

dehumidifierState_t state;

byte serialRxBuf[255];
byte serialTxBuf[255];

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;
ESP8266WebServer webServer(80);

unsigned long lastMqttConnectionAttempt = millis();
const long mqttConnectionInterval = 60000;

unsigned long statusPollPreviousMillis = millis();
const long statusPollInterval = 30000;

char identifier[24];
#define FIRMWARE_PREFIX "comfee"
#define FIRMWARE_VERSION "2026.10.0"

#define AVAILABILITY_ONLINE "Online"
#define AVAILABILITY_OFFLINE "Offline"
char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_COMMAND[128];

char MQTT_TOPIC_AUTOCONF_WIFI_SENSOR[128];
char MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR[128];

char MQTT_TOPIC_AUTOCONF_FAN[128];
char MQTT_TOPIC_AUTOCONF_HUMIDIFIER[128];

bool shouldSaveConfig = false;

void saveConfigCallback () {
  shouldSaveConfig = true;
}

void setup() {
  Serial.begin(9600);

  //Initializing with defaults until we have a status update
  state.powerOn = false;
  state.fanSpeed = low;
  state.mode = setpoint;
  state.humiditySetpoint = 55;
  state.currentHumidity = 45;
  state.errorCode = 0;

  delay(3000);
  updateAndSendNetworkStatus(false);

  snprintf(identifier, sizeof(identifier), "dehumidifier_%X", ESP.getChipId());
  snprintf(MQTT_TOPIC_AVAILABILITY, 127, "%s/%s/LWT", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_STATE, 127, "%s/%s/STATE", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_COMMAND, 127, "%s/%s/cmnd", FIRMWARE_PREFIX, identifier);

  snprintf(MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR, 127, "homeassistant/sensor/%s/%s_humidity/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, 127, "homeassistant/sensor/%s/%s_wifi/config", FIRMWARE_PREFIX, identifier);

  snprintf(MQTT_TOPIC_AUTOCONF_FAN, 127, "homeassistant/select/%s/%s_fan/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_HUMIDIFIER, 127, "homeassistant/humidifier/%s/%s_dehumidifier/config", FIRMWARE_PREFIX, identifier);

  WiFi.hostname(identifier);

  loadConfig();

  setupWifi();
  setupWebServer();
  setupOTA();

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setKeepAlive(10);
  mqttClient.setBufferSize(2048);
  mqttClient.setCallback(mqttCallback);

  mqttReconnect();
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, []() {
    String html;
    html.reserve(2500);

    html += F("<!DOCTYPE html><html><head>");
    html += F("<meta charset='utf-8'>");
    html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<title>Midea Dehumidifier</title>");
    html += F("<style>");
    html += F("body{font-family:Arial,sans-serif;max-width:600px;margin:30px auto;padding:0 15px;}");
    html += F("input{width:100%;box-sizing:border-box;padding:8px;margin:5px 0 15px;}");
    html += F("button{padding:10px 20px;}");
    html += F(".status{background:#eee;padding:15px;margin-bottom:20px;}");
    html += F("</style></head><body>");

    html += F("<h2>Midea Dehumidifier</h2>");

    html += F("<div class='status'>");
    html += F("<b>Hostname:</b> ");
    html += identifier;
    html += F("<br><b>Firmware:</b> ");
    html += FIRMWARE_VERSION;
    html += F("<br><b>WLAN:</b> ");
    html += WiFi.SSID();
    html += F("<br><b>IP:</b> ");
    html += WiFi.localIP().toString();
    html += F("<br><b>RSSI:</b> ");
    html += String(WiFi.RSSI());
    html += F(" dBm<br><b>MQTT:</b> ");
    html += mqttClient.connected() ? F("verbunden") : F("nicht verbunden");
    html += F("</div>");

    html += F("<form method='POST' action='/save'>");

    html += F("<label>MQTT Server</label>");
    html += F("<input name='mqtt_server' value='");
    html += mqtt_server;
    html += F("' maxlength='79'>");

    html += F("<label>MQTT Benutzername</label>");
    html += F("<input name='username' value='");
    html += username;
    html += F("' maxlength='23'>");

    html += F("<label>MQTT Passwort</label>");
    html += F("<input type='password' name='password' value='' maxlength='23' "
              "placeholder='Leer lassen = Passwort beibehalten'>");

    html += F("<button type='submit'>Speichern</button>");
    html += F("</form>");

    html += F("</body></html>");

    webServer.send(200, "text/html; charset=utf-8", html);
  });

  webServer.on("/save", HTTP_POST, []() {
    if (!webServer.hasArg("mqtt_server")) {
      webServer.send(400, "text/plain", "MQTT Server fehlt");
      return;
    }

    webServer.arg("mqtt_server").toCharArray(mqtt_server, sizeof(mqtt_server));
    webServer.arg("username").toCharArray(username, sizeof(username));

    if (webServer.hasArg("password") && webServer.arg("password").length() > 0) {
      webServer.arg("password").toCharArray(password, sizeof(password));
    }

    saveConfig();

    webServer.send(
      200,
      "text/html; charset=utf-8",
      "<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
      "<body><h2>Gespeichert</h2>"
      "<p>Das Gerät wird neu gestartet...</p></body></html>"
    );

    delay(1000);
    ESP.restart();
  });

  webServer.begin();
}

void setupOTA() {
  ArduinoOTA.setHostname(identifier);

  // Schutz vor versehentlichem OTA-Flashen
  ArduinoOTA.setPassword(identifier);

  ArduinoOTA.begin();
}

void loop() {
  webServer.handleClient();
  ArduinoOTA.handle();
  handleUart();
  mqttClient.loop();

  if (statusPollInterval <= (millis() - statusPollPreviousMillis)) {
    statusPollPreviousMillis = millis();
    getStatus();
  }

  if (!mqttClient.connected() && (mqttConnectionInterval <= (millis() - lastMqttConnectionAttempt)) )  {
    lastMqttConnectionAttempt = millis();
    mqttReconnect();
  }
}

void setupWifi() {
  wifiManager.setDebugOutput(false);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Die Parameter erst jetzt erzeugen.
  // loadConfig() wurde in setup() bereits vorher ausgeführt,
  // daher enthalten mqtt_server, username und password
  // hier bereits die gespeicherten Werte.
  WiFiManagerParameter custom_mqtt_server(
    "server",
    "mqtt server",
    mqtt_server,
    sizeof(mqtt_server)
  );

  WiFiManagerParameter custom_mqtt_user(
    "user",
    "MQTT username",
    username,
    sizeof(username)
  );

  WiFiManagerParameter custom_mqtt_pass(
    "pass",
    "MQTT password",
    password,
    sizeof(password)
  );

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);

WiFi.hostname(identifier);

// Konfigurationsportal maximal 5 Minuten geöffnet lassen.
// Dadurch bleibt der Fallback-AP nach einem Stromausfall
// nicht dauerhaft aktiv.
wifiManager.setConfigPortalTimeout(300);

if (!wifiManager.autoConnect(identifier)) {
  ESP.restart();
}

mqttClient.setClient(wifiClient);

  // Nur wenn das Konfigurationsportal tatsächlich neue Werte
  // gespeichert hat, übernehmen und speichern wir diese.
  if (shouldSaveConfig) {
    strlcpy(
      mqtt_server,
      custom_mqtt_server.getValue(),
      sizeof(mqtt_server)
    );

    strlcpy(
      username,
      custom_mqtt_user.getValue(),
      sizeof(username)
    );

    strlcpy(
      password,
      custom_mqtt_pass.getValue(),
      sizeof(password)
    );

    saveConfig();
  }
}

void resetWifiSettingsAndReboot() {
  wifiManager.resetSettings();
  updateAndSendNetworkStatus(false);
  delay(3000);
  ESP.restart();
}

void mqttReconnect() {
  if (mqttClient.connect(
        identifier,
        username,
        password,
        MQTT_TOPIC_AVAILABILITY,
        1,
        true,
        AVAILABILITY_OFFLINE
      )) {

    mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_ONLINE, true);
    updateAndSendNetworkStatus(true);
    getStatus();
    publishAutoConfig();

    // Subscribe only after polling the current appliance status.
    mqttClient.subscribe(MQTT_TOPIC_COMMAND);
  }
}

boolean isMqttConnected() {
  return mqttClient.connected();
}

String getUptimeString(unsigned long uptimeSec) {
  unsigned long days = uptimeSec / 86400UL;
  unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
  unsigned long minutes = (uptimeSec % 3600UL) / 60UL;
  unsigned long seconds = uptimeSec % 60UL;

  char uptime[24];

  snprintf(
    uptime,
    sizeof(uptime),
    "%luT%02lu:%02lu:%02lu",
    days,
    hours,
    minutes,
    seconds
  );

  return String(uptime);
}

void publishState() {
  DynamicJsonDocument stateJson(1024);
  char payload[768];

  unsigned long uptimeSec = millis() / 1000UL;

  stateJson["state"] = state.powerOn ? "on" : "off";
  stateJson["humiditySetpoint"] = state.humiditySetpoint;
  stateJson["humidityCurrent"] = state.currentHumidity;
  stateJson["errorCode"] = state.errorCode;

  switch (state.fanSpeed) {
    case low:
      stateJson["fanSpeed"] = "low";
      break;

    case medium:
      stateJson["fanSpeed"] = "medium";
      break;

    case high:
      stateJson["fanSpeed"] = "high";
      break;
  }

  switch (state.mode) {
    case setpoint:
      stateJson["mode"] = "setpoint";
      break;

    case continuous:
      stateJson["mode"] = "continuous";
      break;

    case smart:
      stateJson["mode"] = "smart";
      break;

    case clothesDrying:
      stateJson["mode"] = "clothesDrying";
      break;
  }

  // Systeminformationen im Tasmota-Stil
  stateJson["IPAddress"] = WiFi.localIP().toString();
  stateJson["Wifi_RSSI"] = WiFi.RSSI();
  stateJson["Wifi_SSId"] = WiFi.SSID();
  stateJson["Version"] = FIRMWARE_VERSION;
  stateJson["UptimeSec"] = uptimeSec;
  stateJson["Uptime"] = getUptimeString(uptimeSec);

  serializeJson(stateJson, payload, sizeof(payload));
  mqttClient.publish(MQTT_TOPIC_STATE, payload, true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_COMMAND) == 0) {
    DynamicJsonDocument commandJson(256);

    DeserializationError err = deserializeJson(commandJson, payload, length);

    if (!err) {
      handleStateUpdateRequest(
        commandJson["state"].as<String>(),
        commandJson["mode"].as<String>(),
        commandJson["fanSpeed"].as<String>(),
        commandJson["humiditySetpoint"].as<byte>()
      );

      getStatus();
    }
  }
}

void publishAutoConfig() {
  char mqttPayload[2048];

  DynamicJsonDocument device(256);
  DynamicJsonDocument autoconfPayload(1024);

  StaticJsonDocument<64> identifiersDoc;
  JsonArray identifiers = identifiersDoc.to<JsonArray>();
  identifiers.add(identifier);

  // Gemeinsame Geräteinformationen
  device["identifiers"] = identifiers;
  device["manufacturer"] = "Midea Group Co., Ltd.";
  device["model"] = "Generic Dehumidifier";
  device["name"] = identifier;
  device["sw_version"] = FIRMWARE_VERSION;


  // ---------------------------------------------------------
  // WiFi-Signal
  // ---------------------------------------------------------

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["name"] = "WiFi";
  autoconfPayload["value_template"] = "{{value_json.Wifi_RSSI}}";
  autoconfPayload["unique_id"] = identifier + String("_wifi");
  autoconfPayload["unit_of_measurement"] = "dBm";
  autoconfPayload["json_attributes_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["json_attributes_template"] =
    "{\"ssid\": \"{{value_json.Wifi_SSId}}\", \"ip\": \"{{value_json.IPAddress}}\"}";
  autoconfPayload["icon"] = "mdi:wifi";

  serializeJson(autoconfPayload, mqttPayload, sizeof(mqttPayload));
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, mqttPayload, true);

  autoconfPayload.clear();


  // ---------------------------------------------------------
  // Luftfeuchtigkeit
  // ---------------------------------------------------------

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["name"] = "Humidity";
  autoconfPayload["device_class"] = "humidity";
  autoconfPayload["unit_of_measurement"] = "%";
  autoconfPayload["value_template"] = "{{value_json.humidityCurrent}}";
  autoconfPayload["unique_id"] = identifier + String("_humidity");

  serializeJson(autoconfPayload, mqttPayload, sizeof(mqttPayload));
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR, mqttPayload, true);

  autoconfPayload.clear();


  // ---------------------------------------------------------
  // Lüfterstufe
  // ---------------------------------------------------------

  StaticJsonDocument<64> optionsDoc;
  JsonArray speedOptions = optionsDoc.to<JsonArray>();

  speedOptions.add("low");
  speedOptions.add("medium");
  speedOptions.add("high");

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["name"] = "Fan";
  autoconfPayload["unique_id"] = identifier + String("_fan");
  autoconfPayload["icon"] = "mdi:fan";

  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["options"] = speedOptions;
  autoconfPayload["value_template"] = "{{value_json.fanSpeed}}";
  autoconfPayload["command_template"] = "{\"fanSpeed\": \"{{value}}\"}";

  serializeJson(autoconfPayload, mqttPayload, sizeof(mqttPayload));
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_FAN, mqttPayload, true);

  autoconfPayload.clear();


  // ---------------------------------------------------------
  // Entfeuchter
  // ---------------------------------------------------------

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["name"] = "Dehumidifier";
  autoconfPayload["unique_id"] = identifier + String("_dehumidifier");
  autoconfPayload["device_class"] = "dehumidifier";

  autoconfPayload["min_humidity"] = 35;
  autoconfPayload["max_humidity"] = 85;

  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["state_value_template"] =
    "{\"state\": \"{{value_json.state}}\"}";
  autoconfPayload["payload_on"] = "{\"state\": \"on\"}";
  autoconfPayload["payload_off"] = "{\"state\": \"off\"}";

  autoconfPayload["target_humidity_state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["target_humidity_command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["target_humidity_state_template"] =
    "{{value_json.humiditySetpoint | int}}";
  autoconfPayload["target_humidity_command_template"] =
    "{\"humiditySetpoint\": {{value | int}}}";

  StaticJsonDocument<64> modesDoc;
  JsonArray modes = modesDoc.to<JsonArray>();

  modes.add("setpoint");
  modes.add("continuous");
  modes.add("smart");
  modes.add("clothesDrying");

  autoconfPayload["modes"] = modes;
  autoconfPayload["mode_state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["mode_command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["mode_state_template"] = "{{value_json.mode}}";
  autoconfPayload["mode_command_template"] =
    "{\"mode\": \"{{value}}\"}";

  serializeJson(autoconfPayload, mqttPayload, sizeof(mqttPayload));
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_HUMIDIFIER, mqttPayload, true);
}
