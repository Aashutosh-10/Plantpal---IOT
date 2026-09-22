from flask import Flask, jsonify, request, render_template
from flask_cors import CORS
import sqlite3
from datetime import datetime, timezone
import os
from pathlib import Path

APP_VERSION = "PlantPal Cloud 2.0"
DEVICE_ONLINE_WINDOW = 20
DATABASE_PATH = Path(os.environ.get("PLANTPAL_DB", "plantpal.db"))

app = Flask(__name__)
CORS(app)


def now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def connect_db():
    conn = sqlite3.connect(DATABASE_PATH, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=5000")
    return conn


def init_db():
    with connect_db() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL DEFAULT 'plantpal-01',
                firmware TEXT,
                plant TEXT,
                species TEXT,
                soil_raw INTEGER NOT NULL DEFAULT 0,
                soil_percent INTEGER NOT NULL DEFAULT 0,
                lux REAL NOT NULL DEFAULT 0,
                temperature REAL NOT NULL DEFAULT 0,
                humidity REAL NOT NULL DEFAULT 0,
                touch INTEGER NOT NULL DEFAULT 0,
                audio INTEGER NOT NULL DEFAULT 0,
                audio_enabled INTEGER NOT NULL DEFAULT 0,
                oled_enabled INTEGER NOT NULL DEFAULT 0,
                display_mode TEXT NOT NULL DEFAULT 'AUTO',
                volume INTEGER NOT NULL DEFAULT 18,
                health_score INTEGER NOT NULL DEFAULT 0,
                health_label TEXT NOT NULL DEFAULT 'Unknown',
                soil_state TEXT NOT NULL DEFAULT 'Unknown',
                light_state TEXT NOT NULL DEFAULT 'Unknown',
                temperature_state TEXT NOT NULL DEFAULT 'Unknown',
                humidity_state TEXT NOT NULL DEFAULT 'Unknown',
                last_audio_track INTEGER NOT NULL DEFAULT 0,
                last_audio_message TEXT NOT NULL DEFAULT '',
                last_action TEXT NOT NULL DEFAULT '',
                last_action_message TEXT NOT NULL DEFAULT '',
                device_timestamp TEXT,
                server_timestamp TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_readings_device_id_id
                ON readings(device_id, id DESC);

            CREATE TABLE IF NOT EXISTS commands (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL DEFAULT 'plantpal-01',
                command TEXT NOT NULL,
                value TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL DEFAULT 'pending',
                result TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL,
                executed_at TEXT
            );

            CREATE INDEX IF NOT EXISTS idx_commands_pending
                ON commands(device_id, status, id);

            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL DEFAULT 'plantpal-01',
                kind TEXT NOT NULL,
                track INTEGER NOT NULL DEFAULT 0,
                message TEXT NOT NULL DEFAULT '',
                icon TEXT NOT NULL DEFAULT 'leaf',
                created_at TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_events_device_id_id
                ON events(device_id, id DESC);

            CREATE TABLE IF NOT EXISTS device_state (
                device_id TEXT PRIMARY KEY,
                online INTEGER NOT NULL DEFAULT 0,
                last_seen TEXT,
                last_ip TEXT,
                rssi INTEGER,
                firmware TEXT,
                plant TEXT,
                species TEXT,
                audio_enabled INTEGER NOT NULL DEFAULT 0,
                oled_enabled INTEGER NOT NULL DEFAULT 0,
                display_mode TEXT NOT NULL DEFAULT 'AUTO',
                volume INTEGER NOT NULL DEFAULT 18,
                last_command_id INTEGER NOT NULL DEFAULT 0,
                last_action TEXT NOT NULL DEFAULT '',
                last_action_message TEXT NOT NULL DEFAULT ''
            );
            """
        )

        # Gentle migration for older PlantPal installations.
        existing = {
            row[1]
            for row in conn.execute("PRAGMA table_info(readings)").fetchall()
        }
        additions = {
            "device_id": "TEXT NOT NULL DEFAULT 'plantpal-01'",
            "firmware": "TEXT",
            "plant": "TEXT",
            "species": "TEXT",
            "audio_enabled": "INTEGER NOT NULL DEFAULT 0",
            "oled_enabled": "INTEGER NOT NULL DEFAULT 0",
            "display_mode": "TEXT NOT NULL DEFAULT 'AUTO'",
            "volume": "INTEGER NOT NULL DEFAULT 18",
            "health_score": "INTEGER NOT NULL DEFAULT 0",
            "health_label": "TEXT NOT NULL DEFAULT 'Unknown'",
            "soil_state": "TEXT NOT NULL DEFAULT 'Unknown'",
            "light_state": "TEXT NOT NULL DEFAULT 'Unknown'",
            "temperature_state": "TEXT NOT NULL DEFAULT 'Unknown'",
            "humidity_state": "TEXT NOT NULL DEFAULT 'Unknown'",
            "last_audio_track": "INTEGER NOT NULL DEFAULT 0",
            "last_audio_message": "TEXT NOT NULL DEFAULT ''",
            "last_action": "TEXT NOT NULL DEFAULT ''",
            "last_action_message": "TEXT NOT NULL DEFAULT ''",
            "device_timestamp": "TEXT",
            "server_timestamp": "TEXT NOT NULL DEFAULT ''",
        }
        for name, definition in additions.items():
            if name not in existing:
                conn.execute(f"ALTER TABLE readings ADD COLUMN {name} {definition}")
        conn.commit()


def row_to_reading(row):
    if row is None:
        return None
    return {
        "id": row["id"],
        "deviceId": row["device_id"],
        "firmware": row["firmware"],
        "plant": row["plant"],
        "species": row["species"],
        "soilRaw": row["soil_raw"],
        "soilPercent": row["soil_percent"],
        "lux": row["lux"],
        "temperature": row["temperature"],
        "humidity": row["humidity"],
        "touch": bool(row["touch"]),
        "audio": bool(row["audio"]),
        "audioEnabled": bool(row["audio_enabled"]),
        "oledEnabled": bool(row["oled_enabled"]),
        "displayMode": row["display_mode"],
        "volume": row["volume"],
        "healthScore": row["health_score"],
        "healthLabel": row["health_label"],
        "soilState": row["soil_state"],
        "lightState": row["light_state"],
        "temperatureState": row["temperature_state"],
        "humidityState": row["humidity_state"],
        "lastAudioTrack": row["last_audio_track"],
        "lastAudioMessage": row["last_audio_message"],
        "lastAction": row["last_action"],
        "lastActionMessage": row["last_action_message"],
        "timestamp": row["device_timestamp"] or row["server_timestamp"],
        "serverTimestamp": row["server_timestamp"],
    }


def upsert_device_state(conn, data):
    device_id = str(data.get("deviceId") or "plantpal-01")[:80]
    conn.execute(
        """
        INSERT INTO device_state (
            device_id, online, last_seen, last_ip, rssi, firmware, plant, species,
            audio_enabled, oled_enabled, display_mode, volume, last_action,
            last_action_message
        ) VALUES (?,1,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(device_id) DO UPDATE SET
            online=1,
            last_seen=excluded.last_seen,
            last_ip=excluded.last_ip,
            rssi=excluded.rssi,
            firmware=excluded.firmware,
            plant=excluded.plant,
            species=excluded.species,
            audio_enabled=excluded.audio_enabled,
            oled_enabled=excluded.oled_enabled,
            display_mode=excluded.display_mode,
            volume=excluded.volume,
            last_action=excluded.last_action,
            last_action_message=excluded.last_action_message
        """,
        (
            device_id,
            data.get("serverTimestamp"),
            request.remote_addr,
            data.get("wifiRssi"),
            data.get("firmware"),
            data.get("plant"),
            data.get("species"),
            int(bool(data.get("audioEnabled"))),
            int(bool(data.get("oledEnabled"))),
            str(data.get("displayMode") or "AUTO")[:20],
            int(data.get("volume") or 0),
            str(data.get("lastAction") or "")[:80],
            str(data.get("lastActionMessage") or "")[:240],
        ),
    )


@app.get("/health")
def health():
    return jsonify({"status": "ok", "service": APP_VERSION, "time": now_iso()})


@app.post("/api/sensor-data")
def receive_sensor_data():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"status": "error", "message": "No valid JSON received"}), 400

    server_timestamp = now_iso()
    device_id = str(data.get("deviceId") or "plantpal-01")[:80]

    numeric_defaults = {
        "soilRaw": 0,
        "soilPercent": 0,
        "lux": 0.0,
        "temperature": 0.0,
        "humidity": 0.0,
        "touch": False,
        "audio": False,
        "audioEnabled": False,
        "oledEnabled": False,
        "volume": 0,
        "healthScore": 0,
        "lastAudioTrack": 0,
    }

    with connect_db() as conn:
        conn.execute(
            """
            INSERT INTO readings (
                device_id, firmware, plant, species,
                soil_raw, soil_percent, lux, temperature, humidity,
                touch, audio, audio_enabled, oled_enabled, display_mode,
                volume, health_score, health_label,
                soil_state, light_state, temperature_state, humidity_state,
                last_audio_track, last_audio_message,
                last_action, last_action_message,
                device_timestamp, server_timestamp
            ) VALUES (
                ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?
            )
            """,
            (
                device_id,
                str(data.get("firmware") or "")[:100],
                str(data.get("plant") or "")[:100],
                str(data.get("species") or "")[:150],
                int(data.get("soilRaw", numeric_defaults["soilRaw"])),
                max(0, min(100, int(data.get("soilPercent", numeric_defaults["soilPercent"])))),
                float(data.get("lux", numeric_defaults["lux"])),
                float(data.get("temperature", numeric_defaults["temperature"])),
                float(data.get("humidity", numeric_defaults["humidity"])),
                int(bool(data.get("touch", False))),
                int(bool(data.get("audio", False))),
                int(bool(data.get("audioEnabled", False))),
                int(bool(data.get("oledEnabled", False))),
                str(data.get("displayMode") or "AUTO")[:20],
                max(0, min(30, int(data.get("volume", numeric_defaults["volume"])))),
                max(0, min(100, int(data.get("healthScore", numeric_defaults["healthScore"])))),
                str(data.get("healthLabel") or "Unknown")[:80],
                str(data.get("soilState") or "Unknown")[:80],
                str(data.get("lightState") or "Unknown")[:80],
                str(data.get("temperatureState") or "Unknown")[:80],
                str(data.get("humidityState") or "Unknown")[:80],
                int(data.get("lastAudioTrack", 0)),
                str(data.get("lastAudioMessage") or "")[:300],
                str(data.get("lastAction") or "")[:80],
                str(data.get("lastActionMessage") or "")[:300],
                str(data.get("timestamp") or "")[:60],
                server_timestamp,
            ),
        )
        data_for_state = dict(data)
        data_for_state["serverTimestamp"] = server_timestamp
        upsert_device_state(conn, data_for_state)
        conn.commit()

    return jsonify({"status": "success", "storedAt": server_timestamp}), 201


@app.get("/api/latest")
def latest():
    device_id = request.args.get("deviceId", "plantpal-01")
    with connect_db() as conn:
        row = conn.execute(
            "SELECT * FROM readings WHERE device_id=? ORDER BY id DESC LIMIT 1",
            (device_id,),
        ).fetchone()
        state = conn.execute(
            "SELECT * FROM device_state WHERE device_id=?",
            (device_id,),
        ).fetchone()
        event = conn.execute(
            "SELECT * FROM events WHERE device_id=? ORDER BY id DESC LIMIT 1",
            (device_id,),
        ).fetchone()

    if row is None:
        return jsonify({"status": "empty", "device": {"online": False}}), 200

    reading = row_to_reading(row)
    device = {
        "online": False,
        "lastSeen": None,
        "rssi": None,
        "firmware": None,
        "audioEnabled": reading["audioEnabled"],
        "oledEnabled": reading["oledEnabled"],
        "displayMode": reading["displayMode"],
    }
    if state:
        device["lastSeen"] = state["last_seen"]
        device["rssi"] = state["rssi"]
        device["firmware"] = state["firmware"]
        device["online"] = True  # refined below
        try:
            seen = datetime.fromisoformat(state["last_seen"] or "")
            if seen.tzinfo is None:
                seen = seen.replace(tzinfo=timezone.utc)
            device["online"] = (datetime.now(timezone.utc) - seen).total_seconds() <= DEVICE_ONLINE_WINDOW
        except ValueError:
            device["online"] = False

    result = {"status": reading["status"], "reading": reading, "device": device}
    if event:
        result["event"] = {
            "id": event["id"],
            "kind": event["kind"],
            "track": event["track"],
            "message": event["message"],
            "icon": event["icon"],
            "createdAt": event["created_at"],
        }
    return jsonify(result)


@app.get("/api/history")
def history():
    device_id = request.args.get("deviceId", "plantpal-01")
    try:
        limit = max(1, min(60, int(request.args.get("limit", 24))))
    except ValueError:
        limit = 24

    with connect_db() as conn:
        rows = conn.execute(
            """
            SELECT soil_percent, lux, temperature, humidity, device_timestamp, server_timestamp
            FROM readings
            WHERE device_id=?
            ORDER BY id DESC LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()

    rows = list(reversed(rows))
    return jsonify(
        {
            "deviceId": device_id,
            "points": [
                {
                    "soilPercent": row["soil_percent"],
                    "lux": row["lux"],
                    "temperature": row["temperature"],
                    "humidity": row["humidity"],
                    "timestamp": row["device_timestamp"] or row["server_timestamp"],
                }
                for row in rows
            ],
        }
    )


@app.post("/api/commands")
def create_command():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"status": "error", "message": "JSON body required"}), 400

    device_id = str(data.get("deviceId") or "plantpal-01")[:80]
    command = str(data.get("command") or "").strip().upper()[:40]
    value = str(data.get("value") if data.get("value") is not None else "")[:160]

    allowed = {
        "PING",
        "PLAY_AUDIO",
        "STOP_AUDIO",
        "CHECK_PLANT",
        "TIME_GREETING",
        "OLED_ON",
        "OLED_OFF",
        "AUDIO_ON",
        "AUDIO_OFF",
        "SILENT_MODE",
        "DISPLAY_MODE",
        "VOLUME",
        "WATERED",
        "CALIBRATE_DRY",
        "CALIBRATE_WET",
        "RESET_CALIBRATION",
        "SHOW_MESSAGE",
        "SCREEN_SAVER",
        "WAKE_SCREEN",
    }

    if command not in allowed:
        return jsonify({"status": "error", "message": "Unsupported command"}), 400

    if command == "PLAY_AUDIO":
        try:
            track = int(value)
        except ValueError:
            return jsonify({"status": "error", "message": "Track must be 1-73"}), 400
        if not 1 <= track <= 73:
            return jsonify({"status": "error", "message": "Track must be 1-73"}), 400

    if command == "DISPLAY_MODE" and value.upper() not in {"AUTO", "SENSORS", "HEALTH", "STATUS", "SAVER"}:
        return jsonify({"status": "error", "message": "Invalid display mode"}), 400

    if command == "VOLUME":
        try:
            volume = int(value)
        except ValueError:
            return jsonify({"status": "error", "message": "Volume must be 0-30"}), 400
        if not 0 <= volume <= 30:
            return jsonify({"status": "error", "message": "Volume must be 0-30"}), 400

    created_at = now_iso()
    with connect_db() as conn:
        cur = conn.execute(
            "INSERT INTO commands(device_id,command,value,status,created_at) VALUES(?,?,?,?,?)",
            (device_id, command, value, "pending", created_at),
        )
        conn.commit()
        command_id = cur.lastrowid

    return jsonify(
        {
            "status": "queued",
            "command": {
                "id": command_id,
                "deviceId": device_id,
                "command": command,
                "value": value,
                "status": "pending",
                "createdAt": created_at,
            },
        }
    ), 201


@app.get("/api/commands/next")
def next_command():
    device_id = request.args.get("deviceId", "plantpal-01")
    with connect_db() as conn:
        row = conn.execute(
            "SELECT id,device_id,command,value,status,created_at FROM commands "
            "WHERE device_id=? AND status='pending' ORDER BY id ASC LIMIT 1",
            (device_id,),
        ).fetchone()

    if not row:
        return jsonify({"status": "empty"}), 200

    return jsonify(
        {
            "status": "pending",
            "id": row["id"],
            "deviceId": row["device_id"],
            "command": row["command"],
            "value": row["value"],
            "createdAt": row["created_at"],
        }
    )


@app.post("/api/commands/<int:command_id>/ack")
def acknowledge_command(command_id):
    data = request.get_json(silent=True) or {}
    success = bool(data.get("success"))
    result = str(data.get("result") or "")[:500]
    device_id = str(data.get("deviceId") or "plantpal-01")[:80]
    executed_at = now_iso()

    with connect_db() as conn:
        row = conn.execute(
            "SELECT device_id FROM commands WHERE id=?",
            (command_id,),
        ).fetchone()
        if row is None:
            return jsonify({"status": "error", "message": "Command not found"}), 404
        if row["device_id"] != device_id:
            return jsonify({"status": "error", "message": "Device mismatch"}), 403

        conn.execute(
            "UPDATE commands SET status=?,result=?,executed_at=? WHERE id=?",
            ("done" if success else "error", result, executed_at, command_id),
        )
        conn.execute(
            "UPDATE device_state SET last_command_id=?,last_action_message=? WHERE device_id=?",
            (command_id, result, device_id),
        )
        conn.commit()

    return jsonify({"status": "acknowledged", "success": success})


@app.get("/api/commands/history")
def command_history():
    device_id = request.args.get("deviceId", "plantpal-01")
    try:
        limit = max(1, min(30, int(request.args.get("limit", 20))))
    except ValueError:
        limit = 20

    with connect_db() as conn:
        rows = conn.execute(
            "SELECT id,command,value,status,result,created_at,executed_at "
            "FROM commands WHERE device_id=? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        ).fetchall()

    return jsonify(
        {
            "deviceId": device_id,
            "commands": [
                {
                    "id": row["id"],
                    "command": row["command"],
                    "value": row["value"],
                    "status": row["status"],
                    "result": row["result"],
                    "createdAt": row["created_at"],
                    "executedAt": row["executed_at"],
                }
                for row in rows
            ],
        }
    )


@app.post("/api/events")
def receive_event():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"status": "error", "message": "JSON body required"}), 400

    device_id = str(data.get("deviceId") or "plantpal-01")[:80]
    kind = str(data.get("kind") or "event")[:50]
    message = str(data.get("message") or "")[:500]
    icon = str(data.get("icon") or "leaf")[:30]
    track = int(data.get("track") or 0)
    created_at = str(data.get("createdAt") or now_iso())[:60]

    with connect_db() as conn:
        conn.execute(
            "INSERT INTO events(device_id,kind,track,message,icon,created_at) VALUES(?,?,?,?,?,?)",
            (device_id, kind, track, message, icon, created_at),
        )
        conn.execute(
            "UPDATE device_state SET last_action=?,last_action_message=?,last_seen=?,online=1 WHERE device_id=?",
            (kind, message, now_iso(), device_id),
        )
        conn.commit()

    return jsonify({"status": "success"}), 201


@app.get("/api/events")
def get_events():
    device_id = request.args.get("deviceId", "plantpal-01")
    try:
        limit = max(1, min(50, int(request.args.get("limit", 20))))
    except ValueError:
        limit = 20

    with connect_db() as conn:
        rows = conn.execute(
            "SELECT id,kind,track,message,icon,created_at FROM events "
            "WHERE device_id=? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        ).fetchall()

    return jsonify(
        {
            "events": [
                {
                    "id": row["id"],
                    "kind": row["kind"],
                    "track": row["track"],
                    "message": row["message"],
                    "icon": row["icon"],
                    "createdAt": row["created_at"],
                }
                for row in rows
            ]
        }
    )


@app.get("/")
def index():
    return render_template("index.html")


init_db()


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", "5000")), debug=False)
