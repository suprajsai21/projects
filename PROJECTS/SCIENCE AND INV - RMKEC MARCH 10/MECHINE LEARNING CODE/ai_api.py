import os
import pandas as pd
import numpy as np
import joblib
from flask import Flask, jsonify, request
from flask_cors import CORS
from datetime import datetime

app = Flask(__name__)
CORS(app)

BASE = os.path.dirname(os.path.abspath(__file__))
CSV_FILE = os.path.join(BASE, "air_quality_data.csv")

# ─── LIVE DATA STORAGE (filled by ESP32 POST) ──────────────────
latest_esp32 = {}

def load_model(name):
    return joblib.load(os.path.join(BASE, name))

def prepare(df):
    df = df.copy()
    df.columns = [c.strip() for c in df.columns]

    if df["airCtrl"].dtype == object:
        df["airCtrl"] = (
            df["airCtrl"].astype(str).str.upper().str.strip()
            .map({"ON": 1, "OFF": 0, "1": 1, "0": 0})
            .fillna(0)
        )

    numeric = ["aqi", "co2", "temp", "humidity", "occupancy", "current_mA", "power_W", "airCtrl"]
    for c in numeric:
        df[c] = pd.to_numeric(df[c], errors="coerce")

    df = df.dropna().reset_index(drop=True)

    df["aqi_diff_1"]      = df["aqi"].diff().fillna(0)
    df["co2_diff_1"]      = df["co2"].diff().fillna(0)
    df["temp_diff_1"]     = df["temp"].diff().fillna(0)
    df["humidity_diff_1"] = df["humidity"].diff().fillna(0)
    df["current_diff_1"]  = df["current_mA"].diff().fillna(0)
    df["power_diff_1"]    = df["power_W"].diff().fillna(0)

    for w in [5, 10, 15]:
        for c in ["aqi", "co2", "temp", "humidity", "current_mA", "power_W"]:
            df[f"{c}_mean_{w}"] = df[c].rolling(w, 1).mean()
            df[f"{c}_std_{w}"]  = df[c].rolling(w, 1).std().fillna(0)

    return df

# ─── ROUTE 1: ESP32 POSTS DATA HERE ────────────────────────────
@app.route("/data", methods=["POST"])
def receive_data():
    global latest_esp32
    data = request.get_json(force=True, silent=True)
    if not data:
        return jsonify({"error": "No JSON received"}), 400

    latest_esp32 = data
    latest_esp32["timestamp"] = datetime.now().isoformat()

    # ── Also append to CSV so AI models keep getting new data ──
    try:
        row = {
            "time_s":     data.get("time_s", 0),
            "aqi":        data.get("aqi", 0),
            "co2":        data.get("co2", 400),
            "temp":       data.get("temp", 25),
            "humidity":   data.get("humidity", 50),
            "nh3":        data.get("nh3", 0),
            "benzene":    data.get("benzene", 0),
            "co":         data.get("co", 0),
            "busVoltage": data.get("voltage", 0),
            "current_mA": data.get("current", 0),
            "power_W":    data.get("power", 0),
            "energy_kWh": data.get("energy", 0),
            "cost":       data.get("cost", 0),
            "occupancy":  data.get("occupancy", 0),
            "airCtrl":    data.get("airCtrl", 0),
            "mode":       data.get("mode", "AUTO"),
            "dhtErr":     int(data.get("dhtErr", False)),
            "inaErr":     int(data.get("inaErr", False)),
            "mqErr":      int(data.get("mqErr", False)),
        }
        new_row = pd.DataFrame([row])
        write_header = not os.path.exists(CSV_FILE)
        new_row.to_csv(CSV_FILE, mode="a", header=write_header, index=False)
    except Exception as e:
        print(f"CSV write error: {e}")

    print(f"[ESP32] AQI={data.get('aqi','?')}  CO2={data.get('co2','?')}  Temp={data.get('temp','?')}")
    return jsonify({"status": "ok"})


# ─── ROUTE 2: WEBSITE FETCHES LIVE DATA ────────────────────────
@app.route("/live", methods=["GET"])
def get_live():
    if not latest_esp32:
        return jsonify({"error": "No data received from ESP32 yet"}), 204
    return jsonify(latest_esp32)


# ─── ROUTE 3: AI PREDICTIONS (unchanged logic, same port) ──────
@app.route("/predict", methods=["GET"])
def predict():
    try:
        if not os.path.exists(CSV_FILE):
            return jsonify({"error": f"CSV not found: {CSV_FILE}"}), 404

        df = pd.read_csv(CSV_FILE)
        df = prepare(df)

        if len(df) == 0:
            return jsonify({"error": "No valid rows in CSV"}), 400

        latest = df.tail(1).copy()

        models = {
            "aqi_10sec": "model_aqi_10sec.pkl",
            "aqi_30sec": "model_aqi_30sec.pkl",
            "aqi_60sec": "model_aqi_60sec.pkl",
            "co2_10sec": "model_co2_10sec.pkl",
            "co2_30sec": "model_co2_30sec.pkl",
            "co2_60sec": "model_co2_60sec.pkl",
            "action":    "model_action.pkl",
            "anomaly":   "model_anomaly.pkl",
        }

        output = {}

        for key, file in models.items():
            path = os.path.join(BASE, file)
            if not os.path.exists(path):
                continue

            obj      = load_model(file)
            model    = obj["model"]
            features = obj["features"]

            missing = [f for f in features if f not in latest.columns]
            if missing:
                return jsonify({"error": f"Missing features for {file}", "missing_features": missing}), 500

            X = latest[features]

            if key == "anomaly":
                r = model.predict(X)[0]
                output[key] = "ANOMALY" if r == -1 else "NORMAL"
            else:
                r = model.predict(X)[0]
                output[key] = round(float(r), 2) if isinstance(r, (np.floating, float, int)) else str(r)

        output["aqi_now"]      = round(float(latest["aqi"].values[0]), 2)
        output["co2_now"]      = round(float(latest["co2"].values[0]), 2)
        output["temp_now"]     = round(float(latest["temp"].values[0]), 2)
        output["humidity_now"] = round(float(latest["humidity"].values[0]), 2)

        return jsonify(output)

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# ─── START ON PORT 5000 (unchanged) ────────────────────────────
if __name__ == "__main__":
    print("=" * 45)
    print("  AirMind AI API Server")
    print("=" * 45)
    print(f"  ESP32 POST  → http://0.0.0.0:5000/data")
    print(f"  Live GET    → http://0.0.0.0:5000/live")
    print(f"  AI GET      → http://0.0.0.0:5000/predict")
    print("=" * 45)
    app.run(host="0.0.0.0", port=5000, debug=True)