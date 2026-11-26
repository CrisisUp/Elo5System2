# Elo5System v4 — Controle de Acesso, Telemetria e Segurança Física de CPD & Rack

Plataforma ponta a ponta (**IoT + mTLS + FastAPI + Painel NOC Web**) para monitoramento de acesso, presença e integridade física de salas técnicas (CPD) e racks de servidores, com correlação automática de alarmes críticos (como saída da sala com a porta do rack aberta).

---

## 1. Arquitetura do Sistema

- **Camada Edge / Dispositivos (`firmware/` e `simulator/`):** ESP32 (hardware real ou simulação Wokwi) e simulador Python publicam eventos JSON via **MQTT sobre TLS 1.2+ com autenticação mútua (mTLS)** na porta `8883` usando certificados X.509 assinados por CA própria + usuário/senha.
- **Broker MQTT (`mosquitto/config/`):** Eclipse Mosquitto 2.0 validando cadeia X.509 (`require_certificate true`), ACLs por usuário/tópico e roteamento em `elo5/#`.
- **Backend & Painel NOC (`api/`):** Serviço **FastAPI + SQLAlchemy + SQLite** (`Elo5api`) que persiste eventos com idempotência por `event_id`, valida credenciais NFC, correlaciona o estado da porta do rack na saída da sala (`alarm_door_open`) e serve o Painel Web NOC em tempo real (`/`) com headers de segurança (`X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, `Content-Security-Policy`).

### Estrutura de Diretórios

| Caminho | Descrição |
| :--- | :--- |
| `api/` | Backend **FastAPI** (`main.py`, `models.py`, `schemas.py`, `database.py`), interface web (`static/`) e testes automatizados (`tests/test_elo5api.py`). |
| `firmware/` | Firmware **ESP32** (PlatformIO) com ambientes `esp32_hardware_real` e `esp32_wokwi_sim` (NFC/RFID, PIR, ultrassônico HC-SR04 e reed switch). |
| `mosquitto/config/` | Configuração do broker `mosquitto.conf`, ACLs e diretório local de certificados X.509 (`certs/`, ignorado no Git). |
| `simulator/` | Simulador Python (`simulate_elo5_flow.py`) que reproduz o ciclo completo de entrada, abertura do rack, proximidade, saída e reset via MQTT mTLS. |
| `scripts/` | Automação de injeção de segredos do ESP32 (`inject_esp32_secrets.sh`) e suíte de validação ponta a ponta (`test_e2e.sh`). |

---

## 2. Pré-requisitos

- **Docker** e **Docker Compose** (OrbStack ou Docker Desktop)
- **OpenSSL** e **Python 3.11+**
- **GNU Make** e **curl**
- **PlatformIO CLI (`pio`)** *(opcional, apenas para compilar/gravar o firmware ESP32)*

---

## 3. Como Executar

### Opção A — Modo Automatizado (Recomendado)

1. **Provisionar `.env`, CA, certificados mTLS, `pwfile` e `firmware/include/secrets.h`:**
   ```bash
   cp -n .env.example .env
   make setup
   ```

2. **Subir os containers (Mosquitto `:8883` + Elo5api `:8000`):**
   ```bash
   make up
   ```

3. **Executar a validação ponta a ponta completa (8 etapas — pytest, Docker, Painel NOC, Simulador mTLS, API REST e PlatformIO):**
   ```bash
   make test-e2e
   ```

4. **Disparar uma simulação avulsa de fluxo MQTT mTLS:**
   ```bash
   make sim SIM_UID=04A1B2C3D4 SIM_DELAY=0.5
   ```

### Opção B — Modo Manual (Sem `make`)

1. **Criar `.env` e gerar a Autoridade Certificadora (CA) e certificados mTLS:**
   ```bash
   cp .env.example .env
   mkdir -p mosquitto/config/certs
   openssl req -new -x509 -days 365 -nodes -subj "/C=BR/ST=SP/L=SaoPaulo/O=Elo5System/CN=Elo5-Root-CA" \
     -keyout mosquitto/config/certs/ca.key -out mosquitto/config/certs/ca.crt
   ```
   *(Gere também `server.crt` com SAN `DNS:mosquitto,DNS:localhost,IP:127.0.0.1` e os certificados de cliente `api`, `sim`, `client` e `esp32` assinados pela CA, além do arquivo de senhas com `mosquitto_passwd`.)*

2. **Injetar certificados e credenciais no firmware ESP32:**
   ```bash
   bash scripts/inject_esp32_secrets.sh mosquitto/config/certs firmware/include/secrets.h firmware/include/secrets.h.example
   ```

3. **Subir a stack Docker e rodar os testes unitários:**
   ```bash
   docker compose up -d --build
   python3 -m venv .venv && source .venv/bin/activate
   pip install -r api/requirements.txt
   pytest
   ```

4. **Compilar o firmware ESP32 manualmente via PlatformIO:**
   ```bash
   cd firmware && pio run
   ```

---

## 4. Endpoints e Contratos de Comunicação

### Interfaces Web & API REST (`http://localhost:8000`)

| Método | Rota | Descrição |
| :--- | :--- | :--- |
| `GET` | `/` | Painel NOC Web em tempo real (telemetria da sala, rack, alertas e logs). |
| `GET` | `/docs` | Documentação interativa OpenAPI / Swagger. |
| `GET` | `/api/v1/health` | Status de saúde da API e conexão com o broker MQTT. |
| `GET` | `/api/v1/state/room` | Estado atual da sala CPD (ocupação, movimento PIR, última sessão). |
| `GET` | `/api/v1/state/rack` | Estado atual do rack (porta aberta/fechada, proximidade, `alarm_door_open`). |
| `POST` | `/api/v1/events/ingest` | Ingestão direta de eventos JSON (com deduplicação por `event_id`). |
| `GET / POST` | `/api/v1/credentials` | Consulta e cadastro de credenciais NFC (`uid_nfc`, permissões de sala/rack). |

### Tópicos MQTT (`elo5/#` na porta `8883` com mTLS)

| Tópico MQTT | Etapa | Evento (`event_type`) | Função |
| :--- | :--- | :--- | :--- |
| `elo5/sala/acesso/nfc/evento` | `1_acesso_sala` | `NFC_ACCESS_EVALUATED` | Valida crachá NFC na entrada da sala CPD. |
| `elo5/sala/presenca/pir` | `2_ocupacao_e_rack` | `PIR_MOTION_CHANGED` | Detecta presença física interna via sensor PIR. |
| `elo5/rack/acesso/nfc/evento` | `2_ocupacao_e_rack` | `RACK_ACCESS_EVALUATED` | Valida autenticação NFC de segundo fator no rack. |
| `elo5/rack/porta/estado` | `2_ocupacao_e_rack` | `RACK_DOOR_CHANGED` | Monitora abertura/fechamento da porta do rack. |
| `elo5/rack/proximidade/cm` | `2_ocupacao_e_rack` | `RACK_PROXIMITY_MEASURED` | Mede distância em centímetros diante do rack aberto. |
| `elo5/sala/saida/botao` | `3_saida_e_reset` | `EXIT_BUTTON_PRESSED` / `SESSION_RESET_COMPLETED` | Registra saída, dispara alarme se o rack ficou aberto e encerra a sessão. |

---

## 5. Segurança e Troubleshooting

- **Segredos fora do Git:** Arquivos `.env`, `mosquitto/config/certs/*`, `mosquitto/config/pwfile` e `firmware/include/secrets.h` são gerados localmente e bloqueados pelo `.gitignore`.
- **Conflito de nome de container (`Conflict. The container name "/elo5-api" is already in use`):** Remova containers remanescentes antes de subir novamente com `docker rm -f elo5-api elo5-mosquitto elo5-sim`.
- **Erro TLS/mTLS após regenerar certificados (`CERTIFICATE_VERIFY_FAILED`):** Sempre recrie os containers para carregar os novos PEMs com `docker compose up -d --force-recreate`.
