from flask import Flask, request, jsonify, render_template
from flask_cors import CORS
import sqlite3
from datetime import datetime

app = Flask(__name__)
CORS(app)

def init_db():
    with sqlite3.connect("plantpal.db") as conn:
        cursor = conn.cursor()
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                soil_raw INTEGER,
                soil_percent INTEGER,
                lux REAL,
                temperature REAL,
                humidity REAL,
                touch INTEGER,
                audio INTEGER,
                status TEXT,
                timestamp TEXT
            )
        """)
        conn.commit()

init_db()

@app.route("/api/sensor-data", methods=["POST"])
def receive_data():
    data = request.get_json()
    if not data:
        return jsonify({"status": "error", "message": "No JSON received"}), 400

    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with sqlite3.connect("plantpal.db") as conn:
        cursor = conn.cursor()
        cursor.execute("""
            INSERT INTO readings (soil_raw, soil_percent, lux, temperature, humidity, touch, audio, status, timestamp)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            data.get("soilRaw", 0),
            data.get("soilPercent", 0),
            data.get("lux", 0.0),
            data.get("temperature", 0.0),
            data.get("humidity", 0.0),
            data.get("touch", 0),
            data.get("audio", 0),
            data.get("status", "Unknown"),
            timestamp
        ))
        conn.commit()
    return jsonify({"status": "success"}), 201

@app.route("/api/latest", methods=["GET"])
def get_latest():
    with sqlite3.connect("plantpal.db") as conn:
        cursor = conn.cursor()
        cursor.execute("""
            SELECT soil_raw, soil_percent, lux, temperature, humidity, touch, audio, status, timestamp 
            FROM readings ORDER BY id DESC LIMIT 1
        """)
        row = cursor.fetchone()

    if row:
        return jsonify({
            "soilRaw": row[0],
            "soilPercent": row[1],
            "lux": row[2],
            "temperature": row[3],
            "humidity": row[4],
            "touch": bool(row[5]),
            "audio": bool(row[6]),
            "status": row[7],
            "timestamp": row[8]
        })
    return jsonify({"status": "empty"}), 200

@app.route("/")
def index():
    return render_template("index.html")

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)