/**
 * @file elo5_protocol.h
 * @brief Contrato de eventos JSON (Elo5api) e sincronização ESP-NOW entre nós.
 */

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "node_config.h"

// =========================================================================
// 1. PACOTE BINÁRIO COMPACTO PARA SINCRONIZAÇÃO LOCAL VIA ESP-NOW
// =========================================================================
enum TipoMsgEspNow : uint8_t {
  ESPNOW_SALA_ACESSO   = 1,
  ESPNOW_SALA_SAIDA    = 2,
  ESPNOW_RACK_EVENTO   = 3,
  ESPNOW_SYNC_ESTADO   = 4
};

struct __attribute__((packed)) PacoteEspNow {
  uint8_t  versao;
  uint8_t  tipo;
  uint8_t  ocupacaoSala;
  uint8_t  portaRackAberta;
  uint8_t  alarmeAtivo;
  uint32_t uptimeMs;
  char     uidAtivo[17];
  char     sessionId[33];
};

// =========================================================================
// 2. CONSTRUTOR DE ENVELOPE JSON COMPATÍVEL COM ELO5API (/api/v1/events/ingest)
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
