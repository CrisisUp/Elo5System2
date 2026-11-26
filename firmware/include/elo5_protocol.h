/**
 * @file elo5_protocol.h
 * @brief Contrato de eventos JSON (Elo5api) e sincronização bidirecional ESP-NOW.
 */

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include "node_config.h"

// =========================================================================
// 1. PACOTE BINÁRIO COMPACTO PARA SINCRONIZAÇÃO LOCAL VIA ESP-NOW
// =========================================================================
enum TipoMsgEspNow : uint8_t {
  ESPNOW_SALA_ACESSO   = 1,
  ESPNOW_SALA_SAIDA    = 2,
  ESPNOW_SALA_PANICO   = 3,
  ESPNOW_RACK_EVENTO   = 4,
  ESPNOW_SYNC_ESTADO   = 5
};

struct __attribute__((packed)) PacoteEspNow {
  uint8_t  versao;
  uint8_t  tipo;
  uint8_t  ocupacaoSala;
  uint8_t  portaRackAberta;
  uint8_t  alarmeAtivo;
  uint8_t  canalWifi;
  uint32_t seq;
  uint32_t uptimeMs;
  char     uidAtivo[17];
  char     sessionId[33];
};

// =========================================================================
// 2. GERENCIADOR ESP-NOW THREAD-SAFE (SEM ALOCAÇÃO DINÂMICA NA CALLBACK)
// =========================================================================
class Elo5EspNowLink {
  public:
    static bool isBroadcastMac(const uint8_t mac[6]) {
      for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) return false;
      }
      return true;
    }

    bool begin(const uint8_t peerMac[6]) {
      memcpy(_peerMac, peerMac, 6);
      _unicastStrict = !isBroadcastMac(_peerMac);
      _lastRxSeq = 0;
      _lastRxUptimeMs = 0;
      WiFi.mode(WIFI_STA);
      esp_wifi_set_channel(EspNowConfig::DEFAULT_CHANNEL, WIFI_SECOND_CHAN_NONE);

      Serial.print("[ESPNOW][MAC_STA] Local: ");
      Serial.println(WiFi.macAddress());
      if (_unicastStrict) {
        Serial.printf("[ESPNOW] Modo UNICAST ESTRITO ativo (peer=%02X:%02X:%02X:%02X:%02X:%02X)\n",
                      _peerMac[0], _peerMac[1], _peerMac[2], _peerMac[3], _peerMac[4], _peerMac[5]);
      } else {
        Serial.println("[ESPNOW][AVISO] Operando em BROADCAST (FF:FF:FF:FF:FF:FF). Configure os MACs reais em node_config.h para producao!");
      }

      if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] Falha ao inicializar esp_now_init().");
        _ativo = false;
        return false;
      }

      _instance = this;
      esp_now_register_recv_cb(_onRecvStatic);
      esp_now_register_send_cb(_onSentStatic);

      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, _peerMac, 6);
      peer.channel = 0; // Usa dinamicamente o canal ativo do rádio Wi-Fi STA
      peer.encrypt = false;

      if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ESPNOW] Falha ao adicionar peer.");
        _ativo = false;
        return false;
      }

      _ativo = true;
      Serial.println("[ESPNOW] Link ESP-NOW inicializado com sucesso.");
      return true;
    }

    bool sendPacket(uint8_t tipo, uint8_t ocupacaoSala, bool portaRackAberta,
                    bool alarmeAtivo, const String& uidAtivo, const String& sessionId) {
      if (!_ativo) return false;

      PacoteEspNow pkt = {};
      pkt.versao          = 1;
      pkt.tipo            = tipo;
      pkt.ocupacaoSala    = ocupacaoSala;
      pkt.portaRackAberta = portaRackAberta ? 1 : 0;
      pkt.alarmeAtivo     = alarmeAtivo ? 1 : 0;
      pkt.canalWifi       = (uint8_t)WiFi.channel();
      pkt.seq             = ++_txSeq;
      pkt.uptimeMs        = millis();

      strncpy(pkt.uidAtivo, uidAtivo.c_str(), sizeof(pkt.uidAtivo) - 1);
      pkt.uidAtivo[sizeof(pkt.uidAtivo) - 1] = '\0';

      strncpy(pkt.sessionId, sessionId.c_str(), sizeof(pkt.sessionId) - 1);
      pkt.sessionId[sizeof(pkt.sessionId) - 1] = '\0';

      esp_err_t res = esp_now_send(_peerMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
      return (res == ESP_OK);
    }

    bool consumeReceived(PacoteEspNow& outPkt) {
      if (!_hasRx) return false;
      noInterrupts();
      memcpy(&outPkt, const_cast<const PacoteEspNow*>(&_rxPkt), sizeof(PacoteEspNow));
      _hasRx = false;
      interrupts();
      return true;
    }

    bool consumeSendError() {
      if (!_sendFailed) return false;
      _sendFailed = false;
      return true;
    }

    bool isPeerConnected(unsigned long maxAgeMs = 10000UL) const {
      return (_lastRxMs > 0) && ((millis() - _lastRxMs) <= maxAgeMs);
    }

  private:
    static Elo5EspNowLink* _instance;
    uint8_t       _peerMac[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool          _ativo       = false;
    bool          _unicastStrict = false;
    uint32_t      _txSeq         = 0;
    uint32_t      _lastRxSeq     = 0;
    uint32_t      _lastRxUptimeMs = 0;
    volatile bool _hasRx         = false;
    volatile bool _sendFailed  = false;
    unsigned long _lastRxMs    = 0;
    PacoteEspNow  _rxPkt       = {};

    static void _onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
      if (_instance == nullptr || info == nullptr || len != sizeof(PacoteEspNow)) return;
      if (_instance->_unicastStrict && memcmp(info->src_addr, _instance->_peerMac, 6) != 0) {
        return; // Descarta pacote de origem nao pareada
      }
      const PacoteEspNow* incoming = reinterpret_cast<const PacoteEspNow*>(data);
      if (incoming->versao != 1) return;
      const bool peerReiniciou = (incoming->uptimeMs < _instance->_lastRxUptimeMs);
      if (!peerReiniciou && incoming->seq <= _instance->_lastRxSeq) {
        return; // Descarta quadro duplicado ou replay
      }
      _instance->_lastRxSeq      = incoming->seq;
      _instance->_lastRxUptimeMs = incoming->uptimeMs;
      memcpy(&_instance->_rxPkt, incoming, sizeof(PacoteEspNow));
      _instance->_lastRxMs = millis();
      _instance->_hasRx    = true;
    }

    static void _onSentStatic(const uint8_t* mac_addr, esp_now_send_status_t status) {
      (void)mac_addr;
      if (_instance != nullptr && status != ESP_NOW_SEND_SUCCESS) {
        _instance->_sendFailed = true;
      }
    }
};

inline Elo5EspNowLink* Elo5EspNowLink::_instance = nullptr;

// =========================================================================
// 3. CONSTRUTOR DE ENVELOPE JSON COMPATÍVEL COM ELO5API (/api/v1/events/ingest)
// =========================================================================
inline String buildElo5EventJson(
    const String& eventId,
    const String& timestampIso,
    const char*   stage,
    const char*   eventType,
    const char*   severity,
    const String& sessionId,
    const char*   uidFieldName,
    const String& uidValue,
    int           occupancyCount,
    JsonDocument& dataDoc) {

  JsonDocument root;
  root["event_id"]  = eventId;
  root["timestamp"] = timestampIso;

  JsonObject source = root["source"].to<JsonObject>();
  source["device_id"] = NodeIdentity::DEVICE_ID;
  source["zone"]      = NodeIdentity::ZONE;

  root["stage"]      = stage;
  root["event_type"] = eventType;
  root["severity"]   = severity;

  JsonObject session = root["session"].to<JsonObject>();
  if (sessionId.length() > 0) {
    session["session_id"] = sessionId;
  } else {
    session["session_id"] = nullptr;
  }
  if (uidFieldName != nullptr && uidValue.length() > 0) {
    session[uidFieldName] = uidValue;
  }
  session["occupancy_count"] = occupancyCount;

  root["data"] = dataDoc.as<JsonVariantConst>();

  String output;
  serializeJson(root, output);
  return output;
}
