#include "MqttHandler.h"

static PubSubClient mqttClient;
static MqttConfig mqttCfg;
static AcStateChangedCallback stateChangedCb = nullptr;

static String topicBaseSet;
static String topicBaseState;

// forward
static void mqttCallback(char *topic, byte *payload, unsigned int length);
static void ensureMqttConnected();

void MqttHandler_init(Client &netClient, AcStateChangedCallback cb) {
  mqttClient.setClient(netClient);
  mqttClient.setCallback(mqttCallback);
  stateChangedCb = cb;

  mqttCfg.enabled   = false;
  mqttCfg.host      = "";
  mqttCfg.port      = 1883;
  mqttCfg.clientId  = "esp-toshiba-ir";
  mqttCfg.baseTopic = "toshiba/ac";
  mqttCfg.user      = "";
  mqttCfg.pass      = "";

  topicBaseSet   = mqttCfg.baseTopic + "/set/";
  topicBaseState = mqttCfg.baseTopic + "/state/";
}

void MqttHandler_setConfig(const MqttConfig &cfg) {
  mqttCfg = cfg;

  topicBaseSet   = mqttCfg.baseTopic + "/set/";
  topicBaseState = mqttCfg.baseTopic + "/state/";

  if (mqttCfg.enabled && mqttCfg.host.length()) {
    mqttClient.setServer(mqttCfg.host.c_str(), mqttCfg.port);
  }
}

const MqttConfig &MqttHandler_getConfig() {
  return mqttCfg;
}

void MqttHandler_loop() {
  if (!mqttCfg.enabled || !mqttCfg.host.length()) return;

  ensureMqttConnected();
  mqttClient.loop();
}

static void ensureMqttConnected() {
  if (mqttClient.connected()) return;

  Serial.print(F("[MQTT] Connecting to "));
  Serial.print(mqttCfg.host);
  Serial.print(F(":"));
  Serial.print(mqttCfg.port);
  Serial.print(F(" ... "));

  String cid = mqttCfg.clientId;
  if (!cid.length()) cid = "esp-toshiba-ir";

  bool ok;
  if (mqttCfg.user.length()) {
    ok = mqttClient.connect(cid.c_str(),
                            mqttCfg.user.c_str(),
                            mqttCfg.pass.c_str());
  } else {
    ok = mqttClient.connect(cid.c_str());
  }

  if (ok) {
    Serial.println(F("OK"));

    String sub = topicBaseSet + "+";
    mqttClient.subscribe(sub.c_str());

    // po reconnectu můžeme znovu publikovat stav
    MqttHandler_publishState();
  } else {
    Serial.print(F("FAIL rc="));
    Serial.println(mqttClient.state());
  }
}

static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String t(topic);
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.print(F("[MQTT] "));
  Serial.print(t);
  Serial.print(F(" = "));
  Serial.println(msg);

  if (!t.startsWith(topicBaseSet)) return;

  String key = t.substring(topicBaseSet.length());
  bool changed = false;

  if (key == "power") {
    if (msg == "on" || msg == "off") {
      gAcState.power = msg;
      changed = true;
    }
  } else if (key == "mode") {
    gAcState.mode = msg;
    changed = true;
  } else if (key == "temp") {
    int v = msg.toInt();
    if (v < 17) v = 17;
    if (v > 30) v = 30;
    gAcState.temp = (uint8_t)v;
    changed = true;
  } else if (key == "fan") {
    gAcState.fan = msg;
    changed = true;
  } else if (key == "swing") {
    gAcState.swing = msg;
    changed = true;
  } else if (key == "pure") {
    gAcState.pure = msg;
    changed = true;
  } else if (key == "pselect") {
    gAcState.powerSelect = msg;
    changed = true;
  }

  if (changed && stateChangedCb) {
    stateChangedCb();   // → main: IRControl_sendFromState + publishState + update Modbus
  }
}

void MqttHandler_publishState() {
  if (!mqttCfg.enabled || !mqttCfg.host.length()) return;
  if (!mqttClient.connected()) return;

  auto pub = [&](const String &sub, const String &val) {
    String topic = topicBaseState + sub;
    mqttClient.publish(topic.c_str(), val.c_str(), true); // retained
  };

  pub("power", gAcState.power);
  pub("mode", gAcState.mode);
  pub("temp", String(gAcState.temp));
  pub("fan", gAcState.fan);
  pub("swing", gAcState.swing);
  pub("pure", gAcState.pure);
  pub("pselect", gAcState.powerSelect);
}
