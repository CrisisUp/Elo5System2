# Elo5System — Monitoramento Físico, Controle de Acesso Duplo Fator e Telemetria de CPD

Sistema distribuído de segurança física, rastreabilidade de sessão e telemetria ambiental para **Salas de CPD e Racks de Servidores**, operando com **dois microcontroladores ESP32 sincronizados em tempo real via ESP-NOW** e integrados via **MQTT sobre TLS/mTLS (`:8883`)** a uma stack de ingestão e observabilidade (**Mosquitto + FastAPI + PostgreSQL + InfluxDB + Grafana**).

## 1. Arquitetura Distribuída (Dois Nós ESP32 + ESP-NOW + MQTT mTLS)

| Camada / Nó | Identificador | Responsabilidade Principal | Periféricos & Sensores |
| :--- | :--- | :--- | :--- |
| **Nó 1 — Porta da Sala** | `porta_controller` (`esp32-porta-01`) | Controle de acesso de entrada na sala CPD, detecção de presença na sala, botão de saída e botão de pânico. | MFRC522 (NFC entrada), PIR (`GPIO 27`), Botão Saída (`GPIO 14`), Botão Pânico (`GPIO 26`), Servo trava (`GPIO 4`), LED RGB (`32/33/25`), Buzzer (`GPIO 13`). |
| **Barramento Local** | **ESP-NOW** (`elo5_protocol.h`) | Sincronização determinística de baixa latência (`ESPNOW_SALA_ACESSO`, `ESPNOW_SALA_SAIDA`, `ESPNOW_SALA_PANICO`, `ESPNOW_RACK_EVENTO` e heartbeat `ESPNOW_SYNC_ESTADO` a cada `3 s`). | Comunicação peer-to-peer Wi-Fi 2.4 GHz (Broadcast inicial ou Unicast com ACK por MAC). |
| **Nó 2 — Rack 01** | `rack-controller` (`esp32-rack-01`) | Autenticação NFC de 2ª etapa (janela de manutenção `15 s`), destravamento do rack, supervisão da porta, proximidade interna e telemetria ambiental. | MFRC522 (NFC 2ª etapa, `RST 16`), Reed Switch (`GPIO 25`), HC-SR04 (`TRIG 26 / ECHO 34`), DHT22 (`GPIO 13`), LDR (`GPIO 35`), OLED SSD1306 (`21/22`), Servo (`GPIO 27`), LED RGB (`32/33/4`), Buzzer (`GPIO 14`). |
| **Backend & Observabilidade** | `Mosquitto` + `Elo5api` + `Grafana` | Broker MQTT mTLS (`:8883`), API FastAPI, persistência relacional em PostgreSQL, séries temporais em InfluxDB e dashboards NOC. | Docker Compose (`docker-compose.yml`). |

## 2. Build, Simulação e Execução

```bash
# Compilar todos os 4 ambientes (porta/rack hardware e simulação)
make build

# Validar os dois ambientes Wokwi isolados
make lint-wokwi

# Subir toda a stack Docker (Mosquitto mTLS, FastAPI, PostgreSQL, InfluxDB e Grafana)
make up
