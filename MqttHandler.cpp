#include "MqttHandler.h"
#include "Logging.h"
#include <PubSubClient.h>
#include <ctype.h>

static PubSubClient           mqtt;
static MqttConfig             gCfg;
static AcStateChangedCallback gStateChangedCb = nullptr;
static bool                   gInitialised    = false;

static String topicState() {
  return gCfg.baseTopic + "/" + gCfg.clientId + "/state";
}
static String topicCommand() {
  return gCfg.baseTopic + "/" + gCfg.clientId + "/set";
}

static bool ensureConnected() {
  if (!gCfg.enabled || !gInitialised) return false;
  if (mqtt.connected()) return true;

  Logging::logf(Logging::LEVEL_INFO, "MQTT", "Connecting to %s:%u ...",
                gCfg.host.c_str(), gCfg.port);

  bool ok;
  if (gCfg.user.length() > 0) {
    ok = mqtt.connect(gCfg.clientId.c_str(),
                      gCfg.user.c_str(), gCfg.pass.c_str());
  } else {
    ok = mqtt.connect(gCfg.clientId.c_str());
  }
  if (!ok) {
    Logging::logf(Logging::LEVEL_WARN, "MQTT", "Connect failed, rc=%d", mqtt.state());
    return false;
  }

  String cmd = topicCommand();
  mqtt.subscribe(cmd.c_str());
  Logging::logf(Logging::LEVEL_INFO, "MQTT", "Connected, subscribed to %s", cmd.c_str());

  return true;
}

static void handleMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    msg += static_cast<char>(payload[i]);
  }
  Logging::logf(Logging::LEVEL_INFO, "MQTT", "RX topic=%s payload=%s", topic, msg.c_str());

  bool changed = false;
  String lower = msg;
  lower.trim();
  lower.toLowerCase();

  // Jednoduché příkazy: "on"/"off"
  if (lower == "on" || lower == "off") {
    gAcState.power = lower;
    changed = true;
  } else {
    // Hledání podřetězců v JSON-like payloadu
    if (lower.indexOf("power\":\"on\"")  >= 0) { gAcState.power = "on";  changed = true; }
    if (lower.indexOf("power\":\"off\"") >= 0) { gAcState.power = "off"; changed = true; }

    if (lower.indexOf("mode\":\"cool\"")     >= 0) { gAcState.mode = "cool";     changed = true; }
    if (lower.indexOf("mode\":\"heat\"")     >= 0) { gAcState.mode = "heat";     changed = true; }
    if (lower.indexOf("mode\":\"dry\"")      >= 0) { gAcState.mode = "dry";      changed = true; }
    if (lower.indexOf("mode\":\"fan_only\"") >= 0) { gAcState.mode = "fan_only"; changed = true; }

    int ti = lower.indexOf("\"temp\":");
    if (ti >= 0) {
      ti += 7;
      int end = ti;
      while (end < (int)lower.length() && isDigit(lower[end])) end++;
      if (end > ti) {
        uint8_t tval = (uint8_t) lower.substring(ti, end).toInt();
        gAcState.temp = tval;
        changed = true;
      }
    }
  }

  if (changed) {
    AcState_normalize();
    if (gStateChangedCb) gStateChangedCb();
  }
}

void MqttHandler_init(Client &netClient, const MqttConfig &cfg, AcStateChangedCallback cb) {
  gCfg = cfg;
  gStateChangedCb = cb;
  mqtt.setClient(netClient);
  mqtt.setServer(cfg.host.c_str(), cfg.port);
  mqtt.setCallback(handleMessage);
  gInitialised = true;
}

void MqttHandler_loop() {
  if (!gCfg.enabled || !gInitialised) return;

  if (!mqtt.connected()) {
    static unsigned long lastAttempt = 0;
    unsigned long now = millis();
    if (now - lastAttempt > 5000) {
      lastAttempt = now;
      ensureConnected();
    }
  } else {
    mqtt.loop();
  }
}

void MqttHandler_publishState() {
  if (!ensureConnected()) return;

  String t = topicState();
  String payload;
  payload.reserve(160);
  payload += F("{\"power\":\"");        payload += gAcState.power;       payload += F("\",");
  payload += F("\"temp\":");            payload += gAcState.temp;        payload += F(",");
  payload += F("\"mode\":\"");          payload += gAcState.mode;        payload += F("\",");
  payload += F("\"fan\":\"");           payload += gAcState.fan;         payload += F("\",");
  payload += F("\"swing\":\"");         payload += gAcState.swing;       payload += F("\",");
  payload += F("\"pure\":\"");          payload += gAcState.pure;        payload += F("\",");
  payload += F("\"powerSelect\":\"");   payload += gAcState.powerSelect; payload += F("\"}");
  mqtt.publish(t.c_str(), payload.c_str(), true);
  Logging::logf(Logging::LEVEL_DEBUG, "MQTT", "Published state to %s: %s", t.c_str(), payload.c_str());
}

MqttConfig MqttHandler_getConfig() {
  return gCfg;
}

void MqttHandler_setConfig(const MqttConfig &cfg) {
  gCfg = cfg;
  mqtt.setServer(cfg.host.c_str(), cfg.port);
  // při změně configu odpojíme, další loop() se připojí s novým nastavením
  if (mqtt.connected()) {
    mqtt.disconnect();
  }
}
