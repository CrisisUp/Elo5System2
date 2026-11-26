/**
 * @file porta_controller.cpp
 * @brief Controlador da Sala CPD (NODE_PORTA): NFC entrada, PIR, saída por botão e pânico.
 */

#if defined(NODE_PORTA)

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

#include "node_config.h"
#include "elo5_protocol.h"
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.h.example"
#endif

namespace {

MFRC522 rfid(Pins::RFID_SS, Pins::RFID_RST);
WiFiClientSecure tlsClient;
PubSubClient mqttClient(tlsClient);

uint8_t PEER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
volatile bool flagPanicoISR = false;

int    ocupacaoSala       = 0;
bool   pirAnterior        = false;
bool   botaoAnterior      = HIGH;
bool   alarmeRemotoRack   = false;
uint32_t seqEvento        = 0;
unsigned long bootMs      = 0;
unsigned long lastMqttTry = 0;

String sessionIdAtiva = "";
String uidAtivoSala   = "";

void IRAM_ATTR onPanicoISR() {
  flagPanicoISR = true;
}

String gerarTimestampIso() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    char buf[28];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buf);
  }
  char fallback[32];
  snprintf(fallback, sizeof(fallback), "2026-11-26T00:00:%02luZ", (millis() / 1000UL) % 60UL);
  return String(fallback);
}

String proximoEventId() {
  seqEvento++;
  return String(NodeIdentity::DEVICE_ID) + "_" + String(millis()) + "_" + String(seqEvento);
}

void setLedRgb(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(Pins::LED_R, 255 - r);
  ledcWrite(Pins::LED_G, 255 - g);
  ledcWrite(Pins::LED_B, 255 - b);
}

void enviarSyncEspNow(uint8_t tipo) {
  PacoteEspNow pkt = {};
  pkt.versao          = 1;
  pkt.tipo            = tipo;
  pkt.ocupacaoSala    = (uint8_t)ocupacaoSala;
  pkt.portaRackAberta = 0;
  pkt.alarmeAtivo     = alarmeRemotoRack ? 1 : 0;
  pkt.uptimeMs        = millis();
  strncpy(pkt.uidAtivo, uidAtivoSala.c_str(), sizeof(pkt.uidAtivo) - 1);
  strncpy(pkt.sessionId, sessionIdAtiva.c_str(), sizeof(pkt.sessionId) - 1);
  esp_now_send(PEER_MAC, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
}

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
  if (len == sizeof(PacoteEspNow)) {
    PacoteEspNow pkt;
    memcpy(&pkt, data, sizeof(pkt));
    alarmeRemotoRack = (pkt.alarmeAtivo != 0);
  }
}

void publicarEvento(const char* topic, const char* stage, const char* eventType,
                    const char* severity, const char* uidField, const String& uidVal,
                    JsonDocument& dataDoc) {
  if (!mqttClient.connected()) return;
  String json = buildElo5EventJson(
      proximoEventId(), gerarTimestampIso(), stage, eventType, severity,
      sessionIdAtiva, uidField, uidVal, ocupacaoSala, dataDoc);
  mqttClient.publish(topic, json.c_str(), false);
}

void garantirConexao() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);
    return;
  }
  if (!mqttClient.connected() && (millis() - lastMqttTry >= Timings::MQTT_RETRY_MS)) {
    lastMqttTry = millis();
    String cid = String("elo5-porta-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqttClient.connect(cid.c_str(), SECRET_MQTT_USERNAME, SECRET_MQTT_PASSWORD,
                             MqttTopics::DEVICE_LWT, 1, true, "OFFLINE")) {
      mqttClient.publish(MqttTopics::DEVICE_LWT, "ONLINE", true);
      Serial.println("[PORTA] Conectado ao broker MQTT mTLS (:8883)");
    }
  }
  mqttClient.loop();
}

void tratarLeituraNfcSala() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (uint8_t i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  uidAtivoSala   = uid;
  ocupacaoSala   = 1;
  sessionIdAtiva = "sess_" + uid + "_" + String(millis());

  digitalWrite(Pins::TRAVA_PORTA, HIGH);
  tone(Pins::BUZZER, 1800, 150);
  setLedRgb(0, 180, 180);

  JsonDocument data;
  data["uid_nfc"]    = uid;
  data["authorized"] = true;
  data["decision"]   = "PERMITIDO";
  publicarEvento(MqttTopics::SALA_NFC_EVENTO, "1_acesso_sala",
                 "NFC_ACCESS_EVALUATED", "INFO", "uid_nfc", uid, data);

  enviarSyncEspNow(ESPNOW_SALA_ACESSO);
  Serial.println("[PORTA] Acesso sala registrado UID=" + uid);
}

void tratarPirSala() {
  if (millis() - bootMs < Timings::WARMUP_PIR_MS) return;
  bool movimento = (digitalRead(Pins::PIR_SALA) == HIGH);
  if (movimento == pirAnterior) return;
  pirAnterior = movimento;

  JsonDocument data;
  data["pir_motion"] = movimento;
  publicarEvento(MqttTopics::SALA_PIR_PRESENCA, "2_ocupacao_e_rack",
                 "PIR_MOTION_CHANGED", "INFO", "uid_nfc", uidAtivoSala, data);
}

void tratarBotaoSaida() {
  bool estadoAtual = digitalRead(Pins::BOTAO_SAIDA);
  if (botaoAnterior == HIGH && estadoAtual == LOW) {
    delay(Timings::DEBOUNCE_BOTAO_MS);
    if (digitalRead(Pins::BOTAO_SAIDA) == LOW) {
      String uidSaindo = uidAtivoSala;
      ocupacaoSala = 0;
      digitalWrite(Pins::TRAVA_PORTA, LOW);

      JsonDocument dataExit;
      dataExit["exit_button_pressed"] = true;
      publicarEvento(MqttTopics::SALA_SAIDA_BOTAO, "3_saida_e_reset",
                     "EXIT_BUTTON_PRESSED", "INFO", "uid_nfc_saindo", uidSaindo, dataExit);

      JsonDocument dataReset;
      dataReset["pir_motion"]           = false;
      dataReset["room_reset_completed"] = true;
      publicarEvento(MqttTopics::SALA_SAIDA_BOTAO, "3_saida_e_reset",
                     "SESSION_RESET_COMPLETED", "INFO", "uid_nfc_saindo", uidSaindo, dataReset);

      enviarSyncEspNow(ESPNOW_SALA_SAIDA);
      uidAtivoSala   = "";
      sessionIdAtiva = "";
      Serial.println("[PORTA] Saida registrada e sessao encerrada.");
    }
  }
  botaoAnterior = estadoAtual;
}

} // namespace

void setupPortaController() {
  Serial.begin(115200);
  bootMs = millis();

  pinMode(Pins::PIR_SALA, INPUT);
  pinMode(Pins::BOTAO_SAIDA, INPUT_PULLUP);
  pinMode(Pins::BOTAO_PANICO, INPUT_PULLUP);
  pinMode(Pins::TRAVA_PORTA, OUTPUT);
  pinMode(Pins::BUZZER, OUTPUT);
  digitalWrite(Pins::TRAVA_PORTA, LOW);

  ledcAttach(Pins::LED_R, 5000, 8);
  ledcAttach(Pins::LED_G, 5000, 8);
  ledcAttach(Pins::LED_B, 5000, 8);
  setLedRgb(0, 120, 0);

  attachInterrupt(digitalPinToInterrupt(Pins::BOTAO_PANICO), onPanicoISR, FALLING);

  SPI.begin(Pins::RFID_SCK, Pins::RFID_MISO, Pins::RFID_MOSI, Pins::RFID_SS);
  rfid.PCD_Init();

  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEER_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }

  tlsClient.setCACert(SECRET_CA_CERT_PEM);
  tlsClient.setCertificate(SECRET_CLIENT_CERT_PEM);
  tlsClient.setPrivateKey(SECRET_CLIENT_KEY_PEM);

  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");

  mqttClient.setServer(SECRET_MQTT_BROKER, SECRET_MQTT_PORT);
  mqttClient.setBufferSize(1024);

  Serial.println("[BOOT] porta_controller pronto.");
}

void loopPortaController() {
  garantirConexao();
  tratarLeituraNfcSala();
  tratarPirSala();
  tratarBotaoSaida();

  if (flagPanicoISR) {
    flagPanicoISR = false;
    setLedRgb(255, 0, 0);
    tone(Pins::BUZZER, 2200, 1000);
    JsonDocument data;
    data["panic_button"] = true;
    publicarEvento(MqttTopics::SALA_SAIDA_BOTAO, "3_saida_e_reset",
                   "PANIC_BUTTON_TRIGGERED", "CRIT", "uid_nfc", uidAtivoSala, data);
  } else if (alarmeRemotoRack) {
    setLedRgb(255, 0, 0);
  } else if (ocupacaoSala > 0) {
    setLedRgb(0, 180, 180);
  } else {
    setLedRgb(0, 120, 0);
  }
}

#endif // NODE_PORTA
