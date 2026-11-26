/**
 * @file node_config.h
 * @brief Mapeamento de hardware, tópicos MQTT (elo5/#), ESP-NOW e identidade por nó ESP32.
 *
 * Selecione o alvo via flags de compilação no platformio.ini:
 *   -DNODE_PORTA  -> Controlador da Sala CPD (NFC Entrada, PIR, Botão Saída/Pânico)
 *   -DNODE_RACK   -> Controlador do Rack (NFC 2ª Etapa, Porta Rack, Proximidade, DHT22, LDR, OLED, Servo)
 */

#pragma once

#include <Arduino.h>

#if !defined(NODE_PORTA) && !defined(NODE_RACK)
  #define NODE_RACK
#endif

#if defined(NODE_PORTA) && defined(NODE_RACK)
  #error "Defina apenas um alvo de compilacao: NODE_PORTA ou NODE_RACK."
#endif

// =========================================================================
// 1. IDENTIDADE DO NÓ E TÓPICOS MQTT OFICIAIS (ELO5/#)
// =========================================================================
namespace NodeIdentity {
#if defined(NODE_PORTA)
  constexpr const char* DEVICE_ID = "esp32-porta-01";
  constexpr const char* ZONE      = "sala_cpd";
  constexpr const char* ROLE_NAME = "PORTA_CONTROLLER";
#elif defined(NODE_RACK)
  constexpr const char* DEVICE_ID = "esp32-rack-01";
  constexpr const char* ZONE      = "rack_01";
  constexpr const char* ROLE_NAME = "RACK_CONTROLLER";
#endif
}

namespace MqttTopics {
  // Tópicos publicados pelo porta_controller
  constexpr const char* SALA_NFC_EVENTO   = "elo5/sala/acesso/nfc/evento";
  constexpr const char* SALA_PIR_PRESENCA = "elo5/sala/presenca/pir";
  constexpr const char* SALA_SAIDA_BOTAO  = "elo5/sala/saida/botao";

  // Tópicos publicados pelo rack-controller
  constexpr const char* RACK_NFC_EVENTO   = "elo5/rack/acesso/nfc/evento";
  constexpr const char* RACK_PORTA_ESTADO = "elo5/rack/porta/estado";
  constexpr const char* RACK_PROXIMIDADE  = "elo5/rack/proximidade/cm";
  constexpr const char* RACK_AMBIENTE     = "elo5/rack/telemetria/ambiente";

  // Tópico de status / LWT por dispositivo
#if defined(NODE_PORTA)
  constexpr const char* DEVICE_LWT        = "elo5/system/porta_controller/lwt";
#elif defined(NODE_RACK)
  constexpr const char* DEVICE_LWT        = "elo5/system/rack_controller/lwt";
#endif
}

// =========================================================================
// 2. CONFIGURAÇÃO ESP-NOW (COMUNICAÇÃO DIRETA ENTRE OS DOIS ESP32)
// =========================================================================
namespace EspNowConfig {
  constexpr uint8_t DEFAULT_CHANNEL      = 1;
  constexpr unsigned long SYNC_HEARTBEAT_MS = 3000UL;

  // Substitua pelos endereços MAC STA reais impressos em [MAC_STA] no boot
  // para ativar unicast com ACK e retransmissão automática na camada MAC.
  // Enquanto mantiver FF:FF:FF:FF:FF:FF, opera em modo broadcast.
  constexpr uint8_t MAC_RACK_CONTROLLER[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  constexpr uint8_t MAC_PORTA_CONTROLLER[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

// =========================================================================
// 3. PINAGEM ROBUSTA POR NÓ (LIVRE DE CONFLITOS DE STRAPPING / BARRAMENTO)
// =========================================================================
namespace Pins {

#if defined(NODE_PORTA)
  // Barramento SPI (VSPI) — Leitor NFC/RFID MFRC522 (3,3V)
  constexpr uint8_t RFID_SS      = 5;
  constexpr uint8_t RFID_SCK     = 18;
  constexpr uint8_t RFID_MISO    = 19;
  constexpr uint8_t RFID_MOSI    = 23;
  constexpr uint8_t RFID_RST     = 22;

  // Sensores e Entradas Digitais
  constexpr uint8_t PIR_SALA     = 27;  // Sensor de presença PIR da sala CPD
  constexpr uint8_t BOTAO_SAIDA  = 14;  // INPUT_PULLUP (ativo em LOW -> botão para GND)
  constexpr uint8_t BOTAO_PANICO = 26;  // INPUT_PULLUP com ISR FALLING

  // Atuadores e Sinalização Local
  constexpr uint8_t LED_R        = 32;  // PWM LEDC
  constexpr uint8_t LED_G        = 33;  // PWM LEDC
  constexpr uint8_t LED_B        = 25;  // PWM LEDC
  constexpr uint8_t BUZZER       = 13;  // PWM tone()
  constexpr uint8_t TRAVA_PORTA  = 4;   // Relé/fechadura da porta da sala

#elif defined(NODE_RACK)
  // Barramento SPI (VSPI) — Leitor NFC/RFID MFRC522 2ª Etapa (3,3V)
  constexpr uint8_t RFID_SS       = 5;
  constexpr uint8_t RFID_SCK      = 18;
  constexpr uint8_t RFID_MISO     = 19;
  constexpr uint8_t RFID_MOSI     = 23;
  constexpr uint8_t RFID_RST      = 16;

  // Barramento I2C — Display OLED SSD1306 (0x3C) e RTC DS1307
  constexpr uint8_t I2C_SDA       = 21;
  constexpr uint8_t I2C_SCL       = 22;

  // Sensores Físicos & Ambientais do Rack
  constexpr uint8_t PORTA_RACK_SW = 25; // Reed switch da porta do rack
  constexpr uint8_t PROX_TRIG     = 26; // Ultrassônico HC-SR04 TRIG
  constexpr uint8_t PROX_ECHO     = 34; // Input-only (usar divisor resistivo 5V->3,3V!)
  constexpr uint8_t DHT_PIN       = 13; // DHT22 Temperatura e Umidade
  constexpr uint8_t LDR_AO        = 35; // Input-only ADC1 (luminosidade)

  // Atuadores do Rack
  constexpr uint8_t SERVO_PWM     = 27; // Servo trava do rack (5V externo + GND comum)
  constexpr uint8_t BUZZER        = 14; // Alarme sonoro local
  constexpr uint8_t LED_R         = 32; // PWM LEDC
  constexpr uint8_t LED_G         = 33; // PWM LEDC
  constexpr uint8_t LED_B         = 4;  // PWM LEDC
#endif

} // namespace Pins

// =========================================================================
// 4. PARÂMETROS DE TEMPORIZAÇÃO E LIMIARES OPERACIONAIS
// =========================================================================
namespace Timings {
  constexpr unsigned long WARMUP_PIR_MS          = 1500UL;
  constexpr unsigned long WARMUP_LDR_MS          = 1500UL;
  constexpr unsigned long DEBOUNCE_PORTA_MS      = 200UL;
  constexpr unsigned long DEBOUNCE_BOTAO_MS      = 45UL;
  constexpr unsigned long DURACAO_ACESSO_RFID_MS = 6000UL;
  constexpr unsigned long JANELA_MANUT_RACK_MS   = 15000UL;
  constexpr unsigned long INTERVALO_PROX_MS      = 1000UL;
  constexpr unsigned long INTERVALO_DHT_MS       = 2000UL;
  constexpr unsigned long MQTT_RETRY_MS          = 5000UL;
}
