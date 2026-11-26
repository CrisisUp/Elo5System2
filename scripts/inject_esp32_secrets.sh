#!/usr/bin/env bash
set -euo pipefail

CERTS_DIR="${1:-mosquitto/config/certs}"
SECRETS_FILE="${2:-firmware/include/secrets.h}"
EXAMPLE_FILE="${3:-firmware/include/secrets.h.example}"

CA_CRT="${CERTS_DIR}/ca.crt"
ESP_CRT="${CERTS_DIR}/esp32.crt"
ESP_KEY="${CERTS_DIR}/esp32.key"

for f in "$CA_CRT" "$ESP_CRT" "$ESP_KEY"; do
  if [[ ! -f "$f" ]]; then
    echo "[ERRO] Arquivo obrigatório não encontrado: $f" >&2
    echo "       Execute 'make certs' ou 'make setup' antes de injetar os segredos." >&2
    exit 1
  fi
done

if [[ ! -f "$SECRETS_FILE" ]]; then
  if [[ -f "$EXAMPLE_FILE" ]]; then
    cp "$EXAMPLE_FILE" "$SECRETS_FILE"
    echo "[INFO] Criado $SECRETS_FILE a partir de $EXAMPLE_FILE."
  else
    echo "[ERRO] Nem $SECRETS_FILE nem $EXAMPLE_FILE existem." >&2
    exit 1
  fi
fi

# Atualiza também MQTT_BROKER / USER / PASS se variáveis estiverem definidas no ambiente (.env)
python3 - "$SECRETS_FILE" "$CA_CRT" "$ESP_CRT" "$ESP_KEY" <<'PY'
import os
import re
import sys

secrets_path, ca_path, crt_path, key_path = sys.argv[1:5]

with open(secrets_path, "r", encoding="utf-8") as f:
    content = f.read()

with open(ca_path, "r", encoding="utf-8") as f:
    ca_pem = f.read().strip()

with open(crt_path, "r", encoding="utf-8") as f:
    crt_pem = f.read().strip()

with open(key_path, "r", encoding="utf-8") as f:
    key_pem = f.read().strip()

def replace_raw_literal(text: str, var_name: str, pem_body: str) -> str:
    pattern = re.compile(
        rf'(static\s+constexpr\s+const\s+char\*\s+{re.escape(var_name)}\s*=\s*R"EOF\().*?(\)EOF";)',
        re.DOTALL,
    )
    if not pattern.search(text):
        raise RuntimeError(f"Constante {var_name} não encontrada em {secrets_path}")
    return pattern.sub(lambda m: f"{m.group(1)}\n{pem_body}\n{m.group(2)}", text, count=1)

content = replace_raw_literal(content, "SECRET_CA_CERT_PEM", ca_pem)
content = replace_raw_literal(content, "SECRET_CLIENT_CERT_PEM", crt_pem)
content = replace_raw_literal(content, "SECRET_CLIENT_KEY_PEM", key_pem)

# Opcionalmente sincroniza credenciais MQTT se exportadas pelo Makefile / .env
esp_user = os.getenv("MQTT_ESP_USER")
esp_pass = os.getenv("MQTT_ESP_PASS")
if esp_user:
    content = re.sub(
        r'(static\s+constexpr\s+const\s+char\*\s+SECRET_MQTT_USERNAME\s*=\s*")[^"]*(";)',
        rf"\g<1>{esp_user}\g<2>",
        content,
        count=1,
    )
if esp_pass:
    content = re.sub(
        r'(static\s+constexpr\s+const\s+char\*\s+SECRET_MQTT_PASSWORD\s*=\s*")[^"]*(";)',
        rf"\g<1>{esp_pass}\g<2>",
        content,
        count=1,
    )

with open(secrets_path, "w", encoding="utf-8") as f:
    f.write(content)

print(f"[OK] Certificados ca.crt, esp32.crt e esp32.key injetados com sucesso em {secrets_path}.")
PY
