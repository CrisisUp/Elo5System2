/**
 * @file rack_controller.cpp
 * @brief Controlador do Rack (NODE_RACK): NFC 2ª etapa, porta rack, ultrassônico, DHT22, OLED e servo.
 */

#if defined(NODE_RACK)

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_now.h>
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
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(Pins::DHT_PIN, DHT22);
WiFiClientSecure tlsClient;
PubSubClient mqttClient(tlsClient);

uint8_t PEER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint8_t  SERVO_CH       = 6;
constexpr uint32_t SERVO_MIN_DUTY = 1638;
constexpr uint32_t SERVO_MAX_DUTY = 7864;

int           ocupacaoSala      = 0;
bool          portaRackAberta   = false;
bool          portaRackAnterior = false;
bool          manutAutorizada   = false;
bool          alarmeRackAtivo   = false;
float         ultimaDistCm      = -1.0f;
float         ultimaTempC       = NAN;
float         ultimaUmidPct     = NAN;
uint32_t      seqEvento         = 0;
unsigned long inicioManutMs     = 0;
unsigned long ultimaProxMs      = 0;
unsigned long ultimoDhtMs       = 0;
unsigned long ultimoOledMs      = 0;
unsigned long lastMqttTry       = 0;

String sessionIdAtiva = "";
String uidOperador    = "";

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

void moverServo(uint8_t angulo) {
  if (angulo > 180) angulo = 180;
  uint32_t duty = SERVO_MIN_DUTY + ((uint32_t)angulo * (SERVO_MAX_DUTY - SERVO_MIN_DUTY)) / 180U;
  ledcWriteChannel(SERVO_CH, duty);
}

void enviarSyncEspNow(uint8_t tipo) {
  PacoteEspNow pkt = {};
  pkt.versao          = 1;
  pkt.tipo            = tipo;
  pkt.ocupacaoSala    = (uint8_t)ocupacaoSala;
  pkt.portaRackAberta = portaRackAberta ? 1 : 0;
  pkt.alarmeAtivo     = alarmeRackAtivo ? 1 : 0;
  pkt.uptimeMs        = millis();
  strncpy(pkt.uidAtivo, uidOperador.c_str(), sizeof(pkt.uidAtivo) - 1);
  strncpy(pkt.sessionId, sessionIdAtiva.c_str(), sizeof(pkt.sessionId) - 1);
  esp_now_send(PEER_MAC, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
}

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
  if (len != sizeof(PacoteEspNow)) return;
  PacoteEspNow pkt;
  memcpy(&pkt, data, sizeof(pkt));

  ocupacaoSala = pkt.ocupacaoSala;
  if (pkt.sessionId[0] != '\0') {
    sessionIdAtiva = String(pkt.sessionId);
  }
  if (pkt.uidAtivo[0] != '\0') {
    uidOperador = String(pkt.uidAtivo);
  }

  // Se operador saiu da sala e a porta do rack ainda está aberta, dispara alarme imediato
  if (pkt.tipo == ESPNOW_SALA_SAIDA && portaRackAberta) {
    alarmeRackAtivo = true;
  }
}

void publicarEvento(const char* topic, const char* stage, const char* eventType,
                    const char* severity, JsonDocument& dataDoc) {
  if (!mqttClient.connected()) return;
  String json = buildElo5EventJson(
      proximoEventId(), gerarTimestampIso(), stage, eventType, severity,
      sessionIdAtiva, "uid_nfc_operador", uidOperador, ocupacaoSala, dataDoc);
  mqttClient.publish(topic, json.c_str(), false);
}

void garantirConexao() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);
    return;
  }
  if (!mqttClient.connected() && (millis() - lastMqttTry >= Timings::MQTT_RETRY_MS)) {
    lastMqttTry = millis();
    String cid = String("elo5-rack-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqttClient.connect(cid.c_str(), SECRET_MQTT_USERNAME, SECRET_MQTT_PASSWORD,
                             MqttTopics::DEVICE_LWT, 1, true, "OFFLINE")) {
      mqttClient.publish(MqttTopics::DEVICE_LWT, "ONLINE", true);
      Serial.println("[RACK] Conectado ao broker MQTT mTLS (:8883)");
    }
  }
  mqttClient.loop();
}

void tratarNfcRack() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (uint8_t i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  uidOperador     = uid;
  manutAutorizada = true;
  inicioManutMs   = millis();
  moverServo(90);
  tone(Pins::BUZZER, 1800, 150);

  JsonDocument data;
  data["uid_nfc"]    = uid;
  data["authorized"] = true;
  data["decision"]   = "PERMITIDO";
  publicarEvento(MqttTopics::RACK_NFC_EVENTO, "2_ocupacao_e_rack",
                 "RACK_ACCESS_EVALUATED", "INFO", data);
  Serial.println("[RACK] 2o fator NFC autorizado UID=" + uid);
}

void tratarPortaRack() {
  portaRackAberta = (digitalRead(Pins::PORTA_RACK_SW) == HIGH);

  if (manutAutorizada && !portaRackAberta &&
      (millis() - inicioManutMs >= Timings::JANELA_MANUT_RACK_MS)) {
    manutAutorizada = false;
    moverServo(0);
  }

  if (portaRackAberta == portaRackAnterior) return;
  portaRackAnterior = portaRackAberta;

  if (portaRackAberta) {
    bool semAutorizacao = !manutAutorizada;
    alarmeRackAtivo = semAutorizacao;

    JsonDocument data;
    data["rack_door_open"]  = true;
    data["alarm_door_open"] = alarmeRackAtivo;
    data["state"]           = "ABERTA";
    publicarEvento(MqttTopics::RACK_PORTA_ESTADO, "2_ocupacao_e_rack",
                   "RACK_DOOR_CHANGED", alarmeRackAtivo ? "ALERT" : "INFO", data);
  } else {
    alarmeRackAtivo = false;
    manutAutorizada = false;
    moverServo(0);

    JsonDocument data;
    data["rack_door_open"]  = false;
    data["alarm_door_open"] = false;
    data["state"]           = "FECHADA";
    publicarEvento(MqttTopics::RACK_PORTA_ESTADO, "2_ocupacao_e_rack",
                   "RACK_DOOR_CHANGED", "INFO", data);
  }

  enviarSyncEspNow(ESPNOW_RACK_EVENTO);
}

void tratarProximidade() {
  if (!portaRackAberta) return;
  if (millis() - ultimaProxMs < Timings::INTERVALO_PROX_MS) return;
  ultimaProxMs = millis();

  digitalWrite(Pins::PROX_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(Pins::PROX_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(Pins::PROX_TRIG, LOW);

  long duracaoUs = pulseIn(Pins::PROX_ECHO, HIGH, 25000UL);
  if (duracaoUs <= 0) return;

  ultimaDistCm = (duracaoUs * 0.0343f) / 2.0f;

  JsonDocument data;
  data["distance_cm"] = roundf(ultimaDistCm * 10.0f) / 10.0f;
  publicarEvento(MqttTopics::RACK_PROXIMIDADE, "2_ocupacao_e_rack",
                 "RACK_PROXIMITY_MEASURED", "INFO", data);
}

void tratarAmbienteEOled() {
  if (millis() - ultimoDhtMs >= Timings::INTERVALO_DHT_MS) {
    ultimoDhtMs = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      ultimaTempC   = t;
      ultimaUmidPct = h;
    }
  }

  if (millis() - ultimoOledMs < 300) return;
  ultimoOledMs = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Elo5 RackController"));
  display.print(F("Porta: "));
  display.println(portaRackAberta ? F("ABERTA") : F("FECHADA"));
  display.print(F("Alarme: "));
  display.println(alarmeRackAtivo ? F("CRITICO") : F("NORMAL"));
  display.print(F("Dist: "));
  if (ultimaDistCm > 0) {
    display.print(ultimaDistCm, 1);
    display.println(F(" cm"));
  } else {
    display.println(F("--"));
  }
  display.print(F("T:"));
  display.print(isnan(ultimaTempC) ? 0.0f : ultimaTempC, 1);
  display.print(F("C U:"));
  display.print(isnan(ultimaUmidPct) ? 0.0f : ultimaUmidPct, 0);
  display.println(F("%"));
  display.display();
}

} // namespace

void setupRackController() {
  Serial.begin(115200);

  pinMode(Pins::PORTA_RACK_SW, INPUT_PULLDOWN);
  pinMode(Pins::PROX_TRIG, OUTPUT);
  pinMode(Pins::PROX_ECHO, INPUT);
  pinMode(Pins::BUZZER, OUTPUT);

  ledcAttach(Pins::LED_R, 5000, 8);
  ledcAttach(Pins::LED_G, 5000, 8);
  ledcAttach(Pins::LED_B, 5000, 8);
  ledcAttachChannel(Pins::SERVO_PWM, 50, 16, SERVO_CH);
  moverServo(0);
  setLedRgb(0, 120, 0);

  Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  dht.begin();

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

  portaRackAnterior = (digitalRead(Pins::PORTA_RACK_SW) == HIGH);
  Serial.println("[BOOT] rack-controller pronto.");
}

void loopRackController() {
  garantirConexao();
  tratarNfcRack();
  tratarPortaRack();
  tratarProximidade();
  tratarAmbienteEOled();

  if (alarmeRackAtivo) {
    setLedRgb(255, 0, 0);
    tone(Pins::BUZZER, ((millis() / 250) % 2 == 0) ? 2200 : 1600);
  } else if (portaRackAberta) {
    setLedRgb(0, 180, 180);
    noTone(Pins::BUZZER);
  } else {
    setLedRgb(0, 120, 0);
    noTone(Pins::BUZZER);
  }
}

#endif // NODE_RACK
