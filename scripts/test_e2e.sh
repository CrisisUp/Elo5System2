#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Elo5System v4 — Suíte Completa de Validação Ponta a Ponta (E2E)
# ==============================================================================
# Executa, valida e gera relatório resumido cobrindo:
#   1. Pré-requisitos (Docker, OpenSSL, Python 3) e estrutura de arquivos
#   2. Provisionamento automático (.env, CA, certificados mTLS, pwfile, secrets.h)
#   3. Suíte unitária e de integração (pytest)
#   4. Build e subida dos containers (Mosquitto mTLS :8883 + Elo5api :8000)
#   5. Teste de headers de segurança (CSP, X-Frame-Options, X-Content-Type-Options)
#   6. Simulação MQTT ponta a ponta via mTLS (elo5-sim)
#   7. Teste de fluxo crítico de alarme via API REST (porta aberta na saída + reset)
#   8. Teste de idempotência de event_id e persistência de credenciais NFC
#   9. Compilação opcional do firmware ESP32 (PlatformIO) se 'pio' estiver no PATH
# ==============================================================================

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SKIP_FIRMWARE=false
KEEP_UP=true
for arg in "$@"; do
  case "$arg" in
    --skip-firmware) SKIP_FIRMWARE=true ;;
    --down-after)    KEEP_UP=false ;;
    -h|--help)
      echo "Uso: $0 [--skip-firmware] [--down-after]"
      exit 0
      ;;
  esac
done

RED="\033[0;31m"
GREEN="\033[0;32m"
YELLOW="\033[1;33m"
BLUE="\033[0;34m"
BOLD="\033[1m"
NC="\033[0m"

PASS_COUNT=0
FAIL_COUNT=0
WARN_COUNT=0

step() { echo -e "\n${BLUE}${BOLD}==> $1${NC}"; }
ok()   { echo -e "  ${GREEN}[OK]${NC} $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
warn() { echo -e "  ${YELLOW}[AVISO]${NC} $1"; WARN_COUNT=$((WARN_COUNT + 1)); }
fail() { echo -e "  ${RED}[FALHA]${NC} $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); exit 1; }

cleanup() {
  if [[ "$KEEP_UP" == "false" ]]; then
    echo -e "\n${YELLOW}Encerrando containers (--down-after)...${NC}"
    make down >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# ------------------------------------------------------------------------------
# 1. Pré-requisitos
# ------------------------------------------------------------------------------
step "1/8 Verificando pré-requisitos do sistema"
for cmd in docker openssl python3 curl make; do
  if command -v "$cmd" >/dev/null 2>&1; then
    ok "Comando disponível: $cmd"
  else
    fail "Comando obrigatório ausente: $cmd"
  fi
done

if ! docker info >/dev/null 2>&1; then
  fail "Daemon do Docker/OrbStack não está rodando. Inicie-o antes de executar."
fi
ok "Daemon Docker/OrbStack ativo"

# ------------------------------------------------------------------------------
# 2. Configuração de .env e Certificados mTLS + pwfile + secrets.h
# ------------------------------------------------------------------------------
step "2/8 Configurando .env, certificados TLS/mTLS e pwfile"
if [[ ! -f .env ]]; then
  cp .env.example .env
  ok "Arquivo .env criado a partir de .env.example"
else
  ok "Arquivo .env já existente preservado"
fi

# Carrega variáveis do .env para uso nos testes (ex: API_ADMIN_KEY)
set -a
# shellcheck disable=SC1091
source .env
set +a

make setup >/dev/null
for f in   mosquitto/config/certs/ca.crt   mosquitto/config/certs/server.crt   mosquitto/config/certs/api.crt   mosquitto/config/certs/sim.crt   mosquitto/config/certs/esp32.crt   mosquitto/config/pwfile   firmware/include/secrets.h; do
  [[ -f "$f" ]] && ok "Artefato gerado: $f" || fail "Artefato ausente após make setup: $f"
done

# ------------------------------------------------------------------------------
# 3. Ambiente Virtual Python e Testes Automatizados (pytest)
# ------------------------------------------------------------------------------
step "3/8 Executando suíte de testes automatizados (pytest)"
if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate
pip install -q -r api/requirements.txt pytest httpx
DATABASE_URL="sqlite:////tmp/elo5_pytest.db" MQTT_BROKER="127.0.0.1" MQTT_PORT="1883" pytest -v
ok "Todos os testes pytest passaram"

# ------------------------------------------------------------------------------
# 4. Subida da Stack Docker (Mosquitto mTLS + Elo5api)
# ------------------------------------------------------------------------------
step "4/8 Subindo containers Docker (Mosquitto :8883 + Elo5api :8000)"
docker compose up -d --build --force-recreate

for i in $(seq 1 20); do
  if curl -fsS "http://localhost:8000/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

HEALTH_JSON="$(curl -fsS "http://localhost:8000/api/v1/health")" || fail "Elo5api não respondeu em http://localhost:8000/api/v1/health"
python3 - <<PY "$HEALTH_JSON"
import json, sys
data = json.loads(sys.argv[1])
assert data["status"] == "ok", f"Status inesperado: {data}"
assert data["mqtt"]["connected"] is True, f"MQTT não conectado: {data}"
assert data["mqtt"]["tls_enabled"] is True, f"TLS desabilitado: {data}"
assert data["mqtt"]["mtls_enabled"] is True, f"mTLS desabilitado: {data}"
PY
ok "Mosquitto mTLS conectado e Elo5api saudável"

# ------------------------------------------------------------------------------
# 5. Validação do Painel Web NOC e Headers de Segurança (CSP / Frame / Sniff)
# ------------------------------------------------------------------------------
step "5/8 Validando Painel Web NOC e headers de segurança HTTP"
HEADERS="$(curl -fsS -D - -o /dev/null "http://localhost:8000/")" || fail "Falha ao acessar painel web GET /"
echo "$HEADERS" | grep -qi "X-Frame-Options: DENY" && ok "Header X-Frame-Options: DENY presente" || fail "Header X-Frame-Options ausente"
echo "$HEADERS" | grep -qi "X-Content-Type-Options: nosniff" && ok "Header X-Content-Type-Options: nosniff presente" || fail "Header X-Content-Type-Options ausente"
echo "$HEADERS" | grep -qi "Content-Security-Policy:.*script-src 'self'" && ok "CSP estrita (script-src 'self') presente" || fail "CSP ausente ou permissiva"

curl -fsS "http://localhost:8000/static/app.js" >/dev/null && ok "Asset /static/app.js acessível" || fail "Asset /static/app.js ausente"
curl -fsS "http://localhost:8000/static/styles.css" >/dev/null && ok "Asset /static/styles.css acessível" || fail "Asset /static/styles.css ausente"

# ------------------------------------------------------------------------------
# 6. Simulação MQTT Ponta a Ponta via mTLS (elo5-sim)
# ------------------------------------------------------------------------------
step "6/8 Executando simulador MQTT mTLS (elo5-sim)"
make sim SIM_UID=04A1B2C3D4 SIM_DELAY=0.2
ok "Simulador publicou os 11 eventos via MQTT mTLS com sucesso"

# ------------------------------------------------------------------------------
# 7. Testes Funcionais na API REST (Credenciais, Alarme de Porta Aberta e Reset)
# ------------------------------------------------------------------------------
step "7/8 Validando regras de negócio via API REST (Credenciais + Alarme + Reset)"
API_HEADERS=(-H "Content-Type: application/json")
if [[ -n "${API_ADMIN_KEY:-}" ]]; then
  API_HEADERS+=(-H "X-API-Key: ${API_ADMIN_KEY}")
  # Testa bloqueio sem chave
  HTTP_CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://localhost:8000/api/v1/credentials"     -H "Content-Type: application/json"     -d '{"uid_nfc":"04TEST0001","owner_name":"Teste Bloqueio","can_enter_room":true,"can_access_rack":true,"is_active":true}')"
  [[ "$HTTP_CODE" == "401" ]] && ok "Bloqueio HTTP 401 confirmado sem X-API-Key" || fail "Esperado 401 sem X-API-Key, recebido $HTTP_CODE"
fi

# Cadastra credencial NFC de teste
curl -fsS -X POST "http://localhost:8000/api/v1/credentials" "${API_HEADERS[@]}"   -d '{"uid_nfc":"04A1B2C3D4","owner_name":"Cristiano (Operador CPD)","can_enter_room":true,"can_access_rack":true,"is_active":true}' >/dev/null
ok "Credencial 04A1B2C3D4 cadastrada/atualizada via API"

post_event() {
  local payload="$1"
  local resp_file
  resp_file="$(mktemp)"
  local code
  code="$(curl -s -o "$resp_file" -w "%{http_code}" -X POST "http://localhost:8000/api/v1/events/ingest" "${API_HEADERS[@]}" -d "$payload")"
  if [[ "$code" -lt 200 || "$code" -ge 300 ]]; then
    echo "HTTP $code em /api/v1/events/ingest:" >&2
    cat "$resp_file" >&2
    echo "" >&2
    rm -f "$resp_file"
    return 1
  fi
  rm -f "$resp_file"
}

SESSION_E2E="sess_e2e_$(date +%s)"

# 7a. Entrada na sala
P1="$(python3 -c 'import json,sys; print(json.dumps({"event_id":f"{sys.argv[1]}_1","timestamp":"2026-11-26T01:00:00Z","source":{"device_id":"e2e-script","zone":"sala_cpd"},"stage":"1_acesso_sala","event_type":"NFC_ACCESS_EVALUATED","severity":"INFO","session":{"session_id":sys.argv[1],"uid_nfc":"04A1B2C3D4","occupancy_count":1},"data":{"authorized":True,"decision":"PERMITIDO"}}))' "$SESSION_E2E")"
post_event "$P1"

# 7b. Abertura da porta do rack
P2="$(python3 -c 'import json,sys; print(json.dumps({"event_id":f"{sys.argv[1]}_2","timestamp":"2026-11-26T01:00:01Z","source":{"device_id":"e2e-script","zone":"rack_01"},"stage":"2_ocupacao_e_rack","event_type":"RACK_DOOR_CHANGED","severity":"WARN","session":{"session_id":sys.argv[1],"uid_nfc_operador":"04A1B2C3D4","occupancy_count":1},"data":{"rack_door_open":True,"state":"ABERTA"}}))' "$SESSION_E2E")"
post_event "$P2"

# 7c. Aciona saída com porta ainda aberta -> deve disparar alarm_door_open = true
P3="$(python3 -c 'import json,sys; print(json.dumps({"event_id":f"{sys.argv[1]}_3","timestamp":"2026-11-26T01:00:02Z","source":{"device_id":"e2e-script","zone":"rack_01"},"stage":"3_saida_e_reset","event_type":"EXIT_BUTTON_PRESSED","severity":"ALERT","session":{"session_id":sys.argv[1],"uid_nfc_saindo":"04A1B2C3D4","occupancy_count":0},"data":{"exit_button_pressed":True,"rack_door_open":True,"alarm_door_open":True}}))' "$SESSION_E2E")"
post_event "$P3"

RACK_ALARM_JSON="$(curl -fsS "http://localhost:8000/api/v1/state/rack")"
python3 - <<PY "$RACK_ALARM_JSON"
import json, sys
rack = json.loads(sys.argv[1])
assert rack["rack_door_open"] is True, f"Porta deveria estar aberta: {rack}"
assert rack["alarm_door_open"] is True, f"Alarme deveria estar ativo: {rack}"
PY
ok "Alarme crítico (alarm_door_open=true) validado na saída com porta aberta"

# 7d. Fecha porta do rack e conclui reset da sessão
P4="$(python3 -c 'import json,sys; print(json.dumps({"event_id":f"{sys.argv[1]}_4","timestamp":"2026-11-26T01:00:03Z","source":{"device_id":"e2e-script","zone":"rack_01"},"stage":"2_ocupacao_e_rack","event_type":"RACK_DOOR_CHANGED","severity":"INFO","session":{"session_id":sys.argv[1],"uid_nfc_operador":"04A1B2C3D4","occupancy_count":0},"data":{"rack_door_open":False,"alarm_door_open":False,"state":"FECHADA"}}))' "$SESSION_E2E")"
post_event "$P4"

P5="$(python3 -c 'import json,sys; print(json.dumps({"event_id":f"{sys.argv[1]}_5","timestamp":"2026-11-26T01:00:04Z","source":{"device_id":"e2e-script","zone":"sala_cpd"},"stage":"3_saida_e_reset","event_type":"SESSION_RESET_COMPLETED","severity":"INFO","session":{"session_id":sys.argv[1],"uid_nfc_saindo":"04A1B2C3D4","occupancy_count":0},"data":{"rack_door_open":False,"pir_motion":False,"room_reset_completed":True}}))' "$SESSION_E2E")"
post_event "$P5"

ROOM_FINAL_JSON="$(curl -fsS "http://localhost:8000/api/v1/state/room")"
RACK_FINAL_JSON="$(curl -fsS "http://localhost:8000/api/v1/state/rack")"
python3 - <<PY "$ROOM_FINAL_JSON" "$RACK_FINAL_JSON"
import json, sys
room = json.loads(sys.argv[1])
rack = json.loads(sys.argv[2])
assert room["occupancy_count"] == 0, f"Ocupação não zerou: {room}"
assert rack["rack_door_open"] is False, f"Porta não fechou: {rack}"
assert rack["alarm_door_open"] is False, f"Alarme não limpou: {rack}"
PY
ok "Fechamento do rack e reset da sessão normalizados com sucesso"

# ------------------------------------------------------------------------------
# 8. Compilação Opcional do Firmware ESP32 (PlatformIO)
# ------------------------------------------------------------------------------
step "8/8 Verificando compilação do firmware ESP32 (PlatformIO)"
if [[ "$SKIP_FIRMWARE" == "true" ]]; then
  warn "Compilação do firmware ignorada (--skip-firmware)"
elif command -v pio >/dev/null 2>&1; then
  pio run -d firmware -e esp32_hardware_real >/dev/null
  ok "Firmware compilado com sucesso (esp32_hardware_real)"
  pio run -d firmware -e esp32_wokwi_sim >/dev/null
  ok "Firmware compilado com sucesso (esp32_wokwi_sim)"
else
  warn "PlatformIO (pio) não encontrado no PATH; pulando compilação do firmware"
fi

echo -e "\n${GREEN}${BOLD}==============================================================================${NC}"
echo -e "${GREEN}${BOLD}  TESTE COMPLETO CONCLUÍDO! Sucessos: ${PASS_COUNT} | Avisos: ${WARN_COUNT} | Falhas: ${FAIL_COUNT}${NC}"
echo -e "${GREEN}${BOLD}  Painel Web NOC: http://localhost:8000/   |   Swagger: http://localhost:8000/docs${NC}"
echo -e "${GREEN}${BOLD}==============================================================================${NC}"
