SHELL := /bin/bash
PIO   ?= pio
WOKWI ?= wokwi-cli

.PHONY: help build build-porta build-rack build-sim lint-wokwi test-e2e up down logs clean

help:
	@echo "Alvos disponíveis — Elo5System:"
	@echo "  make build        - Compila todos os 4 firmwares (porta/rack hardware + sim)"
	@echo "  make build-porta  - Compila porta_hardware e porta_wokwi_sim"
	@echo "  make build-rack   - Compila rack_hardware e rack_wokwi_sim"
	@echo "  make lint-wokwi   - Valida diagramas Wokwi separados (porta e rack)"
	@echo "  make test-e2e     - Executa validações de firmware, lint Wokwi e testes Python"
	@echo "  make up           - Sobe a stack Docker (Mosquitto mTLS, FastAPI, Postgres, InfluxDB, Grafana)"
	@echo "  make down         - Encerra a stack Docker"
	@echo "  make logs         - Exibe logs em tempo real dos containers"

build:
	cd firmware && $(PIO) run -e porta_hardware -e porta_wokwi_sim -e rack_hardware -e rack_wokwi_sim

build-porta:
	cd firmware && $(PIO) run -e porta_hardware -e porta_wokwi_sim

build-rack:
	cd firmware && $(PIO) run -e rack_hardware -e rack_wokwi_sim

build-sim:
	cd firmware && $(PIO) run -e porta_wokwi_sim -e rack_wokwi_sim

lint-wokwi:
	$(WOKWI) lint firmware/wokwi/porta
	$(WOKWI) lint firmware/wokwi/rack

test-e2e: build lint-wokwi
	pytest -q

up:
	docker compose up -d --build

down:
	docker compose down

logs:
	docker compose logs -f --tail=100

clean:
	cd firmware && $(PIO) run -t clean
