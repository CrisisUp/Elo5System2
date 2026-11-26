#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <time.h>
#include <math.h>

#ifndef WOKWI_NO_RC522
#include <MFRC522.h>
#endif

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.h.example"
#endif

// ============================================================================
// 1. CONFIGURAÇÃO DE REDE, MQTT E HARDWARE
// ============================================================================
static const char* WIFI_SSID     = SECRET_WIFI_SSID;
static const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

static const char* MQTT_BROKER   = SECRET_MQTT_BROKER;
static const uint16_t MQTT_PORT  = SECRET_MQTT_PORT;
static const char* MQTT_USER     = SECRET_MQTT_USERNAME;
static const char* MQTT_PASS     = SECRET_MQTT_PASSWORD;
static const char* DEVICE_ID     = "esp32-cpd-01";
static const char* TOPIC_STATUS  = "elo5/sistema/status";

// Pinos (GPIO 34 no ECHO evita conflito com VSPI SCK no GPIO 18)
static constexpr uint8_t PIN_PIR          = 27;
static constexpr uint8_t PIN_REED_DOOR    = 26; // INPUT_PULLUP: LOW = fechada, HIGH = aberta
static constexpr uint8_t PIN_EXIT_BTN     = 25; // INPUT_PULLUP: LOW = pressionado
static constexpr uint8_t PIN_NFC_ROOM_BTN = 32; // INPUT_PULLUP: LOW = simula NFC SALA
static constexpr uint8_t PIN_NFC_RACK_BTN = 33; // INPUT_PULLUP: LOW = simula NFC RACK
static constexpr uint8_t PIN_TRIG         = 5;  // HC-SR04 TRIG
static constexpr uint8_t PIN_ECHO         = 34; // HC-SR04 ECHO (input-only, sem conflito SPI)
static constexpr uint8_t PIN_BUZZER       = 14; // Alarme/feedback local

// RC522 SPI (VSPI: SCK=18, MISO=19, MOSI=23, SS=21, RST=22)
static constexpr uint8_t PIN_RFID_SS      = 21;
static constexpr uint8_t PIN_RFID_RST     = 22;

static const char* SIM_ROOM_NFC_UID = "04A1B2C3D4";
static const char* SIM_RACK_NFC_UID = "04A1B2C3D4";

static constexpr float         PROX_MIN_DELTA_CM     = 3.0f;
static constexpr unsigned long PROX_MIN_INTERVAL_MS  = 5000UL;
static constexpr unsigned long PROX_SAMPLE_PERIOD_MS = 500UL;
static constexpr unsigned long DEBOUNCE_MS           = 120UL;
static constexpr unsigned long ALARM_REPEAT_MS       = 5000UL;

static const char* ALLOWED_ROOM_UIDS[] = {"04A1B2C3D4", "0411223344"};
static const char* ALLOWED_RACK_UIDS[] = {"04A1B2C3D4"};

enum class SystemState {
  IDLE_WAIT_ROOM_NFC,
  ROOM_ACTIVE_MONITOR,
  RACK_UNLOCKED_DOOR_MONITOR,
  EXIT_ALARM_DOOR_OPEN
};

struct SessionContext {
  String sessionId;
  String lastRoomUid;
  String rackOperatorUid;
  int    occupancyCount = 0;
  bool   pirMotion = false;
  bool   rackDoorOpen = false;
  bool   alarmDoorOpen = false;
  float  lastDistanceCm = -1.0f;
  float  lastLoggedDistanceCm = -1.0f;
  unsigned long lastProxLogMs = 0;
  unsigned long lastProxSampleMs = 0;
  unsigned long lastAlarmLogMs = 0;
  unsigned long eventSeq = 0;
};

WiFiClient plainWifiClient;
WiFiClientSecure secureWifiClient;
PubSubClient mqttClient(SECRET_MQTT_TLS_ENABLED ? (Client&)secureWifiClient : (Client&)plainWifiClient);

#ifndef WOKWI_NO_RC522
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
#endif

SystemState currentState = SystemState::IDLE_WAIT_ROOM_NFC;
SessionContext ctx;

bool lastRawDoorOpen = false;
unsigned long lastDoorDebounceMs = 0;
bool lastRawExitPressed = false;
unsigned long lastExitDebounceMs = 0;
bool lastRawRoomBtnPressed = false;
unsigned long lastRoomBtnDebounceMs = 0;
bool lastRawRackBtnPressed = false;
unsigned long lastRackBtnDebounceMs = 0;

String nextEventId() {
  ctx.eventSeq++;
  return String("evt_") + String(millis()) + "_" + String((uint32_t)esp_random(), HEX) + "_" + String(ctx.eventSeq);
}

String makeTimestampIso() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    char buf[40];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buf);
  }
  unsigned long sec = millis() / 1000UL;
  unsigned long ms  = millis() % 1000UL;
  char buf[48];
  snprintf(buf, sizeof(buf), "1970-01-01T00:00:%02lu.%03luZ", sec % 60, ms);
  return String(buf);
}

void generateSessionId() {
  ctx.sessionId = String("sess_") + String(millis()) + "_" + String((uint16_t)esp_random(), HEX);
}

bool uidInList(const String& uid, const char* const* list, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (uid.equalsIgnoreCase(list[i])) return true;
  }
  return false;
}

String readPhysicalNfcUidHex() {
#ifdef WOKWI_NO_RC522
  return "";
#else
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return "";
  }
  String uid;
  uid.reserve(rfid.uid.size * 2);
  for (byte i = 0; i < rfid.uid.size; ++i) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
#endif
}

float readUltrasonicCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long durationUs = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (durationUs <= 0) return -1.0f;
  float dist = (durationUs * 0.0343f) / 2.0f;
  if (dist < 2.0f || dist > 400.0f) return -1.0f;
  return roundf(dist * 10.0f) / 10.0f;
}

bool publishElo5Event(
    const char* topic,
    const char* stage,
    const char* eventType,
    const char* zone,
    const char* severity,
    const char* readerOrNull,
    const char* sensorOrNull,
    const char* uidNfc,
    const char* uidOperador,
    const char* uidSaindo,
    JsonDocument& dataDoc,
    uint8_t qos = 1,
    bool retained = false) {
  if (!mqttClient.connected()) return false;

  JsonDocument doc;
  doc["event_id"]  = nextEventId();
  doc["timestamp"] = makeTimestampIso();

  JsonObject src = doc["source"].to<JsonObject>();
  src["device_id"] = DEVICE_ID;
  if (readerOrNull) src["reader"] = readerOrNull;
  if (sensorOrNull) src["sensor"] = sensorOrNull;
  src["zone"] = zone;

  doc["stage"]      = stage;
  doc["event_type"] = eventType;
  doc["severity"]   = severity;

  JsonObject sess = doc["session"].to<JsonObject>();
  sess["session_id"]       = ctx.sessionId.length() ? ctx.sessionId : nullptr;
  sess["uid_nfc"]          = (uidNfc && strlen(uidNfc) > 0) ? uidNfc : nullptr;
  sess["uid_nfc_operador"] = (uidOperador && strlen(uidOperador) > 0) ? uidOperador : nullptr;
  sess["uid_nfc_saindo"]   = (uidSaindo && strlen(uidSaindo) > 0) ? uidSaindo : nullptr;
  sess["occupancy_count"]  = ctx.occupancyCount;

  doc["data"] = dataDoc;

  String out;
  serializeJson(doc, out);

  (void)qos;
  bool ok = mqttClient.publish(topic, out.c_str(), retained);
  Serial.printf("[MQTT %s] %s -> %s\n", ok ? "OK" : "ERR", topic, out.c_str());
  return ok;
}

void publishSystemStatus(const char* status) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["status"]    = status;
  doc["uptime_ms"] = millis();
  String out;
  serializeJson(doc, out);
  mqttClient.publish(TOPIC_STATUS, out.c_str(), true);
}

void syncTimeNtp() {
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  unsigned long start = millis();
  while (!getLocalTime(&timeinfo) && (millis() - start) < 5000UL) {
    delay(200);
  }
}

void configureTlsClient() {
  if (!SECRET_MQTT_TLS_ENABLED) return;
  if (SECRET_MQTT_TLS_INSECURE) {
    secureWifiClient.setInsecure();
    return;
  }
  secureWifiClient.setCACert(SECRET_CA_CERT_PEM);
  if (SECRET_MQTT_MTLS_ENABLED) {
    secureWifiClient.setCertificate(SECRET_CLIENT_CERT_PEM);
    secureWifiClient.setPrivateKey(SECRET_CLIENT_KEY_PEM);
  }
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000UL) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " conectado!" : " falha");
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  configureTlsClient();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(30);
  mqttClient.setBufferSize(1024);

  String clientId = String(DEVICE_ID) + "-" + String((uint16_t)esp_random(), HEX);
  const char* willPayload = "{\"device_id\":\"esp32-cpd-01\",\"status\":\"offline\"}";

  bool ok = mqttClient.connect(
      clientId.c_str(),
      MQTT_USER,
      MQTT_PASS,
      TOPIC_STATUS,
      1,
      true,
      willPayload);

  if (ok) {
    Serial.println("[MQTT] Conectado!");
    publishSystemStatus("online");
  } else {
    Serial.printf("[MQTT] Falhou (rc=%d)\n", mqttClient.state());
  }
}

void processRoomNfcUid(const String& uid) {
  if (uid.isEmpty()) return;
  bool allowed = uidInList(uid, ALLOWED_ROOM_UIDS, sizeof(ALLOWED_ROOM_UIDS) / sizeof(ALLOWED_ROOM_UIDS[0]));

  JsonDocument data;
  data["card_detected"] = true;
  data["authorized"]    = allowed;
  data["decision"]      = allowed ? "PERMITIDO" : "NEGADO";
  data["reason"]        = allowed ? "CREDENTIAL_VALID" : "SCOPE_NOT_ALLOWED";

  if (allowed) {
    if (ctx.sessionId.isEmpty()) generateSessionId();
    ctx.lastRoomUid = uid;
    ctx.occupancyCount += 1;
    data["next_state"] = "ROOM_ACTIVE_MONITOR";

    publishElo5Event(
        "elo5/sala/acesso/nfc/evento",
        "1_acesso_sala",
        "NFC_ACCESS_EVALUATED",
        "sala_cpd",
        "INFO",
        "nfc_sala_entrada",
        nullptr,
        uid.c_str(),
        nullptr,
        nullptr,
        data,
        1,
        false);
    currentState = SystemState::ROOM_ACTIVE_MONITOR;
  } else {
    data["next_state"] = "IDLE_WAIT_ROOM_NFC";
    digitalWrite(PIN_BUZZER, HIGH);
    delay(150);
    digitalWrite(PIN_BUZZER, LOW);

    publishElo5Event(
        "elo5/sala/acesso/nfc/evento",
        "1_acesso_sala",
        "NFC_ACCESS_EVALUATED",
        "sala_cpd",
        "WARN",
        "nfc_sala_entrada",
        nullptr,
        uid.c_str(),
        nullptr,
        nullptr,
        data,
        1,
        false);
  }
}

void processRackNfcUid(const String& uid) {
  if (uid.isEmpty()) return;
  bool allowed = uidInList(uid, ALLOWED_RACK_UIDS, sizeof(ALLOWED_RACK_UIDS) / sizeof(ALLOWED_RACK_UIDS[0]));

  JsonDocument data;
  data["card_detected"] = true;
  data["authorized"]    = allowed;
  data["decision"]      = allowed ? "LIBERADO" : "NEGADO";

  if (allowed) {
    ctx.rackOperatorUid = uid;
    publishElo5Event(
        "elo5/rack/acesso/nfc/evento",
        "2_ocupacao_e_rack",
        "RACK_ACCESS_EVALUATED",
        "rack_01",
        "INFO",
        "nfc_rack_01",
        nullptr,
        nullptr,
        ctx.rackOperatorUid.c_str(),
        nullptr,
        data,
        1,
        false);
    currentState = SystemState::RACK_UNLOCKED_DOOR_MONITOR;
  } else {
    publishElo5Event(
        "elo5/rack/acesso/nfc/evento",
        "2_ocupacao_e_rack",
        "RACK_ACCESS_EVALUATED",
        "rack_01",
        "WARN",
        "nfc_rack_01",
        nullptr,
        nullptr,
        uid.c_str(),
        nullptr,
        data,
        1,
        false);
  }
}

bool consumeButtonEdge(uint8_t pin, bool& lastRawPressed, unsigned long& lastDebounceMs) {
  unsigned long now = millis();
  bool rawPressed = (digitalRead(pin) == LOW);

  if (rawPressed != lastRawPressed) {
    lastRawPressed = rawPressed;
    lastDebounceMs = now;
  }

  if ((now - lastDebounceMs) >= DEBOUNCE_MS && rawPressed) {
    while (digitalRead(pin) == LOW) {
      mqttClient.loop();
      delay(10);
    }
    lastRawPressed = false;
    return true;
  }
  return false;
}

void handleSimNfcButtons() {
  if (consumeButtonEdge(PIN_NFC_ROOM_BTN, lastRawRoomBtnPressed, lastRoomBtnDebounceMs)) {
    processRoomNfcUid(SIM_ROOM_NFC_UID);
  }
  if (consumeButtonEdge(PIN_NFC_RACK_BTN, lastRawRackBtnPressed, lastRackBtnDebounceMs)) {
    processRackNfcUid(SIM_RACK_NFC_UID);
  }
}

void handleRoomNfcAccess() {
  String uid = readPhysicalNfcUidHex();
  if (!uid.isEmpty()) processRoomNfcUid(uid);
}

void handleRackNfcAccess() {
  String uid = readPhysicalNfcUidHex();
  if (!uid.isEmpty()) processRackNfcUid(uid);
}

void handlePirSensor() {
  bool motion = (digitalRead(PIN_PIR) == HIGH);
  if (motion == ctx.pirMotion) return;

  ctx.pirMotion = motion;
  JsonDocument data;
  data["pir_motion"]    = ctx.pirMotion;
  data["room_occupied"] = (ctx.occupancyCount > 0) || ctx.pirMotion;

  publishElo5Event(
      "elo5/sala/presenca/pir",
      "2_ocupacao_e_rack",
      "PIR_MOTION_CHANGED",
      "sala_cpd",
      "INFO",
      nullptr,
      "pir_sala_cpd",
      ctx.lastRoomUid.c_str(),
      nullptr,
      nullptr,
      data,
      1,
      false);
}

void handleRackDoorAndProximity() {
  bool rawDoorOpen = (digitalRead(PIN_REED_DOOR) == HIGH);
  unsigned long now = millis();

  if (rawDoorOpen != lastRawDoorOpen) {
    lastRawDoorOpen = rawDoorOpen;
    lastDoorDebounceMs = now;
  }

  if ((now - lastDoorDebounceMs) >= DEBOUNCE_MS && rawDoorOpen != ctx.rackDoorOpen) {
    ctx.rackDoorOpen = rawDoorOpen;
    if (!ctx.rackDoorOpen) {
      ctx.alarmDoorOpen = false;
      ctx.lastDistanceCm = -1.0f;
      ctx.lastLoggedDistanceCm = -1.0f;
      digitalWrite(PIN_BUZZER, LOW);
    }

    JsonDocument data;
    data["rack_door_open"]  = ctx.rackDoorOpen;
    data["alarm_door_open"] = ctx.alarmDoorOpen;
    data["state"]           = ctx.rackDoorOpen ? "ABERTA" : "FECHADA";

    publishElo5Event(
        "elo5/rack/porta/estado",
        "2_ocupacao_e_rack",
        "RACK_DOOR_CHANGED",
        "rack_01",
        ctx.rackDoorOpen ? "WARN" : "INFO",
        nullptr,
        "reed_switch_rack_01",
        nullptr,
        ctx.rackOperatorUid.c_str(),
        nullptr,
        data,
        1,
        true);
  }

  if (ctx.rackDoorOpen && (now - ctx.lastProxSampleMs) >= PROX_SAMPLE_PERIOD_MS) {
    ctx.lastProxSampleMs = now;
    float dist = readUltrasonicCm();
    if (dist > 0.0f) {
      ctx.lastDistanceCm = dist;
      float delta = (ctx.lastLoggedDistanceCm < 0.0f) ? 999.0f : fabsf(dist - ctx.lastLoggedDistanceCm);
      bool deltaOk    = (delta >= PROX_MIN_DELTA_CM);
      bool intervalOk = (now - ctx.lastProxLogMs) >= PROX_MIN_INTERVAL_MS;

      if (deltaOk || intervalOk) {
        ctx.lastLoggedDistanceCm = dist;
        ctx.lastProxLogMs = now;

        JsonDocument data;
        data["rack_door_open"] = true;
        data["distance_cm"]    = dist;
        data["delta_cm"]       = (delta > 500.0f) ? 0.0f : (roundf(delta * 10.0f) / 10.0f);
        data["trigger"]        = deltaOk ? "DELTA_THRESHOLD" : "INTERVAL_T";

        publishElo5Event(
            "elo5/rack/proximidade/cm",
            "2_ocupacao_e_rack",
            "RACK_PROXIMITY_MEASURED",
            "rack_01",
            "INFO",
            nullptr,
            "hc_sr04_rack_01",
            nullptr,
            ctx.rackOperatorUid.c_str(),
            nullptr,
            data,
            0,
            false);
      }
    }
  }
}

void performRoomResetIfEmpty() {
  if (ctx.occupancyCount <= 0) {
    ctx.occupancyCount = 0;
    ctx.pirMotion = false;
    ctx.rackOperatorUid = "";
    ctx.lastRoomUid = "";
    ctx.sessionId = "";
    currentState = SystemState::IDLE_WAIT_ROOM_NFC;

    JsonDocument data;
    data["rack_door_open"]       = ctx.rackDoorOpen;
    data["pir_motion"]           = false;
    data["room_reset_completed"] = true;

    publishElo5Event(
        "elo5/sala/saida/botao",
        "3_saida_e_reset",
        "SESSION_RESET_COMPLETED",
        "sala_cpd",
        "INFO",
        nullptr,
        "botao_saida_sala",
        nullptr,
        nullptr,
        nullptr,
        data,
        1,
        false);
  } else {
    currentState = SystemState::ROOM_ACTIVE_MONITOR;
  }
}

void handleExitButtonAndAlarm() {
  unsigned long now = millis();

  if (consumeButtonEdge(PIN_EXIT_BTN, lastRawExitPressed, lastExitDebounceMs)) {
    String exitingUid = ctx.lastRoomUid.length() ? ctx.lastRoomUid : "DESCONHECIDO";
    if (ctx.occupancyCount > 0) ctx.occupancyCount -= 1;

    bool doorStillOpen = (digitalRead(PIN_REED_DOOR) == HIGH) || ctx.rackDoorOpen;
    ctx.rackDoorOpen = doorStillOpen;

    JsonDocument data;
    data["exit_button_pressed"]  = true;
    data["rack_door_open"]       = doorStillOpen;
    data["alarm_door_open"]      = doorStillOpen;
    data["room_reset_completed"] = (!doorStillOpen && ctx.occupancyCount == 0);

    publishElo5Event(
        "elo5/sala/saida/botao",
        "3_saida_e_reset",
        doorStillOpen ? "RACK_DOOR_LEFT_OPEN_ON_EXIT" : "EXIT_BUTTON_PRESSED",
        doorStillOpen ? "rack_01" : "sala_cpd",
        doorStillOpen ? "ALERT" : "INFO",
        nullptr,
        "botao_saida_sala",
        nullptr,
        nullptr,
        exitingUid.c_str(),
        data,
        1,
        false);

    if (doorStillOpen) {
      ctx.alarmDoorOpen = true;
      ctx.lastAlarmLogMs = now;
      digitalWrite(PIN_BUZZER, HIGH);
      currentState = SystemState::EXIT_ALARM_DOOR_OPEN;
    } else {
      performRoomResetIfEmpty();
    }
    return;
  }

  if (currentState == SystemState::EXIT_ALARM_DOOR_OPEN) {
    bool doorOpenNow = (digitalRead(PIN_REED_DOOR) == HIGH);
    if (!doorOpenNow) {
      ctx.rackDoorOpen = false;
      ctx.alarmDoorOpen = false;
      digitalWrite(PIN_BUZZER, LOW);
      performRoomResetIfEmpty();
    } else if ((now - ctx.lastAlarmLogMs) >= ALARM_REPEAT_MS) {
      ctx.lastAlarmLogMs = now;
      JsonDocument data;
      data["rack_door_open"]  = true;
      data["alarm_door_open"] = true;
      data["alert_mode"]      = "PERIODIC_RE_ALERT";

      publishElo5Event(
          "elo5/rack/porta/estado",
          "3_saida_e_reset",
          "RACK_DOOR_OPEN_ALERT",
          "rack_01",
          "ALERT",
          nullptr,
          "reed_switch_rack_01",
          nullptr,
          ctx.rackOperatorUid.c_str(),
          nullptr,
          data,
          1,
          true);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_REED_DOOR, INPUT_PULLUP);
  pinMode(PIN_EXIT_BTN, INPUT_PULLUP);
  pinMode(PIN_NFC_ROOM_BTN, INPUT_PULLUP);
  pinMode(PIN_NFC_RACK_BTN, INPUT_PULLUP);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_BUZZER, LOW);

#ifndef WOKWI_NO_RC522
  SPI.begin();
  rfid.PCD_Init();
#endif

  lastRawDoorOpen = (digitalRead(PIN_REED_DOOR) == HIGH);
  lastRawExitPressed = (digitalRead(PIN_EXIT_BTN) == LOW);
  lastRawRoomBtnPressed = (digitalRead(PIN_NFC_ROOM_BTN) == LOW);
  lastRawRackBtnPressed = (digitalRead(PIN_NFC_RACK_BTN) == LOW);

  ensureWifi();
  syncTimeNtp();
  ensureMqtt();
}

void loop() {
  ensureWifi();
  ensureMqtt();
  mqttClient.loop();

  handleSimNfcButtons();

  switch (currentState) {
    case SystemState::IDLE_WAIT_ROOM_NFC:
      handleRoomNfcAccess();
      break;
    case SystemState::ROOM_ACTIVE_MONITOR:
      handlePirSensor();
      handleRackNfcAccess();
      handleRackDoorAndProximity();
      handleExitButtonAndAlarm();
      break;
    case SystemState::RACK_UNLOCKED_DOOR_MONITOR:
      handlePirSensor();
      handleRackDoorAndProximity();
      handleExitButtonAndAlarm();
      break;
    case SystemState::EXIT_ALARM_DOOR_OPEN:
      handleExitButtonAndAlarm();
      break;
  }

  delay(20);
}
