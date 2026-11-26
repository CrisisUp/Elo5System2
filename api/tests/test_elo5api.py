import os
import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker

import tempfile
os.environ["DATABASE_URL"] = f"sqlite:///{tempfile.gettempdir()}/elo5_pytest_boot.db"
os.environ["MQTT_BROKER"] = "127.0.0.1"
os.environ.setdefault("MQTT_PORT", "1883")

import database  # noqa: E402
from main import app, get_db  # noqa: E402


@pytest.fixture()
def client(tmp_path):
    db_file = tmp_path / "test_elo5.db"
    test_engine = create_engine(
        f"sqlite:///{db_file}",
        connect_args={"check_same_thread": False},
    )
    TestingSessionLocal = sessionmaker(
        autocommit=False,
        autoflush=False,
        bind=test_engine,
    )

    database.Base.metadata.create_all(bind=test_engine)

    def override_get_db():
        db = TestingSessionLocal()
        try:
            yield db
        finally:
            db.close()

    app.dependency_overrides[get_db] = override_get_db

    with TestClient(app) as test_client:
        yield test_client

    app.dependency_overrides.clear()
    database.Base.metadata.drop_all(bind=test_engine)


def build_event(
    event_id: str,
    stage: str,
    event_type: str,
    zone: str = "sala_cpd",
    severity: str = "INFO",
    session_id: str = "sess_20261126_01",
    uid_nfc: str | None = "04A1B2C3D4",
    uid_nfc_operador: str | None = None,
    uid_nfc_saindo: str | None = None,
    occupancy_count: int | None = 1,
    data: dict | None = None,
) -> dict:
    return {
        "event_id": event_id,
        "timestamp": "2026-11-26T00:24:00.000-03:00",
        "source": {
            "device_id": "esp32-cpd-01",
            "reader": "nfc_sala_entrada",
            "sensor": "sensor_teste",
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


def test_health_endpoint(client: TestClient):
    resp = client.get("/api/v1/health")
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ok"
    assert "mqtt" in body


def test_credentials_crud_and_permissions(client: TestClient):
    create_resp = client.post(
        "/api/v1/credentials",
        json={
            "uid_nfc": "04A1B2C3D4",
            "owner_name": "Operador CPD",
            "can_enter_room": True,
            "can_access_rack": True,
            "is_active": True,
        },
    )
    assert create_resp.status_code in (200, 201)

    perm_room = client.get("/api/v1/credentials/04A1B2C3D4/permissions?zone=sala_cpd")
    assert perm_room.status_code == 200
    assert perm_room.json()["allowed"] is True

    perm_rack = client.get("/api/v1/credentials/04A1B2C3D4/permissions?zone=rack_01")
    assert perm_rack.status_code == 200
    assert perm_rack.json()["allowed"] is True


def test_event_id_idempotency(client: TestClient):
    ev = build_event(
        event_id="evt_idempotencia_001",
        stage="1_acesso_sala",
        event_type="NFC_ACCESS_EVALUATED",
        zone="sala_cpd",
        uid_nfc="04A1B2C3D4",
        occupancy_count=1,
        data={"card_detected": True, "authorized": True, "decision": "PERMITIDO"},
    )

    first = client.post("/api/v1/events/ingest", json=ev)
    assert first.status_code in (200, 201)

    second = client.post("/api/v1/events/ingest", json=ev)
    assert second.status_code in (200, 201)

    logs_resp = client.get("/api/v1/logs?limit=50")
    assert logs_resp.status_code == 200
    items = logs_resp.json()
    matching = [item for item in items if item["event_id"] == "evt_idempotencia_001"]
    assert len(matching) == 1


def test_rack_door_left_open_on_exit_triggers_and_clears_alert(client: TestClient):
    session_id = "sess_alerta_rack_01"

    r1 = client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_flow_01",
            stage="1_acesso_sala",
            event_type="NFC_ACCESS_EVALUATED",
            zone="sala_cpd",
            session_id=session_id,
            uid_nfc="04A1B2C3D4",
            occupancy_count=1,
            data={"authorized": True, "decision": "PERMITIDO"},
        ),
    )
    assert r1.status_code in (200, 201)

    r2 = client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_flow_02",
            stage="2_ocupacao_e_rack",
            event_type="RACK_DOOR_CHANGED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_operador="04A1B2C3D4",
            occupancy_count=1,
            data={"rack_door_open": True, "state": "ABERTA"},
        ),
    )
    assert r2.status_code in (200, 201)

    rack_open_state = client.get("/api/v1/state/rack").json()
    assert rack_open_state["rack_door_open"] is True
    assert rack_open_state["alarm_door_open"] is False

    r3 = client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_flow_03",
            stage="3_saida_e_reset",
            event_type="EXIT_BUTTON_PRESSED",
            zone="rack_01",
            severity="ALERT",
            session_id=session_id,
            uid_nfc_saindo="04A1B2C3D4",
            occupancy_count=0,
            data={
                "exit_button_pressed": True,
                "rack_door_open": True,
                "alarm_door_open": True,
                "room_reset_completed": False,
            },
        ),
    )
    assert r3.status_code in (200, 201)

    rack_alarm_state = client.get("/api/v1/state/rack").json()
    assert rack_alarm_state["rack_door_open"] is True
    assert rack_alarm_state["alarm_door_open"] is True

    alerts_resp = client.get("/api/v1/logs?severity=ALERT")
    assert alerts_resp.status_code == 200
    assert any(item["event_id"] == "evt_flow_03" for item in alerts_resp.json())

    r4 = client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_flow_04",
            stage="2_ocupacao_e_rack",
            event_type="RACK_DOOR_CHANGED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_operador="04A1B2C3D4",
            occupancy_count=0,
            data={
                "rack_door_open": False,
                "alarm_door_open": False,
                "state": "FECHADA",
            },
        ),
    )
    assert r4.status_code in (200, 201)

    rack_closed_state = client.get("/api/v1/state/rack").json()
    assert rack_closed_state["rack_door_open"] is False
    assert rack_closed_state["alarm_door_open"] is False

    r5 = client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_flow_05",
            stage="3_saida_e_reset",
            event_type="SESSION_RESET_COMPLETED",
            zone="sala_cpd",
            session_id=session_id,
            uid_nfc_saindo="04A1B2C3D4",
            occupancy_count=0,
            data={
                "rack_door_open": False,
                "pir_motion": False,
                "room_reset_completed": True,
            },
        ),
    )
    assert r5.status_code in (200, 201)

    room_final_state = client.get("/api/v1/state/room").json()
    assert room_final_state["occupancy_count"] == 0
    assert room_final_state["pir_motion"] is False


def test_exit_button_infers_rack_door_alarm_from_persisted_state(client: TestClient):
    session_id = "sess_infer_alarm_01"

    client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_infer_01",
            stage="1_acesso_sala",
            event_type="NFC_ACCESS_EVALUATED",
            zone="sala_cpd",
            session_id=session_id,
            uid_nfc="04A1B2C3D4",
            occupancy_count=1,
            data={"authorized": True, "decision": "PERMITIDO"},
        ),
    )

    client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_infer_02",
            stage="2_ocupacao_e_rack",
            event_type="RACK_DOOR_CHANGED",
            zone="rack_01",
            session_id=session_id,
            uid_nfc_operador="04A1B2C3D4",
            occupancy_count=1,
            data={"rack_door_open": True, "state": "ABERTA"},
        ),
    )

    r3 = client.post(
        "/api/v1/events/ingest",
        json=build_event(
            event_id="evt_infer_03",
            stage="3_saida_e_reset",
            event_type="EXIT_BUTTON_PRESSED",
            zone="sala_cpd",
            severity="INFO",
            session_id=session_id,
            uid_nfc_saindo="04A1B2C3D4",
            occupancy_count=0,
            data={"exit_button_pressed": True},
        ),
    )
    assert r3.status_code in (200, 201)

    rack_state = client.get("/api/v1/state/rack").json()
    assert rack_state["rack_door_open"] is True
    assert rack_state["alarm_door_open"] is True


def test_dashboard_html_and_security_headers(client: TestClient):
    resp = client.get("/")
    assert resp.status_code == 200
    assert "text/html" in resp.headers["content-type"]
    assert "Elo5System" in resp.text
    assert resp.headers.get("X-Frame-Options") == "DENY"
    assert resp.headers.get("X-Content-Type-Options") == "nosniff"
    assert "script-src 'self'" in resp.headers.get("Content-Security-Policy", "")


def test_api_key_protection_when_configured(client: TestClient, monkeypatch):
    import main as main_mod

    monkeypatch.setattr(main_mod, "API_ADMIN_KEY", "segredo-super-forte")

    unauth = client.post(
        "/api/v1/credentials",
        json={
            "uid_nfc": "0499887766",
            "owner_name": "Sem Chave",
            "can_enter_room": True,
            "can_access_rack": False,
            "is_active": True,
        },
    )
    assert unauth.status_code == 401

    auth = client.post(
        "/api/v1/credentials",
        headers={"X-API-Key": "segredo-super-forte"},
        json={
            "uid_nfc": "0499887766",
            "owner_name": "Com Chave",
            "can_enter_room": True,
            "can_access_rack": False,
            "is_active": True,
        },
    )
    assert auth.status_code == 201
