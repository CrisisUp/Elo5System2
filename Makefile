SHELL := /bin/bash
.DEFAULT_GOAL := help

-include .env
export

CERTS_DIR        := mosquitto/config/certs
CONFIG_DIR       := mosquitto/config
PWFILE           := $(CONFIG_DIR)/pwfile
SECRETS_FILE     := firmware/include/secrets.h
SECRETS_EXAMPLE  := firmware/include/secrets.h.example

MQTT_HEALTH_USER ?= healthcheck
MQTT_HEALTH_PASS ?= SenhaForteHealth123
MQTT_API_USER    ?= elo5api
MQTT_API_PASSWORD?= SenhaForteApi123
MQTT_SIM_USER    ?= elo5sim
MQTT_SIM_PASSWORD?= SenhaForteSim123
MQTT_ESP_USER    ?= esp32_cpd_01
MQTT_ESP_PASS    ?= SenhaForteEsp123

SIM_UID          ?= 04A1B2C3D4
SIM_DELAY        ?= 1
CERT_DAYS        ?= 365

.PHONY: help setup certs pwfile esp32-secrets up down restart logs sim health test-e2e clean-certs

help: ## Exibe os comandos disponíveis no Makefile
	@echo "Uso: make <alvo> [VAR=valor]"
	@echo ""
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2}'

setup: certs pwfile esp32-secrets ## Provisiona certificados mTLS, pwfile e injeta PEMs no firmware/include/secrets.h
	@echo "[OK] Ambiente completo provisionado (Mosquitto + Elo5api + ESP32 secrets.h)."

certs: ## Gera CA, certificado do servidor Mosquitto (com SAN) e clientes (api, sim, esp32, client)
	@mkdir -p $(CERTS_DIR)
	@echo "[1/3] Gerando Autoridade Certificadora (CA)..."
	@openssl req -new -x509 -days $(CERT_DAYS) -nodes \
		-subj "/C=BR/ST=SP/L=SaoPaulo/O=Elo5System/CN=Elo5-Root-CA" \
		-keyout $(CERTS_DIR)/ca.key \
		-out $(CERTS_DIR)/ca.crt 2>/dev/null

	@echo "[2/3] Gerando certificado do broker Mosquitto com SAN (mosquitto, localhost, 127.0.0.1)..."
	@openssl req -new -nodes \
		-subj "/C=BR/ST=SP/L=SaoPaulo/O=Elo5System/CN=mosquitto" \
		-keyout $(CERTS_DIR)/server.key \
		-out $(CERTS_DIR)/server.csr 2>/dev/null
	@printf "subjectAltName=DNS:mosquitto,DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n" > $(CERTS_DIR)/server.ext
	@openssl x509 -req -in $(CERTS_DIR)/server.csr \
		-CA $(CERTS_DIR)/ca.crt -CAkey $(CERTS_DIR)/ca.key -CAcreateserial \
		-out $(CERTS_DIR)/server.crt -days $(CERT_DAYS) \
		-extfile $(CERTS_DIR)/server.ext 2>/dev/null

	@echo "[3/3] Gerando certificados mTLS dos clientes (api, sim, client, esp32)..."
	@for client in api sim client esp32; do \
		openssl req -new -nodes \
			-subj "/C=BR/ST=SP/L=SaoPaulo/O=Elo5System/CN=elo5-$$client" \
			-keyout $(CERTS_DIR)/$$client.key \
			-out $(CERTS_DIR)/$$client.csr 2>/dev/null; \
		printf "extendedKeyUsage=clientAuth\n" > $(CERTS_DIR)/$$client.ext; \
		openssl x509 -req -in $(CERTS_DIR)/$$client.csr \
			-CA $(CERTS_DIR)/ca.crt -CAkey $(CERTS_DIR)/ca.key -CAcreateserial \
			-out $(CERTS_DIR)/$$client.crt -days $(CERT_DAYS) \
			-extfile $(CERTS_DIR)/$$client.ext 2>/dev/null; \
	done

	@rm -f $(CERTS_DIR)/*.csr $(CERTS_DIR)/*.ext $(CERTS_DIR)/*.srl
	@chmod 644 $(CERTS_DIR)/*.crt $(CERTS_DIR)/*.key
	@echo "[OK] Certificados gerados em $(CERTS_DIR)."

pwfile: ## Cria o arquivo de usuários/senhas (pwfile) do Mosquitto via container
	@mkdir -p $(CONFIG_DIR)
	@rm -f $(PWFILE) && touch $(PWFILE)
	@docker run --rm -v "$(CURDIR)/$(CONFIG_DIR):/mosquitto/config" eclipse-mosquitto:2.0 \
		sh -c "mosquitto_passwd -b -c /mosquitto/config/pwfile '$(MQTT_HEALTH_USER)' '$(MQTT_HEALTH_PASS)' && \
		       mosquitto_passwd -b /mosquitto/config/pwfile '$(MQTT_API_USER)' '$(MQTT_API_PASSWORD)' && \
		       mosquitto_passwd -b /mosquitto/config/pwfile '$(MQTT_SIM_USER)' '$(MQTT_SIM_PASSWORD)' && \
		       mosquitto_passwd -b /mosquitto/config/pwfile '$(MQTT_ESP_USER)' '$(MQTT_ESP_PASS)'"
	@chmod 644 $(PWFILE)
	@echo "[OK] Arquivo $(PWFILE) gerado com usuários: $(MQTT_HEALTH_USER), $(MQTT_API_USER), $(MQTT_SIM_USER), $(MQTT_ESP_USER)."

esp32-secrets: ## Injeta ca.crt, esp32.crt, esp32.key e credenciais MQTT em firmware/include/secrets.h
	@bash scripts/inject_esp32_secrets.sh "$(CERTS_DIR)" "$(SECRETS_FILE)" "$(SECRETS_EXAMPLE)"

up: ## Constrói e sobe Mosquitto + Elo5api em background
	docker compose up -d --build

down: ## Derruba os containers da stack
	docker compose down

restart: ## Reinicia os serviços da stack
	docker compose restart

logs: ## Acompanha os logs da Elo5api e do Mosquitto
	docker compose logs -f elo5api mosquitto

sim: ## Dispara uma execução completa do simulador (use SIM_UID=... SIM_DELAY=...)
	docker compose --profile sim run --rm elo5-sim --uid $(SIM_UID) --delay $(SIM_DELAY)

health: ## Verifica status de saúde da Elo5api e últimos estados gravados
	@curl -s http://localhost:8000/api/v1/health | python3 -m json.tool
	@echo "--- Estado da Sala ---"
	@curl -s http://localhost:8000/api/v1/state/room | python3 -m json.tool
	@echo "--- Estado do Rack ---"
	@curl -s http://localhost:8000/api/v1/state/rack | python3 -m json.tool

clean-certs: ## Remove certificados e pwfile gerados localmente
	rm -rf $(CERTS_DIR) $(PWFILE)
	@echo "[OK] Certificados e pwfile removidos."

test-e2e: ## Executa a suite completa de validacao ponta a ponta (8 etapas)
	./scripts/test_e2e.sh
