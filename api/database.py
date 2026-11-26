import os
from datetime import datetime, timezone
from sqlalchemy import Boolean, Column, DateTime, Float, Integer, String, Text, create_engine
from sqlalchemy.orm import declarative_base, sessionmaker

DATABASE_URL = os.getenv("DATABASE_URL", "sqlite:///./elo5system.db")

connect_args = {"check_same_thread": False} if DATABASE_URL.startswith("sqlite") else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args)
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)
Base = declarative_base()


class NfcCredential(Base):
    __tablename__ = "nfc_credentials"

    uid_nfc = Column(String(64), primary_key=True, index=True)
    owner_name = Column(String(120), nullable=False)
    can_enter_room = Column(Boolean, default=True, nullable=False)
    can_access_rack = Column(Boolean, default=False, nullable=False)
    is_active = Column(Boolean, default=True, nullable=False)


class RoomState(Base):
    __tablename__ = "room_state"

    id = Column(Integer, primary_key=True, default=1)
    active_session_id = Column(String(128), nullable=True)
    last_room_uid = Column(String(64), nullable=True)
    occupancy_count = Column(Integer, default=0, nullable=False)
    pir_motion = Column(Boolean, default=False, nullable=False)
    updated_at = Column(DateTime, default=lambda: datetime.now(timezone.utc), nullable=False)


class RackState(Base):
    __tablename__ = "rack_state"

    id = Column(Integer, primary_key=True, default=1)
    rack_operator_uid = Column(String(64), nullable=True)
    rack_door_open = Column(Boolean, default=False, nullable=False)
    alarm_door_open = Column(Boolean, default=False, nullable=False)
    last_distance_cm = Column(Float, nullable=True)
    updated_at = Column(DateTime, default=lambda: datetime.now(timezone.utc), nullable=False)


class EventLog(Base):
    __tablename__ = "event_logs"

    id = Column(Integer, primary_key=True, autoincrement=True)
    event_id = Column(String(128), unique=True, index=True, nullable=False)
    timestamp = Column(String(64), nullable=False)
    stage = Column(String(64), nullable=False, index=True)
    event_type = Column(String(64), nullable=False, index=True)
    severity = Column(String(32), nullable=False, index=True)
    zone = Column(String(64), nullable=False, index=True)
    session_id = Column(String(128), nullable=True, index=True)
    uid_nfc = Column(String(64), nullable=True)
    uid_nfc_operador = Column(String(64), nullable=True)
    uid_nfc_saindo = Column(String(64), nullable=True)
    payload_json = Column(Text, nullable=False)


def init_db() -> None:
    Base.metadata.create_all(bind=engine)
