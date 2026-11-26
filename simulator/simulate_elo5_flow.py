import argparse
import json
import os
import ssl
import time
import uuid
from datetime import datetime, timezone
import paho.mqtt.client as mqtt


def env_bool(name: str, default: bool = False) -> bool:
    val = os.getenv(name)
    if val is None:
        return default
    return val.strip().lower() in ("1", "true", "yes", "on")


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def make_event(
    stage: str,
    event_type: str,
    zone: str,
    session_id: str,
    uid_nfc: str | None = None,
    uid_nfc_operador: str | None = None,
    uid_nfc_saindo: str | None = None,
    occupancy_count: int | None = None,
    severity: str = "INFO",
    device_id: str = "esp32-cpd-01",
    reader: str | None = None,
    sensor: str | None = None,
    data: dict | None = None,
) -> dict:
    short_id = uuid.uuid4().hex[:6]
    ts_compact = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    return {
        "event_id": f"evt_{ts_compact}_{short_id}",
        "timestamp": now_iso(),
        "source": {
            "device_id": device_id,
            "reader": reader,
            "sensor": sensor,
            "zone": zone,
        },
        "stage": stage,
        "event_type": event_type,
        "severity": severity,
        "session": {
            "session_id": session_id,
            "uid_nfc": uid_nfc,
            "uid_nfc_operador": uid_nfc_operador,
            "uid_nfc_saindo": uid_nfc_saindo,
            "occupancy_count": occupancy_count,
        },
        "data": data or {},
    }


def publish_event(client: mqtt.Client, topic: str, payload: dict, qos: int = 1, retain: bool = False) -> None:
    encoded = json.dumps(payload, ensure_ascii=False)
    info = client.publish(topic, payload=encoded, qos=qos, retain=retain)
    info.wait_for_publish(timeout=5.0)
    print(f"[{payload['stage']}] {topic} -> {payload['event_type']} (id={payload['event_id']})")


def configure_mqtt_security(
    client: mqtt.Client,
    username: str | None,
    password: str | None,
    tls_enabled: bool,
    cafile: str | None,
    certfile: str | None,
    keyfile: str | None,
    tls_insecure: bool,
) -> None:
    if username is not None:
        client.username_pw_set(username=username, password=password)

    use_tls = tls_enabled or bool(cafile or certfile or keyfile)
    if not use_tls:
        return

    if bool(certfile) ^ bool(keyfile):
        raise ValueError("Para mTLS, informe tanto --certfile (MQTT_CLIENT_CERT) quanto --keyfile (MQTT_CLIENT_KEY).")

    cert_reqs = ssl.CERT_NONE if tls_insecure else ssl.CERT_REQUIRED
    client.tls_set(
        ca_certs=cafile,
        certfile=certfile,
        keyfile=keyfile,
        cert_reqs=cert_reqs,
        tls_version=ssl.PROTOCOL_TLS_CLIENT,
    )
    if tls_insecure:
        client.tls_insecure_set(True)


def run_simulation(
    broker: str,
    port: int,
    uid_nfc: str,
    delay: float,
    username: str | None = None,
    password: str | None = None,
    tls_enabled: bool = False,
    cafile: str | None = None,
    certfile: str | None = None,
    keyfile: str | None = None,
    tls_insecure: bool = False,
) -> None:
    session_id = f"sess_{datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:4]}"

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"elo5-sim-{uuid.uuid4().hex[:8]}",
    )
    configure_mqtt_security(
        client=client,
        username=username,
        password=password,
        tls_enabled=tls_enabled,
        cafile=cafile,
        certfile=certfile,
        keyfile=keyfile,
        tls_insecure=tls_insecure,
    )

    client.connect(broker, port, keepalive=60)
    client.loop_start()

    try:
        ev_entry = make_event(
            stage="1_acesso_sala",
            event_type="NFC_ACCESS_EVALUATED",
            zone="sala_cpd",
            session_id=session_id,
            uid_nfc=uid_nfc,
            occupancy_count=1,
            reader="nfc_sala_entrada",
            data={
                "card_detected": True,
                "authorized": True,
                "decision": "PERMITIDO",
                "reason": "CREDENTIAL_VALID",
                "next_state": "ESPERA_MOVIMENTO_PIR",
            },
        )
        publish_event(client, "elo5/sala/acesso/nfc/evento", ev_entry, qos=1)
        time.sleep(delay)

        ev_pir = make_event(
            stage="2_ocupacao_e_rack",
            event_type="PIR_MOTION_CHANGED",
            zone="sala_cpd",
            session_id=session_id,
            uid_nfc=uid_nfc,
            occupancy_count=1,
            sensor="pir_sala_cpd",
            data={"pir_motion": True, "room_occupied": True},
        )
        publish_event(client, "elo5/sala/presenca/pir", ev_pir, qos=1)
        time.sleep(delay)

        ev_rack_auth = make_event(
            stage="2_ocupacao_e_rack",
            event_type="RACK_ACCESS_EVALUATED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_operador=uid_nfc,
            occupancy_count=1,
            reader="nfc_rack_01",
            data={"card_detected": True, "authorized": True, "decision": "LIBERADO"},
        )
        publish_event(client, "elo5/rack/acesso/nfc/evento", ev_rack_auth, qos=1)
        time.sleep(delay)

        ev_door_open = make_event(
            stage="2_ocupacao_e_rack",
            event_type="RACK_DOOR_CHANGED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_operador=uid_nfc,
            occupancy_count=1,
            sensor="reed_switch_rack_01",
            data={"rack_door_open": True, "state": "ABERTA"},
        )
        publish_event(client, "elo5/rack/porta/estado", ev_door_open, qos=1, retain=True)
        time.sleep(delay)

        for idx, dist in enumerate([64.0, 42.5, 29.0, 31.0]):
            ev_dist = make_event(
                stage="2_ocupacao_e_rack",
                event_type="RACK_PROXIMITY_MEASURED",
                zone="rack_01",
                session_id=session_id,
                uid_nfc_operador=uid_nfc,
                occupancy_count=1,
                sensor="hc_sr04_rack_01",
                data={
                    "rack_door_open": True,
                    "distance_cm": dist,
                    "sample_index": idx + 1,
                    "trigger": "THRESHOLD_OR_INTERVAL_T",
                },
            )
            publish_event(client, "elo5/rack/proximidade/cm", ev_dist, qos=0)
            time.sleep(delay)

        ev_exit_alert = make_event(
            stage="3_saida_e_reset",
            event_type="EXIT_BUTTON_PRESSED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_saindo=uid_nfc,
            occupancy_count=0,
            severity="ALERT",
            sensor="botao_saida_sala",
            data={
                "exit_button_pressed": True,
                "rack_door_open": True,
                "alarm_door_open": True,
                "room_reset_completed": False,
            },
        )
        publish_event(client, "elo5/sala/saida/botao", ev_exit_alert, qos=1)
        time.sleep(delay)

        ev_door_closed = make_event(
            stage="2_ocupacao_e_rack",
            event_type="RACK_DOOR_CHANGED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_operador=uid_nfc,
            occupancy_count=0,
            sensor="reed_switch_rack_01",
            data={"rack_door_open": False, "alarm_door_open": False, "state": "FECHADA"},
        )
        publish_event(client, "elo5/rack/porta/estado", ev_door_closed, qos=1, retain=True)
        time.sleep(delay)

        ev_reset = make_event(
            stage="3_saida_e_reset",
            event_type="SESSION_RESET_COMPLETED",
            zone="sala_cpd",
            session_id=session_id,
            uid_nfc_saindo=uid_nfc,
            occupancy_count=0,
            data={"rack_door_open": False, "pir_motion": False, "room_reset_completed": True},
        )
        publish_event(client, "elo5/sala/saida/botao", ev_reset, qos=1)
        print("\nSequência completa publicada com sucesso.")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Simulador de eventos MQTT do Elo5System")
    parser.add_argument("--broker", default=os.getenv("MQTT_BROKER", "localhost"))
    parser.add_argument("--port", type=int, default=int(os.getenv("MQTT_PORT", "1883")))
    parser.add_argument("--uid", default="04A1B2C3D4", help="UID do cartão NFC simulado")
    parser.add_argument("--delay", type=float, default=1.0, help="Intervalo em segundos entre eventos")
    parser.add_argument("--username", default=os.getenv("MQTT_USERNAME") or None)
    parser.add_argument("--password", default=os.getenv("MQTT_PASSWORD") or None)
    parser.add_argument("--tls", action="store_true", default=env_bool("MQTT_TLS_ENABLED", False))
    parser.add_argument("--cafile", default=os.getenv("MQTT_CA_CERT") or None)
    parser.add_argument("--certfile", default=os.getenv("MQTT_CLIENT_CERT") or None)
    parser.add_argument("--keyfile", default=os.getenv("MQTT_CLIENT_KEY") or None)
    parser.add_argument("--tls-insecure", action="store_true", default=env_bool("MQTT_TLS_INSECURE", False))
    args = parser.parse_args()

    run_simulation(
        broker=args.broker,
        port=args.port,
        uid_nfc=args.uid,
        delay=args.delay,
        username=args.username,
        password=args.password,
        tls_enabled=args.tls,
        cafile=args.cafile,
        certfile=args.certfile,
        keyfile=args.keyfile,
        tls_insecure=args.tls_insecure,
    )
