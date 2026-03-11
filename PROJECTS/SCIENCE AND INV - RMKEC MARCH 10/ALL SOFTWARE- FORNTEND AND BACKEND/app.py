import pandas as pd
import numpy as np
from sklearn.ensemble import RandomForestRegressor, RandomForestClassifier, IsolationForest
from sklearn.model_selection import train_test_split
from sklearn.metrics import mean_absolute_error, accuracy_score
import joblib
import os

CSV_FILE = "air_quality_data.csv"
ROW_INTERVAL_SECONDS = 2

HORIZONS = {
    "5min": 5 * 60,
    "1hr": 60 * 60,
    "2hr": 2 * 60 * 60
}

HORIZON_ROWS = {k: v // ROW_INTERVAL_SECONDS for k, v in HORIZONS.items()}
ROLL_WINDOWS = [5, 15, 30]

def load_data(csv_file: str) -> pd.DataFrame:
    df = pd.read_csv(csv_file)
    df.columns = [c.strip() for c in df.columns]

    if {"dhtErr", "inaErr", "mqErr"}.issubset(df.columns):
        df = df[(df["dhtErr"] == 0) & (df["inaErr"] == 0) & (df["mqErr"] == 0)].copy()

    if df["airCtrl"].dtype == object:
        df["airCtrl"] = df["airCtrl"].astype(str).str.upper().map({"ON": 1, "OFF": 0}).fillna(0)

    df["occupancy"] = pd.to_numeric(df["occupancy"], errors="coerce").fillna(0).astype(int)

    numeric_cols = ["aqi", "co2", "temp", "humidity", "current_mA", "power_W", "airCtrl"]
    for col in numeric_cols:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    df = df.dropna().reset_index(drop=True)
    return df

def add_features(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()

    df["aqi_diff_1"] = df["aqi"].diff().fillna(0)
    df["co2_diff_1"] = df["co2"].diff().fillna(0)
    df["temp_diff_1"] = df["temp"].diff().fillna(0)
    df["humidity_diff_1"] = df["humidity"].diff().fillna(0)
    df["current_diff_1"] = df["current_mA"].diff().fillna(0)
    df["power_diff_1"] = df["power_W"].diff().fillna(0)

    for w in ROLL_WINDOWS:
        for col in ["aqi", "co2", "temp", "humidity", "current_mA", "power_W"]:
            df[f"{col}_mean_{w}"] = df[col].rolling(window=w, min_periods=1).mean()
            df[f"{col}_std_{w}"] = df[col].rolling(window=w, min_periods=1).std().fillna(0)

    return df

def add_targets(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()

    for label, shift_rows in HORIZON_ROWS.items():
        df[f"target_aqi_{label}"] = df["aqi"].shift(-shift_rows)
        df[f"target_co2_{label}"] = df["co2"].shift(-shift_rows)

    def recommend_action(row):
        if row["aqi"] > 200 or row["co2"] > 1400:
            return "RUN_5MIN"
        elif row["aqi"] > 120 or row["co2"] > 1000:
            return "RUN_2MIN"
        elif row["occupancy"] == 1 and (row["aqi"] > 90 or row["co2"] > 850):
            return "TURN_ON"
        else:
            return "KEEP_OFF"

    df["target_action"] = df.apply(recommend_action, axis=1)
    return df

def get_feature_columns(df: pd.DataFrame):
    ignore_cols = ["time_s", "mode", "nh3", "benzene", "co", "busVoltage", "energy_kWh", "cost", "dhtErr", "inaErr", "mqErr"]
    feature_cols = []
    for col in df.columns:
        if col.startswith("target_"):
            continue
        if col in ignore_cols:
            continue
        feature_cols.append(col)
    return feature_cols

def train_regressor(df: pd.DataFrame, target_col: str, model_name: str):
    df2 = df.dropna(subset=[target_col]).copy()
    if len(df2) < 10:
        print(f"Not enough data for {target_col}")
        return

    feature_cols = get_feature_columns(df2)
    X = df2[feature_cols]
    y = df2[target_col]

    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)

    model = RandomForestRegressor(
        n_estimators=100,
        max_depth=10,
        random_state=42,
        n_jobs=-1
    )
    model.fit(X_train, y_train)
    preds = model.predict(X_test)
    mae = mean_absolute_error(y_test, preds)

    joblib.dump({"model": model, "features": feature_cols}, model_name)
    print(f"[Saved] {model_name} | MAE: {mae:.3f}")

def train_classifier(df: pd.DataFrame, target_col: str, model_name: str):
    df2 = df.dropna(subset=[target_col]).copy()
    if len(df2) < 10:
        print(f"Not enough data for {target_col}")
        return

    feature_cols = get_feature_columns(df2)
    X = df2[feature_cols]
    y = df2[target_col]

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    model = RandomForestClassifier(
        n_estimators=100,
        max_depth=10,
        random_state=42,
        n_jobs=-1
    )
    model.fit(X_train, y_train)
    preds = model.predict(X_test)
    acc = accuracy_score(y_test, preds)

    joblib.dump({"model": model, "features": feature_cols}, model_name)
    print(f"[Saved] {model_name} | Accuracy: {acc:.3f}")

def train_anomaly_model(df: pd.DataFrame, model_name: str):
    feature_cols = ["aqi", "co2", "temp", "humidity", "occupancy", "current_mA", "power_W", "airCtrl"]
    X = df[feature_cols].copy()

    model = IsolationForest(contamination=0.05, random_state=42)
    model.fit(X)

    joblib.dump({"model": model, "features": feature_cols}, model_name)
    print(f"[Saved] {model_name}")

def predict_latest(df: pd.DataFrame):
    latest = df.tail(1).copy()
    outputs = {}

    model_files = {
        "aqi_5min": "model_aqi_5min.pkl",
        "aqi_1hr": "model_aqi_1hr.pkl",
        "aqi_2hr": "model_aqi_2hr.pkl",
        "co2_5min": "model_co2_5min.pkl",
        "co2_1hr": "model_co2_1hr.pkl",
        "co2_2hr": "model_co2_2hr.pkl",
        "action": "model_action.pkl",
        "anomaly": "model_anomaly.pkl"
    }

    for key, file in model_files.items():
        if not os.path.exists(file):
            continue

        obj = joblib.load(file)
        model = obj["model"]
        features = obj["features"]
        X = latest[features]

        if key == "anomaly":
            pred = model.predict(X)[0]
            outputs[key] = "ANOMALY" if pred == -1 else "NORMAL"
        else:
            outputs[key] = model.predict(X)[0]

    return outputs

def main():
    df = load_data(CSV_FILE)
    df = add_features(df)
    df = add_targets(df)

    print("Rows loaded:", len(df))

    train_regressor(df, "target_aqi_5min", "model_aqi_5min.pkl")
    train_regressor(df, "target_aqi_1hr", "model_aqi_1hr.pkl")
    train_regressor(df, "target_aqi_2hr", "model_aqi_2hr.pkl")

    train_regressor(df, "target_co2_5min", "model_co2_5min.pkl")
    train_regressor(df, "target_co2_1hr", "model_co2_1hr.pkl")
    train_regressor(df, "target_co2_2hr", "model_co2_2hr.pkl")

    train_classifier(df, "target_action", "model_action.pkl")
    train_anomaly_model(df, "model_anomaly.pkl")

    outputs = predict_latest(df)

    print("\n===== LATEST PREDICTIONS =====")
    for k, v in outputs.items():
        print(f"{k}: {v}")

if __name__ == "__main__":
    main()