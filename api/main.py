import asyncio
import json
import os
import secrets
import ssl
from contextlib import asynccontextmanager
from pathlib import Path
from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, status
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError
from sqlalchemy.orm import Session
import aiomqtt

from database import EventLog, NfcCredential, SessionLocal, init_db
from schemas import CredentialCreate, CredentialResponse, Elo5Event
from service import (
    check_permission,
    get_or_create_rack_state,
    get_or_create_room_state,
    process_event,
)


def _env_bool(name: str, default: bool = False) -> bool:
    val = os.getenv(name)
    if val is None:
        return default
    return val.strip().lower() in ("1", "true", "yes", "on")


MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC_WILDCARD = os.getenv("MQTT_TOPIC_WILDCARD", "elo5/#")

MQTT_USERNAME = os.getenv("MQTT_USERNAME") or None
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD") or None

MQTT_TLS_ENABLED = _env_bool("MQTT_TLS_ENABLED", default=False)
MQTT_CA_CERT = os.getenv("MQTT_CA_CERT") or None
MQTT_CLIENT_CERT = os.getenv("MQTT_CLIENT_CERT") or None
MQTT_CLIENT_KEY = os.getenv("MQTT_CLIENT_KEY") or None
MQTT_TLS_INSECURE = _env_bool("MQTT_TLS_INSECURE", default=False)

# Chave opcional para proteger endpoints mutáveis (POST/DELETE) via header X-API-Key
API_ADMIN_KEY = os.getenv("API_ADMIN_KEY") or None

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"

mqtt_connected = False


def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


def require_api_key(x_api_key: str | None = Header(default=None, alias="X-API-Key")) -> None:
    """Exige X-API-Key válido em rotas de escrita caso API_ADMIN_KEY esteja configurado."""
    if not API_ADMIN_KEY:
        return
    if not x_api_key or not secrets.compare_digest(x_api_key, API_ADMIN_KEY):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="X-API-Key ausente ou inválido.",
        )


def build_mqtt_tls_context() -> ssl.SSLContext | None:
    if not MQTT_TLS_ENABLED:
        return None

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)

    if MQTT_TLS_INSECURE:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    else:
        context.check_hostname = True
        context.verify_mode = ssl.CERT_REQUIRED
        if MQTT_CA_CERT:
            context.load_verify_locations(cafile=MQTT_CA_CERT)
        else:
            context.load_default_certs()

    if MQTT_CLIENT_CERT or MQTT_CLIENT_KEY:
        if not (MQTT_CLIENT_CERT and MQTT_CLIENT_KEY):
            raise ValueError("Para mTLS, informe tanto MQTT_CLIENT_CERT quanto MQTT_CLIENT_KEY.")
        context.load_cert_chain(certfile=MQTT_CLIENT_CERT, keyfile=MQTT_CLIENT_KEY)

    return context


async def mqtt_consumer_loop() -> None:
    global mqtt_connected

    try:
        tls_context = build_mqtt_tls_context()
    except Exception as exc:
        print(f"[MQTT] Erro fatal de configuração TLS/mTLS: {exc}")
        return

    while True:
        try:
            async with aiomqtt.Client(
                hostname=MQTT_BROKER,
                port=MQTT_PORT,
                username=MQTT_USERNAME,
                password=MQTT_PASSWORD,
                tls_context=tls_context,
            ) as client:
                mqtt_connected = True
                await client.subscribe(MQTT_TOPIC_WILDCARD, qos=1)
                async for message in client.messages:
                    try:
                        raw_payload = message.payload.decode("utf-8")
                        parsed = json.loads(raw_payload)
                        event = Elo5Event.model_validate(parsed)
                        db = SessionLocal()
                        try:
                            process_event(db, event)
                        finally:
                            db.close()
                    except (UnicodeDecodeError, json.JSONDecodeError, ValidationError) as exc:
                        print(f"[MQTT] Payload inválido no tópico {message.topic}: {exc}")
        except asyncio.CancelledError:
            mqtt_connected = False
            raise
        except Exception as exc:
            mqtt_connected = False
            print(f"[MQTT] Conexão falhou ({exc}). Tentando novamente em 3s...")
            await asyncio.sleep(3)


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    task = asyncio.create_task(mqtt_consumer_loop())
    try:
        yield
    finally:
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass


app = FastAPI(title="Elo5api", version="1.1.0", lifespan=lifespan)


@app.middleware("http")
async def add_security_headers(request: Request, call_next):
    response = await call_next(request)
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
    response.headers["Permissions-Policy"] = "geolocation=(), microphone=(), camera=()"
    # CSP estrito sem scripts/estilos inline (apenas assets locais de /static)
    if not request.url.path.startswith(("/docs", "/redoc", "/openapi.json")):
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; "
            "script-src 'self'; "
            "style-src 'self'; "
            "img-src 'self' data:; "
            "connect-src 'self'; "
            "frame-ancestors 'none'; "
            "base-uri 'self'; "
            "form-action 'self'"
        )
    return response


if STATIC_DIR.exists():
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


@app.get("/", include_in_schema=False)
@app.get("/dashboard", include_in_schema=False)
def serve_dashboard():
    index_file = STATIC_DIR / "index.html"
    if not index_file.exists():
        return JSONResponse({"detail": "Dashboard não encontrado."}, status_code=404)
    return FileResponse(str(index_file), media_type="text/html; charset=utf-8")


@app.get("/api/v1/health")
def health():
    return {
        "status": "ok",
        "security": {
            "api_key_required_for_writes": bool(API_ADMIN_KEY),
        },
        "mqtt": {
            "broker": MQTT_BROKER,
            "port": MQTT_PORT,
            "connected": mqtt_connected,
            "topic": MQTT_TOPIC_WILDCARD,
            "tls_enabled": MQTT_TLS_ENABLED,
            "mtls_enabled": bool(MQTT_CLIENT_CERT and MQTT_CLIENT_KEY),
            "auth_enabled": bool(MQTT_USERNAME),
        },
    }


@app.get("/api/v1/credentials", response_model=list[CredentialResponse])
def list_credentials(db: Session = Depends(get_db)):
    return db.query(NfcCredential).order_by(NfcCredential.uid_nfc.asc()).all()


@app.post(
    "/api/v1/credentials",
    response_model=CredentialResponse,
    status_code=201,
    dependencies=[Depends(require_api_key)],
)
def upsert_credential(payload: CredentialCreate, db: Session = Depends(get_db)):
    uid_clean = payload.uid_nfc.strip().upper()
    cred = db.query(NfcCredential).filter(NfcCredential.uid_nfc == uid_clean).first()
    if cred:
        cred.owner_name = payload.owner_name.strip()
        cred.can_enter_room = payload.can_enter_room
        cred.can_access_rack = payload.can_access_rack
        cred.is_active = payload.is_active
    else:
        cred = NfcCredential(
            uid_nfc=uid_clean,
            owner_name=payload.owner_name.strip(),
            can_enter_room=payload.can_enter_room,
            can_access_rack=payload.can_access_rack,
            is_active=payload.is_active,
        )
        db.add(cred)
    db.commit()
    db.refresh(cred)
    return cred


@app.get("/api/v1/credentials/{uid_nfc}/permissions")
def get_credential_permissions(
    uid_nfc: str,
    zone: str = Query(default="sala_cpd"),
    db: Session = Depends(get_db),
):
    return check_permission(db, uid_nfc=uid_nfc.strip().upper(), zone=zone)


@app.post(
    "/api/v1/events/ingest",
    status_code=201,
    dependencies=[Depends(require_api_key)],
)
def ingest_event(event: Elo5Event, db: Session = Depends(get_db)):
    entry = process_event(db, event)
    return {"status": "ingested", "event_id": entry.event_id, "id": entry.id}


@app.get("/api/v1/state/room")
def get_room_state(db: Session = Depends(get_db)):
    room = get_or_create_room_state(db)
    return {
        "active_session_id": room.active_session_id,
        "last_room_uid": room.last_room_uid,
        "occupancy_count": room.occupancy_count,
        "pir_motion": room.pir_motion,
        "updated_at": room.updated_at.isoformat(),
    }


@app.get("/api/v1/state/rack")
def get_rack_state(db: Session = Depends(get_db)):
    rack = get_or_create_rack_state(db)
    return {
        "rack_operator_uid": rack.rack_operator_uid,
        "rack_door_open": rack.rack_door_open,
        "alarm_door_open": rack.alarm_door_open,
        "last_distance_cm": rack.last_distance_cm,
        "updated_at": rack.updated_at.isoformat(),
    }


@app.get("/api/v1/logs")
def list_logs(
    severity: str | None = Query(default=None),
    stage: str | None = Query(default=None),
    zone: str | None = Query(default=None),
    limit: int = Query(default=50, ge=1, le=500),
    db: Session = Depends(get_db),
):
    query = db.query(EventLog)
    if severity:
        query = query.filter(EventLog.severity == severity)
    if stage:
        query = query.filter(EventLog.stage == stage)
    if zone:
        query = query.filter(EventLog.zone == zone)
    rows = query.order_by(EventLog.id.desc()).limit(limit).all()
    return [
        {
            "id": r.id,
            "event_id": r.event_id,
            "timestamp": r.timestamp,
            "stage": r.stage,
            "event_type": r.event_type,
            "severity": r.severity,
            "zone": r.zone,
            "session_id": r.session_id,
            "uid_nfc": r.uid_nfc,
            "uid_nfc_operador": r.uid_nfc_operador,
            "uid_nfc_saindo": r.uid_nfc_saindo,
            "payload": json.loads(r.payload_json),
        }
        for r in rows
    ]
