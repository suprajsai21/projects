import os
import pandas as pd
import numpy as np
from sklearn.ensemble import RandomForestRegressor, RandomForestClassifier, IsolationForest
from sklearn.model_selection import train_test_split
from sklearn.metrics import mean_absolute_error, accuracy_score
import joblib

# =========================================================
# FILE SETTINGS
# =========================================================
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

POSSIBLE_CSV_FILES = [
    "air_quality_data.csv",
    "air_quality_log.csv"
]

ROW_INTERVAL_SECONDS = 2

# DEMO HORIZONS FOR SMALL DATASET
HORIZONS = {
    "10sec": 10,
    "30sec": 30,
    "60sec": 60
}

HORIZON_ROWS = {k: v // ROW_INTERVAL_SECONDS for k, v in HORIZONS.items()}
ROLL_WINDOWS = [5, 10, 15]

REQUIRED_COLUMNS = [
    "aqi",
    "co2",
    "temp",
    "humidity",
    "occupancy",
    "current_mA",
    "power_W",
    "airCtrl"
]

# =========================================================
# HELPERS
# =========================================================
def find_csv_file() -> str:
    for file_name in POSSIBLE_CSV_FILES:
        full_path = os.path.join(SCRIPT_DIR, file_name)
        if os.path.exists(full_path):
            return full_path
    raise FileNotFoundError(
        "CSV file not found. Put one of these in same folder:\n"
        "- air_quality_data.csv\n"
        "- air_quality_log.csv"
    )

def model_path(name: str) -> str:
    return os.path.join(SCRIPT_DIR, name)

# =========================================================
# LOAD DATA
# =========================================================
def load_data(csv_file: str) -> pd.DataFrame:
    df = pd.read_csv(csv_file)
    df.columns = [c.strip() for c in df.columns]

    print("CSV loaded from:", csv_file)
    print("Columns found:", list(df.columns))

    missing = [col for col in REQUIRED_COLUMNS if col not in df.columns]
    if missing:
        raise ValueError(f"Missing required columns in CSV: {missing}")

    # Keep only healthy rows if error columns exist
    if {"dhtErr", "inaErr", "mqErr"}.issubset(df.columns):
        df = df[
            (pd.to_numeric(df["dhtErr"], errors="coerce").fillna(1) == 0) &
            (pd.to_numeric(df["inaErr"], errors="coerce").fillna(1) == 0) &
            (pd.to_numeric(df["mqErr"], errors="coerce").fillna(1) == 0)
        ].copy()

    # Convert airCtrl
    if df["airCtrl"].dtype == object:
        df["airCtrl"] = (
            df["airCtrl"]
            .astype(str)
            .str.upper()
            .str.strip()
            .map({"ON": 1, "OFF": 0, "1": 1, "0": 0})
            .fillna(0)
        )

    # Numeric conversion
    numeric_cols = ["aqi", "co2", "temp", "humidity", "occupancy", "current_mA", "power_W", "airCtrl"]
    for col in numeric_cols:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    optional_numeric_cols = ["time_s", "nh3", "benzene", "co", "busVoltage", "energy_kWh", "cost"]
    for col in optional_numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    df = df.dropna(subset=REQUIRED_COLUMNS).reset_index(drop=True)

    print("Rows after cleaning:", len(df))
    return df

# =========================================================
# FEATURE ENGINEERING
# =========================================================
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
    ignore_cols = [
        "time_s", "mode", "nh3", "benzene", "co",
        "busVoltage", "energy_kWh", "cost",
        "dhtErr", "inaErr", "mqErr"
    ]

    feature_cols = []
    for col in df.columns:
        if col.startswith("target_"):
            continue
        if col in ignore_cols:
            continue
        feature_cols.append(col)

    return feature_cols

# =========================================================
# TRAINING
# =========================================================
def train_regressor(df: pd.DataFrame, target_col: str, model_name: str):
    df2 = df.dropna(subset=[target_col]).copy()

    if len(df2) < 20:
        print(f"[Skip] Not enough data for {target_col} (need more rows)")
        return False

    feature_cols = get_feature_columns(df2)
    X = df2[feature_cols]
    y = df2[target_col]

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42
    )

    model = RandomForestRegressor(
        n_estimators=100,
        max_depth=10,
        random_state=42,
        n_jobs=-1
    )

    model.fit(X_train, y_train)
    preds = model.predict(X_test)
    mae = mean_absolute_error(y_test, preds)

    joblib.dump(
        {"model": model, "features": feature_cols},
        model_path(model_name)
    )

    print(f"[Saved] {model_name} | MAE: {mae:.3f}")
    return True

def train_classifier(df: pd.DataFrame, target_col: str, model_name: str):
    df2 = df.dropna(subset=[target_col]).copy()

    if len(df2) < 20:
        print(f"[Skip] Not enough data for {target_col}")
        return False

    class_counts = df2[target_col].value_counts()
    if len(class_counts) < 2:
        print(f"[Skip] Only one class found for {target_col}: {list(class_counts.index)}")
        return False

    feature_cols = get_feature_columns(df2)
    X = df2[feature_cols]
    y = df2[target_col]

    if class_counts.min() >= 2:
        X_train, X_test, y_train, y_test = train_test_split(
            X, y, test_size=0.2, random_state=42, stratify=y
        )
    else:
        X_train, X_test, y_train, y_test = train_test_split(
            X, y, test_size=0.2, random_state=42
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

    joblib.dump(
        {"model": model, "features": feature_cols},
        model_path(model_name)
    )

    print(f"[Saved] {model_name} | Accuracy: {acc:.3f}")
    return True

def train_anomaly_model(df: pd.DataFrame, model_name: str):
    if len(df) < 20:
        print("[Skip] Not enough data for anomaly model")
        return False

    feature_cols = ["aqi", "co2", "temp", "humidity", "occupancy", "current_mA", "power_W", "airCtrl"]
    X = df[feature_cols].copy()

    model = IsolationForest(
        contamination=0.05,
        random_state=42
    )
    model.fit(X)

    joblib.dump(
        {"model": model, "features": feature_cols},
        model_path(model_name)
    )

    print(f"[Saved] {model_name}")
    return True

# =========================================================
# PREDICTION
# =========================================================
def predict_latest(df: pd.DataFrame):
    latest = df.tail(1).copy()
    outputs = {}

    model_files = {
        "aqi_10sec": "model_aqi_10sec.pkl",
        "aqi_30sec": "model_aqi_30sec.pkl",
        "aqi_60sec": "model_aqi_60sec.pkl",
        "co2_10sec": "model_co2_10sec.pkl",
        "co2_30sec": "model_co2_30sec.pkl",
        "co2_60sec": "model_co2_60sec.pkl",
        "action": "model_action.pkl",
        "anomaly": "model_anomaly.pkl"
    }

    for key, file_name in model_files.items():
        full_path = model_path(file_name)
        if not os.path.exists(full_path):
            continue

        obj = joblib.load(full_path)
        model = obj["model"]
        features = obj["features"]
        X = latest[features]

        if key == "anomaly":
            pred = model.predict(X)[0]
            outputs[key] = "ANOMALY" if pred == -1 else "NORMAL"
        else:
            outputs[key] = model.predict(X)[0]

    return outputs

# =========================================================
# MAIN
# =========================================================
def main():
    try:
        csv_file = find_csv_file()
        df = load_data(csv_file)
        df = add_features(df)
        df = add_targets(df)

        print("\nTraining models...\n")

        train_regressor(df, "target_aqi_10sec", "model_aqi_10sec.pkl")
        train_regressor(df, "target_aqi_30sec", "model_aqi_30sec.pkl")
        train_regressor(df, "target_aqi_60sec", "model_aqi_60sec.pkl")

        train_regressor(df, "target_co2_10sec", "model_co2_10sec.pkl")
        train_regressor(df, "target_co2_30sec", "model_co2_30sec.pkl")
        train_regressor(df, "target_co2_60sec", "model_co2_60sec.pkl")

        train_classifier(df, "target_action", "model_action.pkl")
        train_anomaly_model(df, "model_anomaly.pkl")

        outputs = predict_latest(df)

        print("\n===== LATEST PREDICTIONS =====")
        if not outputs:
            print("No prediction models available yet. Need more data.")
        else:
            for k, v in outputs.items():
                if isinstance(v, (np.floating, float)):
                    print(f"{k}: {float(v):.2f}")
                else:
                    print(f"{k}: {v}")

    except Exception as e:
        print("\n[ERROR]")
        print(str(e))

if __name__ == "__main__":
    main()