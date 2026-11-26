from typing import Any
from pydantic import BaseModel, Field


class EventSource(BaseModel):
    device_id: str
    reader: str | None = None
    sensor: str | None = None
    zone: str


class EventSession(BaseModel):
    session_id: str | None = None
    uid_nfc: str | None = None
    uid_nfc_operador: str | None = None
    uid_nfc_saindo: str | None = None
    occupancy_count: int | None = None


class Elo5Event(BaseModel):
    event_id: str
    timestamp: str
    source: EventSource
    stage: str
    event_type: str
    severity: str = "INFO"
    session: EventSession
    data: dict[str, Any] = Field(default_factory=dict)


class CredentialCreate(BaseModel):
    uid_nfc: str
    owner_name: str
    can_enter_room: bool = True
    can_access_rack: bool = False
    is_active: bool = True


class CredentialResponse(BaseModel):
    uid_nfc: str
    owner_name: str
    can_enter_room: bool
    can_access_rack: bool
    is_active: bool

    model_config = {"from_attributes": True}
