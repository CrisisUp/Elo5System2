import json
from datetime import datetime, timezone
from sqlalchemy.orm import Session
from database import EventLog, NfcCredential, RackState, RoomState
from schemas import Elo5Event


def get_or_create_room_state(db: Session) -> RoomState:
    state = db.query(RoomState).filter(RoomState.id == 1).first()
    if not state:
        state = RoomState(id=1, occupancy_count=0, pir_motion=False)
        db.add(state)
        db.commit()
        db.refresh(state)
    return state


def get_or_create_rack_state(db: Session) -> RackState:
    state = db.query(RackState).filter(RackState.id == 1).first()
    if not state:
        state = RackState(id=1, rack_door_open=False, alarm_door_open=False)
        db.add(state)
        db.commit()
        db.refresh(state)
    return state


def check_permission(db: Session, uid_nfc: str, zone: str) -> dict:
    cred = db.query(NfcCredential).filter(NfcCredential.uid_nfc == uid_nfc).first()
    if not cred or not cred.is_active:
        return {"uid_nfc": uid_nfc, "zone": zone, "allowed": False, "reason": "CREDENTIAL_NOT_FOUND_OR_INACTIVE"}
    if zone == "rack_01":
        allowed = bool(cred.can_access_rack)
    else:
        allowed = bool(cred.can_enter_room)
    return {
        "uid_nfc": uid_nfc,
        "zone": zone,
        "owner_name": cred.owner_name,
        "allowed": allowed,
        "reason": "ALLOWED" if allowed else "SCOPE_NOT_ALLOWED",
    }


def process_event(db: Session, event: Elo5Event) -> EventLog:
    existing = db.query(EventLog).filter(EventLog.event_id == event.event_id).first()
    if existing:
        return existing

    room = get_or_create_room_state(db)
    rack = get_or_create_rack_state(db)
    now = datetime.now(timezone.utc)

    if event.session.session_id:
        room.active_session_id = event.session.session_id

    if event.event_type == "NFC_ACCESS_EVALUATED" and event.data.get("authorized"):
        room.last_room_uid = event.session.uid_nfc
        if event.session.occupancy_count is not None:
            room.occupancy_count = event.session.occupancy_count
        else:
            room.occupancy_count += 1

    elif event.event_type == "PIR_MOTION_CHANGED":
        room.pir_motion = bool(event.data.get("pir_motion", False))

    elif event.event_type == "RACK_ACCESS_EVALUATED" and event.data.get("authorized"):
        rack.rack_operator_uid = event.session.uid_nfc_operador

    elif event.event_type == "RACK_DOOR_CHANGED":
        door_open = bool(event.data.get("rack_door_open", False))
        rack.rack_door_open = door_open
        if not door_open:
            rack.alarm_door_open = False
            rack.last_distance_cm = None
        elif "alarm_door_open" in event.data:
            rack.alarm_door_open = bool(event.data["alarm_door_open"])

    elif event.event_type == "RACK_PROXIMITY_MEASURED":
        if "distance_cm" in event.data:
            rack.last_distance_cm = float(event.data["distance_cm"])

    elif event.event_type in ("EXIT_BUTTON_PRESSED", "RACK_DOOR_LEFT_OPEN_ON_EXIT"):
        if event.session.occupancy_count is not None:
            room.occupancy_count = max(0, event.session.occupancy_count)
        elif room.occupancy_count > 0:
            room.occupancy_count -= 1

        payload_door_open = bool(event.data.get("rack_door_open", False))
        door_still_open = rack.rack_door_open or payload_door_open
        rack.rack_door_open = door_still_open
        if door_still_open:
            rack.alarm_door_open = True
            event.severity = "ALERT"

    elif event.event_type == "SESSION_RESET_COMPLETED":
        room.occupancy_count = 0
        room.pir_motion = False
        room.active_session_id = None
        room.last_room_uid = None
        rack.rack_operator_uid = None
        if not bool(event.data.get("rack_door_open", False)):
            rack.rack_door_open = False
            rack.alarm_door_open = False

    room.updated_at = now
    rack.updated_at = now

    log_entry = EventLog(
        event_id=event.event_id,
        timestamp=event.timestamp,
        stage=event.stage,
        event_type=event.event_type,
        severity=event.severity,
        zone=event.source.zone,
        session_id=event.session.session_id,
        uid_nfc=event.session.uid_nfc,
        uid_nfc_operador=event.session.uid_nfc_operador,
        uid_nfc_saindo=event.session.uid_nfc_saindo,
        payload_json=json.dumps(event.model_dump(), ensure_ascii=False),
    )
    db.add(log_entry)
    db.commit()
    db.refresh(log_entry)
    return log_entry
