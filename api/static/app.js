"use strict";

let currentSessionId = "sess_web_" + Date.now();
let eventCounter = 0;

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = String(text);
  return node;
}

function getApiHeaders() {
  const headers = { "Content-Type": "application/json" };
  const apiKeyInput = document.getElementById("api-key-input");
  if (apiKeyInput && apiKeyInput.value.trim()) {
    headers["X-API-Key"] = apiKeyInput.value.trim();
  }
  return headers;
}

function formatIsoShort(isoStr) {
  if (!isoStr) return "--";
  try {
    const d = new Date(isoStr);
    if (Number.isNaN(d.getTime())) return isoStr;
    return d.toLocaleTimeString("pt-BR", { hour12: false });
  } catch {
    return isoStr;
  }
}

async function fetchJson(url) {
  const res = await fetch(url, { cache: "no-store" });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

function renderHealth(health) {
  const mqttPill = document.getElementById("mqtt-pill");
  const mqttText = document.getElementById("mqtt-status-text");
  const tlsPill = document.getElementById("tls-pill");
  const authBadge = document.getElementById("auth-badge");

  const connected = Boolean(health.mqtt && health.mqtt.connected);
  mqttPill.className = `status-pill ${connected ? "pill-ok" : "pill-danger"}`;
  mqttText.textContent = connected
    ? `MQTT Online (${health.mqtt.broker}:${health.mqtt.port})`
    : "MQTT Desconectado";

  const tlsOn = Boolean(health.mqtt && health.mqtt.tls_enabled);
  const mtlsOn = Boolean(health.mqtt && health.mqtt.mtls_enabled);
  tlsPill.className = `status-pill ${tlsOn ? "pill-ok" : "pill-neutral"}`;
  tlsPill.textContent = mtlsOn ? "mTLS Ativo" : tlsOn ? "TLS Ativo" : "TCP Sem TLS";

  const keyReq = Boolean(health.security && health.security.api_key_required_for_writes);
  authBadge.className = `badge ${keyReq ? "badge-ok" : "badge-warn"}`;
  authBadge.textContent = keyReq ? "API Key Exigida" : "API Key Opcional";
}

function renderRoom(room) {
  const occ = Number(room.occupancy_count || 0);
  document.getElementById("room-occupancy").textContent = String(occ);

  const badge = document.getElementById("room-occupancy-badge");
  badge.className = `badge ${occ > 0 ? "badge-ok" : "badge-neutral"}`;
  badge.textContent = occ > 0 ? "Ocupada" : "Vazia";

  const pir = Boolean(room.pir_motion);
  document.getElementById("room-pir").textContent = pir ? "Movimento Detectado" : "Sem movimento";

  if (room.active_session_id) {
    currentSessionId = room.active_session_id;
  }
  document.getElementById("room-session").textContent = room.active_session_id || "--";
  document.getElementById("room-uid").textContent = room.last_room_uid || "--";
  document.getElementById("room-updated").textContent = formatIsoShort(room.updated_at);
}

function renderRack(rack) {
  const doorOpen = Boolean(rack.rack_door_open);
  const alarmOpen = Boolean(rack.alarm_door_open);

  const doorBadge = document.getElementById("rack-door-badge");
  doorBadge.className = `badge ${alarmOpen ? "badge-danger" : doorOpen ? "badge-warn" : "badge-ok"}`;
  doorBadge.textContent = doorOpen ? "Porta Aberta" : "Porta Fechada";

  const distEl = document.getElementById("rack-distance");
  distEl.textContent = "";
  if (rack.last_distance_cm !== null && rack.last_distance_cm !== undefined) {
    distEl.appendChild(document.createTextNode(`${Number(rack.last_distance_cm).toFixed(1)} `));
    distEl.appendChild(el("small", "", "cm"));
    const pct = Math.max(5, Math.min(100, (Number(rack.last_distance_cm) / 100) * 100));
    document.getElementById("distance-bar").style.width = `${pct}%`;
  } else {
    distEl.appendChild(document.createTextNode("-- "));
    distEl.appendChild(el("small", "", "cm"));
    document.getElementById("distance-bar").style.width = "0%";
  }

  document.getElementById("rack-alarm").textContent = alarmOpen ? "ALARME ATIVO" : "Normal";
  document.getElementById("rack-operator").textContent = rack.rack_operator_uid || "--";
  document.getElementById("rack-updated").textContent = formatIsoShort(rack.updated_at);

  const banner = document.getElementById("alarm-banner");
  const rackCard = document.getElementById("rack-card");
  if (alarmOpen) {
    banner.classList.remove("hidden");
    rackCard.classList.add("card-alert");
  } else {
    banner.classList.add("hidden");
    rackCard.classList.remove("card-alert");
  }
}

function renderCredentials(creds) {
  document.getElementById("cred-count").textContent = String(creds.length);
  const tbody = document.getElementById("cred-tbody");
  tbody.textContent = "";

  if (creds.length === 0) {
    const tr = el("tr");
    const td = el("td", "", "Nenhuma credencial cadastrada.");
    td.colSpan = 4;
    tr.appendChild(td);
    tbody.appendChild(tr);
    return;
  }

  for (const c of creds) {
    const tr = el("tr");
    tr.appendChild(el("td", "mono", c.uid_nfc));
    tr.appendChild(el("td", "", c.owner_name));

    const scopes = [];
    if (c.can_enter_room) scopes.push("Sala");
    if (c.can_access_rack) scopes.push("Rack");
    tr.appendChild(el("td", "", scopes.join(" + ") || "Nenhum"));

    const tdStatus = el("td");
    tdStatus.appendChild(
      el("span", `badge ${c.is_active ? "badge-ok" : "badge-danger"}`, c.is_active ? "Ativa" : "Inativa")
    );
    tr.appendChild(tdStatus);
    tbody.appendChild(tr);
  }
}

function renderLogs(logs) {
  const tbody = document.getElementById("logs-tbody");
  tbody.textContent = "";

  if (logs.length === 0) {
    const tr = el("tr");
    const td = el("td", "", "Nenhum evento encontrado para os filtros selecionados.");
    td.colSpan = 6;
    tr.appendChild(td);
    tbody.appendChild(tr);
    return;
  }

  for (const item of logs) {
    const tr = el("tr");
    tr.appendChild(el("td", "mono", formatIsoShort(item.timestamp)));

    const tdSev = el("td");
    const sevClass =
      item.severity === "ALERT" ? "badge-danger" : item.severity === "WARN" ? "badge-warn" : "badge-ok";
    tdSev.appendChild(el("span", `badge ${sevClass}`, item.severity));
    tr.appendChild(tdSev);

    tr.appendChild(el("td", "mono", item.stage));
    tr.appendChild(el("td", "mono", item.event_type));
    tr.appendChild(el("td", "mono", item.zone));

    const uid = item.uid_nfc || item.uid_nfc_operador || item.uid_nfc_saindo || "--";
    const extra = item.payload && item.payload.data ? JSON.stringify(item.payload.data) : "";
    tr.appendChild(el("td", "mono", `${uid} ${extra}`));
    tbody.appendChild(tr);
  }
}

async function refreshAll() {
  try {
    const sev = document.getElementById("filter-severity").value;
    const zone = document.getElementById("filter-zone").value;
    const params = new URLSearchParams({ limit: "30" });
    if (sev) params.set("severity", sev);
    if (zone) params.set("zone", zone);

    const [health, room, rack, creds, logs] = await Promise.all([
      fetchJson("/api/v1/health"),
      fetchJson("/api/v1/state/room"),
      fetchJson("/api/v1/state/rack"),
      fetchJson("/api/v1/credentials"),
      fetchJson(`/api/v1/logs?${params.toString()}`),
    ]);

    renderHealth(health);
    renderRoom(room);
    renderRack(rack);
    renderCredentials(creds);
    renderLogs(logs);
  } catch (err) {
    const mqttPill = document.getElementById("mqtt-pill");
    mqttPill.className = "status-pill pill-danger";
    document.getElementById("mqtt-status-text").textContent = `Erro API: ${err.message}`;
  }
}

async function triggerSimulatedStep(action) {
  const uid = (document.getElementById("sim-uid-input").value || "04A1B2C3D4").trim().toUpperCase();
  const feedback = document.getElementById("action-feedback");
  eventCounter += 1;

  if (action === "enter_room") {
    currentSessionId = `sess_web_${Date.now()}`;
  }

  const presets = {
    enter_room: {
      stage: "1_acesso_sala",
      event_type: "NFC_ACCESS_EVALUATED",
      zone: "sala_cpd",
      severity: "INFO",
      session: { session_id: currentSessionId, uid_nfc: uid, occupancy_count: 1 },
      data: { card_detected: true, authorized: true, decision: "PERMITIDO" },
    },
    pir_motion: {
      stage: "2_ocupacao_e_rack",
      event_type: "PIR_MOTION_CHANGED",
      zone: "sala_cpd",
      severity: "INFO",
      session: { session_id: currentSessionId, uid_nfc: uid, occupancy_count: 1 },
      data: { pir_motion: true, room_occupied: true },
    },
    open_rack: {
      stage: "2_ocupacao_e_rack",
      event_type: "RACK_DOOR_CHANGED",
      zone: "rack_01",
      severity: "WARN",
      session: { session_id: currentSessionId, uid_nfc_operador: uid, occupancy_count: 1 },
      data: { rack_door_open: true, state: "ABERTA" },
    },
    prox_read: {
      stage: "2_ocupacao_e_rack",
      event_type: "RACK_PROXIMITY_MEASURED",
      zone: "rack_01",
      severity: "INFO",
      session: { session_id: currentSessionId, uid_nfc_operador: uid, occupancy_count: 1 },
      data: { rack_door_open: true, distance_cm: 32.5, trigger: "WEB_SIM" },
    },
    exit_open: {
      stage: "3_saida_e_reset",
      event_type: "EXIT_BUTTON_PRESSED",
      zone: "rack_01",
      severity: "ALERT",
      session: { session_id: currentSessionId, uid_nfc_saindo: uid, occupancy_count: 0 },
      data: { exit_button_pressed: true, rack_door_open: true, alarm_door_open: true },
    },
    close_reset: {
      stage: "3_saida_e_reset",
      event_type: "SESSION_RESET_COMPLETED",
      zone: "sala_cpd",
      severity: "INFO",
      session: { session_id: currentSessionId, uid_nfc_saindo: uid, occupancy_count: 0 },
      data: { rack_door_open: false, pir_motion: false, room_reset_completed: true },
    },
  };

  const spec = presets[action];
  if (!spec) return;

  const payload = {
    event_id: `evt_web_${Date.now()}_${eventCounter}`,
    timestamp: new Date().toISOString(),
    source: { device_id: "web-noc-console", zone: spec.zone },
    stage: spec.stage,
    event_type: spec.event_type,
    severity: spec.severity,
    session: spec.session,
    data: spec.data,
  };

  try {
    const res = await fetch("/api/v1/events/ingest", {
      method: "POST",
      headers: getApiHeaders(),
      body: JSON.stringify(payload),
    });
    if (!res.ok) {
      const errBody = await res.json().catch(() => ({}));
      throw new Error(errBody.detail || `HTTP ${res.status}`);
    }
    feedback.textContent = `Evento ${spec.event_type} enviado com sucesso.`;
    await refreshAll();
  } catch (err) {
    feedback.textContent = `Falha ao enviar evento: ${err.message}`;
  }
}

async function handleCredentialSubmit(ev) {
  ev.preventDefault();
  const feedback = document.getElementById("action-feedback");
  const body = {
    uid_nfc: document.getElementById("cred-uid").value.trim().toUpperCase(),
    owner_name: document.getElementById("cred-owner").value.trim(),
    can_enter_room: document.getElementById("cred-room").checked,
    can_access_rack: document.getElementById("cred-rack").checked,
    is_active: document.getElementById("cred-active").checked,
  };

  try {
    const res = await fetch("/api/v1/credentials", {
      method: "POST",
      headers: getApiHeaders(),
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      const errBody = await res.json().catch(() => ({}));
      throw new Error(errBody.detail || `HTTP ${res.status}`);
    }
    document.getElementById("cred-uid").value = "";
    document.getElementById("cred-owner").value = "";
    feedback.textContent = `Credencial ${body.uid_nfc} salva com sucesso.`;
    await refreshAll();
  } catch (err) {
    feedback.textContent = `Erro ao salvar credencial: ${err.message}`;
  }
}

document.addEventListener("DOMContentLoaded", () => {
  document.getElementById("btn-refresh").addEventListener("click", refreshAll);
  document.getElementById("filter-severity").addEventListener("change", refreshAll);
  document.getElementById("filter-zone").addEventListener("change", refreshAll);
  document.getElementById("cred-form").addEventListener("submit", handleCredentialSubmit);

  for (const btn of document.querySelectorAll("[data-action]")) {
    btn.addEventListener("click", () => triggerSimulatedStep(btn.getAttribute("data-action")));
  }

  refreshAll();
  setInterval(() => {
    const autoToggle = document.getElementById("auto-refresh-toggle");
    if (autoToggle && autoToggle.checked) {
      refreshAll();
    }
  }, 2000);
});
