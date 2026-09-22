from __future__ import annotations

import os
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from flask import Flask, jsonify, render_template, request
from flask_cors import CORS

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = Path(os.environ.get("PLANTPAL_DB", BASE_DIR / "plantpal.db"))
DEVICE_ID_DEFAULT = os.environ.get("PLANTPAL_DEVICE_ID", "plantpal-01")
MAX_HISTORY_ROWS = 5000

app = Flask(__name__)
CORS(app, resources={r"/api/*": {"origins": "*"}})

ALLOWED_COMMANDS = {
    "PLAY_AUDIO",
    "CHECK_PLANT",
    "TIME_GREETING",
    "OLED_ON",
    "OLED_OFF",
    "AUDIO_ON",
    "AUDIO_OFF",
    "SILENT_MODE",
    "DISPLAY_MODE",
    "WATERED",
    "CALIBRATE_DRY",
    "CALIBRATE_WET",
}


def now_utc_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def get_connection() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH, timeout=15)
    conn.execute("PRAGMA busy_timeout=15000")
    conn.execute("PRAGMA journal_mode=WAL")
    conn.row_factory = sqlite3.Row
    return conn


def table_columns(conn: sqlite3.Connection, table: str) -> set[str]:
    rows = conn.execute(f"PRAGMA table_info({table})").fetchall()
    return {row[1] for row in rows}


def init_db() -> None:
    with get_connection() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                soil_raw INTEGER,
                soil_percent INTEGER,
                lux REAL,
                temperature REAL,
                humidity REAL,
                touch INTEGER,
                audio INTEGER,
                status TEXT,
                timestamp TEXT,
                health_score INTEGER DEFAULT 0,
                health_label TEXT DEFAULT 'Unknown',
                soil_state TEXT DEFAULT 'Unknown',
                light_state TEXT DEFAULT 'Unknown',
                temperature_state TEXT DEFAULT 'Unknown',
                humidity_state TEXT DEFAULT 'Unknown',
                audio_enabled INTEGER DEFAULT 1,
                oled_enabled INTEGER DEFAULT 1,
                last_audio_track INTEGER DEFAULT 0,
                last_audio_message TEXT DEFAULT '',
                last_action TEXT DEFAULT '',
                last_action_message TEXT DEFAULT ''
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS commands (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                command TEXT NOT NULL,
                value TEXT DEFAULT '',
                status TEXT NOT NULL DEFAULT 'pending',
                created_at TEXT NOT NULL,
                executed_at TEXT,
                result TEXT DEFAULT ''
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                kind TEXT NOT NULL,
                track INTEGER DEFAULT 0,
                message TEXT DEFAULT '',
                icon TEXT DEFAULT 'leaf',
                created_at TEXT NOT NULL
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS device_state (
                device_id TEXT PRIMARY KEY,
                last_seen TEXT,
                ip_address TEXT DEFAULT '',
                audio_available INTEGER DEFAULT 0,
                audio_enabled INTEGER DEFAULT 1,
                oled_enabled INTEGER DEFAULT 1,
                display_mode TEXT DEFAULT 'AUTO',
                last_watered_at TEXT,
                last_audio_track INTEGER DEFAULT 0,
                last_audio_message TEXT DEFAULT '',
                last_action TEXT DEFAULT '',
                last_action_message TEXT DEFAULT '',
                last_action_at TEXT
            )
            """
        )

        # Migrate older PlantPal databases without destroying existing readings.
        required = {
            "device_id": "TEXT NOT NULL DEFAULT 'plantpal-01'",
            "health_score": "INTEGER DEFAULT 0",
            "health_label": "TEXT DEFAULT 'Unknown'",
            "soil_state": "TEXT DEFAULT 'Unknown'",
            "light_state": "TEXT DEFAULT 'Unknown'",
            "temperature_state": "TEXT DEFAULT 'Unknown'",
            "humidity_state": "TEXT DEFAULT 'Unknown'",
            "audio_enabled": "INTEGER DEFAULT 1",
            "oled_enabled": "INTEGER DEFAULT 1",
            "last_audio_track": "INTEGER DEFAULT 0",
            "last_audio_message": "TEXT DEFAULT ''",
            "last_action": "TEXT DEFAULT ''",
            "last_action_message": "TEXT DEFAULT ''",
        }
        existing = table_columns(conn, "readings")
        for name, sql_type in required.items():
            if name not in existing:
                conn.execute(f"ALTER TABLE readings ADD COLUMN {name} {sql_type}")

        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_readings_device_id ON readings(device_id, id DESC)"
        )
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_commands_pending ON commands(device_id, status, id)"
        )
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_events_device_id ON events(device_id, id DESC)"
        )
        conn.commit()


init_db()


def parse_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return default


def clean_number(value: Any, default: float = 0.0) -> float:
    try:
        number = float(value)
        if number != number:  # NaN
            return default
        return number
    except (TypeError, ValueError):
        return default


def clean_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def update_device_state(
    conn: sqlite3.Connection,
    device_id: str,
    *,
    ip_address: str = "",
    audio_available: bool | None = None,
    audio_enabled: bool | None = None,
    oled_enabled: bool | None = None,
    display_mode: str | None = None,
    last_audio_track: int | None = None,
    last_audio_message: str | None = None,
    last_action: str | None = None,
    last_action_message: str | None = None,
    last_action_at: str | None = None,
) -> None:
    current = conn.execute(
        "SELECT * FROM device_state WHERE device_id = ?", (device_id,)
    ).fetchone()
    base = {
        "last_seen": now_utc_iso(),
        "ip_address": ip_address if ip_address else (current["ip_address"] if current else ""),
        "audio_available": int(audio_available if audio_available is not None else (current["audio_available"] if current else 0)),
        "audio_enabled": int(audio_enabled if audio_enabled is not None else (current["audio_enabled"] if current else 1)),
        "oled_enabled": int(oled_enabled if oled_enabled is not None else (current["oled_enabled"] if current else 1)),
        "display_mode": display_mode if display_mode else (current["display_mode"] if current else "AUTO"),
        "last_watered_at": current["last_watered_at"] if current else None,
        "last_audio_track": int(last_audio_track if last_audio_track is not None else (current["last_audio_track"] if current else 0)),
        "last_audio_message": last_audio_message if last_audio_message is not None else (current["last_audio_message"] if current else ""),
        "last_action": last_action if last_action is not None else (current["last_action"] if current else ""),
        "last_action_message": last_action_message if last_action_message is not None else (current["last_action_message"] if current else ""),
        "last_action_at": last_action_at if last_action_at is not None else (current["last_action_at"] if current else None),
    }
    conn.execute(
        """
        INSERT INTO device_state (
            device_id, last_seen, ip_address, audio_available, audio_enabled,
            oled_enabled, display_mode, last_watered_at,
            last_audio_track, last_audio_message, last_action, last_action_message,
            last_action_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET
            last_seen=excluded.last_seen,
            ip_address=excluded.ip_address,
            audio_available=excluded.audio_available,
            audio_enabled=excluded.audio_enabled,
            oled_enabled=excluded.oled_enabled,
            display_mode=excluded.display_mode,
            last_watered_at=excluded.last_watered_at,
            last_audio_track=excluded.last_audio_track,
            last_audio_message=excluded.last_audio_message,
            last_action=excluded.last_action,
            last_action_message=excluded.last_action_message,
            last_action_at=excluded.last_action_at
        """,
        (
            device_id,
            base["last_seen"],
            base["ip_address"],
            base["audio_available"],
            base["audio_enabled"],
            base["oled_enabled"],
            base["display_mode"],
            base["last_watered_at"],
            base["last_audio_track"],
            base["last_audio_message"],
            base["last_action"],
            base["last_action_message"],
            base["last_action_at"],
        ),
    )


def device_online(last_seen: str | None) -> bool:
    if not last_seen:
        return False
    try:
        dt = datetime.fromisoformat(last_seen)
        age = (datetime.now(timezone.utc) - dt).total_seconds()
        return age <= 25
    except ValueError:
        return False


def row_to_latest(row: sqlite3.Row, state: sqlite3.Row | None, event: sqlite3.Row | None) -> dict[str, Any]:
    latest = {
        "soilRaw": row["soil_raw"],
        "soilPercent": row["soil_percent"],
        "lux": row["lux"],
        "temperature": row["temperature"],
        "humidity": row["humidity"],
        "touch": bool(row["touch"]),
        "audio": bool(row["audio"]),
        "status": row["status"] or "Unknown",
        "timestamp": row["timestamp"],
        "healthScore": row["health_score"],
        "healthLabel": row["health_label"],
        "soilState": row["soil_state"],
        "lightState": row["light_state"],
        "temperatureState": row["temperature_state"],
        "humidityState": row["humidity_state"],
        "audioEnabled": bool(row["audio_enabled"]),
        "oledEnabled": bool(row["oled_enabled"]),
        "lastAudioTrack": row["last_audio_track"],
        "lastAudioMessage": row["last_audio_message"],
        "lastAction": row["last_action"],
        "lastActionMessage": row["last_action_message"],
        "deviceId": row["device_id"],
    }

    latest["device"] = {
        "online": device_online(state["last_seen"] if state else None),
        "lastSeen": state["last_seen"] if state else None,
        "ipAddress": state["ip_address"] if state else "",
        "audioAvailable": bool(state["audio_available"]) if state else False,
        "audioEnabled": bool(state["audio_enabled"]) if state else bool(row["audio_enabled"]),
        "oledEnabled": bool(state["oled_enabled"]) if state else bool(row["oled_enabled"]),
        "displayMode": state["display_mode"] if state else "AUTO",
        "lastWateredAt": state["last_watered_at"] if state else None,
    }
    latest["lastEvent"] = (
        {
            "kind": event["kind"],
            "track": event["track"],
            "message": event["message"],
            "icon": event["icon"],
            "createdAt": event["created_at"],
        }
        if event
        else None
    )
    return latest


@app.get("/api/health")
def api_health():
    return jsonify({"status": "ok", "service": "PlantPal Cloud", "time": now_utc_iso()})


@app.post("/api/sensor-data")
def receive_data():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"status": "error", "message": "JSON object required"}), 400

    device_id = str(data.get("deviceId") or DEVICE_ID_DEFAULT)[:80]
    timestamp = str(data.get("timestamp") or now_utc_iso())[:64]

    values = (
        clean_int(data.get("soilRaw")),
        max(0, min(100, clean_int(data.get("soilPercent")))),
        clean_number(data.get("lux")),
        clean_number(data.get("temperature")),
        clean_number(data.get("humidity")),
        int(parse_bool(data.get("touch"))),
        int(parse_bool(data.get("audio"))),
        str(data.get("status") or "Unknown")[:160],
        timestamp,
        max(0, min(100, clean_int(data.get("healthScore")))),
        str(data.get("healthLabel") or "Unknown")[:40],
        str(data.get("soilState") or "Unknown")[:40],
        str(data.get("lightState") or "Unknown")[:40],
        str(data.get("temperatureState") or "Unknown")[:40],
        str(data.get("humidityState") or "Unknown")[:40],
        int(parse_bool(data.get("audioEnabled"), True)),
        int(parse_bool(data.get("oledEnabled"), True)),
        clean_int(data.get("lastAudioTrack")),
        str(data.get("lastAudioMessage") or "")[:240],
        str(data.get("lastAction") or "")[:80],
        str(data.get("lastActionMessage") or "")[:240],
    )

    try:
        init_db()
        with get_connection() as conn:
            conn.execute(
                """
                INSERT INTO readings (
                    device_id, soil_raw, soil_percent, lux, temperature, humidity,
                    touch, audio, status, timestamp, health_score, health_label,
                    soil_state, light_state, temperature_state, humidity_state,
                    audio_enabled, oled_enabled, last_audio_track,
                    last_audio_message, last_action, last_action_message
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (device_id, *values),
            )
            update_device_state(
                conn, device_id,
                audio_available=parse_bool(data.get("audio")),
                audio_enabled=parse_bool(data.get("audioEnabled"), True),
                oled_enabled=parse_bool(data.get("oledEnabled"), True),
                display_mode=str(data.get("displayMode") or "AUTO")[:20],
                last_audio_track=clean_int(data.get("lastAudioTrack")),
                last_audio_message=str(data.get("lastAudioMessage") or "")[:240],
                last_action=str(data.get("lastAction") or "")[:80],
                last_action_message=str(data.get("lastActionMessage") or "")[:240],
                last_action_at=timestamp if data.get("lastActionMessage") else None,
            )
            conn.execute(
                "DELETE FROM readings WHERE id <= (SELECT MAX(id) - ? FROM readings)",
                (MAX_HISTORY_ROWS,),
            )
            conn.commit()
    except sqlite3.Error:
        app.logger.exception("PlantPal telemetry database error")
        return jsonify({"status": "error", "message": "Database write failed"}), 500
    except Exception:
        app.logger.exception("PlantPal telemetry unexpected error")
        return jsonify({"status": "error", "message": "Telemetry processing failed"}), 500

    return jsonify({"status": "success", "deviceId": device_id}), 201


@app.get("/api/latest")
def get_latest():
    device_id = request.args.get("deviceId", DEVICE_ID_DEFAULT)
    with get_connection() as conn:
        row = conn.execute(
            "SELECT * FROM readings WHERE device_id = ? ORDER BY id DESC LIMIT 1",
            (device_id,),
        ).fetchone()
        state = conn.execute(
            "SELECT * FROM device_state WHERE device_id = ?",
            (device_id,),
        ).fetchone()
        event = conn.execute(
            "SELECT * FROM events WHERE device_id = ? ORDER BY id DESC LIMIT 1",
            (device_id,),
        ).fetchone()

    if not row:
        return jsonify({"status": "empty", "deviceId": device_id}), 200
    return jsonify(row_to_latest(row, state, event))


@app.get("/api/history")
def get_history():
    device_id = request.args.get("deviceId", DEVICE_ID_DEFAULT)
    try:
        limit = max(5, min(60, int(request.args.get("limit", 24))))
    except ValueError:
        limit = 24

    with get_connection() as conn:
        rows = conn.execute(
            """
            SELECT timestamp, soil_percent, lux, temperature, humidity, health_score
            FROM readings
            WHERE device_id = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()
    return jsonify(
        {
            "deviceId": device_id,
            "items": [
                {
                    "timestamp": row["timestamp"],
                    "soilPercent": row["soil_percent"],
                    "lux": row["lux"],
                    "temperature": row["temperature"],
                    "humidity": row["humidity"],
                    "healthScore": row["health_score"],
                }
                for row in reversed(rows)
            ],
        }
    )


@app.get("/api/commands/next")
def get_next_command():
    device_id = request.args.get("deviceId", DEVICE_ID_DEFAULT)
    with get_connection() as conn:
        command = conn.execute(
            """
            SELECT id, device_id, command, value, status, created_at
            FROM commands
            WHERE device_id = ? AND status = 'pending'
            ORDER BY id ASC
            LIMIT 1
            """,
            (device_id,),
        ).fetchone()

    if not command:
        return jsonify({"status": "empty"}), 200
    return jsonify({
        "status": "pending",
        "id": command["id"],
        "deviceId": command["device_id"],
        "command": command["command"],
        "value": command["value"],
        "createdAt": command["created_at"],
    })


@app.post("/api/commands")
def create_command():
    data = request.get_json(silent=True) or {}
    command = str(data.get("command") or "").strip().upper()
    value = str(data.get("value") if data.get("value") is not None else "")[:120]
    device_id = str(data.get("deviceId") or DEVICE_ID_DEFAULT)[:80]

    if command not in ALLOWED_COMMANDS:
        return jsonify({"status": "error", "message": "Unsupported command"}), 400

    if command == "PLAY_AUDIO":
        try:
            track = int(value)
        except ValueError:
            return jsonify({"status": "error", "message": "PLAY_AUDIO requires track 1-73"}), 400
        if not 1 <= track <= 73:
            return jsonify({"status": "error", "message": "Audio track must be 1-73"}), 400
        value = str(track)

    if command == "DISPLAY_MODE" and value.upper() not in {"AUTO", "HEALTH", "SENSORS", "STATUS", "SAVER"}:
        return jsonify({"status": "error", "message": "Invalid display mode"}), 400

    with get_connection() as conn:
        cursor = conn.execute(
            "INSERT INTO commands (device_id, command, value, status, created_at) VALUES (?, ?, ?, 'pending', ?)",
            (device_id, command, value, now_utc_iso()),
        )
        command_id = cursor.lastrowid
        conn.commit()

    return jsonify({"status": "queued", "id": command_id, "command": command, "value": value}), 201


@app.post("/api/commands/<int:command_id>/ack")
def acknowledge_command(command_id: int):
    data = request.get_json(silent=True) or {}
    success = parse_bool(data.get("success"), True)
    result = str(data.get("result") or "")[:240]
    device_id = str(data.get("deviceId") or DEVICE_ID_DEFAULT)[:80]

    with get_connection() as conn:
        cursor = conn.execute(
            """
            UPDATE commands
            SET status = ?, executed_at = ?, result = ?
            WHERE id = ? AND device_id = ?
            """,
            ("done" if success else "failed", now_utc_iso(), result, command_id, device_id),
        )
        conn.commit()
        if cursor.rowcount == 0:
            return jsonify({"status": "error", "message": "Command not found"}), 404

    return jsonify({"status": "acknowledged", "id": command_id, "success": success})


@app.post("/api/events")
def receive_event():
    data = request.get_json(silent=True) or {}
    device_id = str(data.get("deviceId") or DEVICE_ID_DEFAULT)[:80]
    kind = str(data.get("kind") or "event")[:40]
    track = max(0, min(73, clean_int(data.get("track"))))
    message = str(data.get("message") or "")[:240]
    icon = str(data.get("icon") or "leaf")[:20]
    created_at = str(data.get("createdAt") or now_utc_iso())[:64]

    with get_connection() as conn:
        conn.execute(
            "INSERT INTO events (device_id, kind, track, message, icon, created_at) VALUES (?, ?, ?, ?, ?, ?)",
            (device_id, kind, track, message, icon, created_at),
        )
        update_device_state(
            conn,
            device_id,
            last_audio_track=track if track else None,
            last_audio_message=message if track else None,
            last_action=kind,
            last_action_message=message,
            last_action_at=created_at,
        )
        if kind == "watered":
            conn.execute(
                "UPDATE device_state SET last_watered_at = ? WHERE device_id = ?",
                (created_at, device_id),
            )
        conn.commit()

    return jsonify({"status": "success"}), 201


@app.get("/api/activity")
def activity():
    device_id = request.args.get("deviceId", DEVICE_ID_DEFAULT)
    try:
        limit = max(5, min(30, int(request.args.get("limit", 12))))
    except ValueError:
        limit = 12

    with get_connection() as conn:
        rows = conn.execute(
            """
            SELECT kind, track, message, icon, created_at
            FROM events
            WHERE device_id = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()
        commands = conn.execute(
            """
            SELECT id, command, value, status, created_at, executed_at, result
            FROM commands
            WHERE device_id = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()

    return jsonify(
        {
            "events": [dict(row) for row in rows],
            "commands": [dict(row) for row in commands],
        }
    )


@app.post("/api/manual-water")
def manual_water():
    # This does not fake sensor data; it logs a human watering action.
    device_id = str((request.get_json(silent=True) or {}).get("deviceId") or DEVICE_ID_DEFAULT)[:80]
    created_at = now_utc_iso()
    with get_connection() as conn:
        conn.execute(
            "INSERT INTO events (device_id, kind, track, message, icon, created_at) VALUES (?, 'watered', 39, ?, 'water', ?)",
            (device_id, "Watering logged. Thanks for taking care of me.", created_at),
        )
        update_device_state(
            conn,
            device_id,
            last_audio_track=39,
            last_audio_message="Thanks for taking care of me.",
            last_action="watered",
            last_action_message="Watering logged.",
            last_action_at=created_at,
        )
        conn.execute(
            "UPDATE device_state SET last_watered_at = ? WHERE device_id = ?",
            (created_at, device_id),
        )
        conn.execute(
            "INSERT INTO commands (device_id, command, value, status, created_at) VALUES (?, 'PLAY_AUDIO', '39', 'pending', ?)",
            (device_id, created_at),
        )
        conn.commit()
    return jsonify({"status": "queued", "message": "Watering event logged"}), 201


@app.route("/")
def index():
    return render_template("index.html")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", "5000")), debug=False)
