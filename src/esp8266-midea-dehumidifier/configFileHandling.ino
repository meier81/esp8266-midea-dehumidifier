void saveConfig() {
  DynamicJsonDocument json(512);
  json["mqtt_server"] = mqtt_server;
  json["username"] = username;
  json["password"] = password;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    return;
  }

  serializeJson(json, configFile);
  configFile.close();
}

void loadConfig() {
  if (!SPIFFS.begin()) {
    return;
  }

  if (!SPIFFS.exists("/config.json")) {
    return;
  }

  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    return;
  }

  DynamicJsonDocument json(512);
  DeserializationError error = deserializeJson(json, configFile);
  configFile.close();

  if (error) {
    return;
  }

  const char* savedMqttServer = json["mqtt_server"];
  const char* savedUsername   = json["username"];
  const char* savedPassword   = json["password"];

  if (savedMqttServer != nullptr) {
    strlcpy(mqtt_server, savedMqttServer, sizeof(mqtt_server));
  }

  if (savedUsername != nullptr) {
    strlcpy(username, savedUsername, sizeof(username));
  }

  if (savedPassword != nullptr) {
    strlcpy(password, savedPassword, sizeof(password));
  }
}
