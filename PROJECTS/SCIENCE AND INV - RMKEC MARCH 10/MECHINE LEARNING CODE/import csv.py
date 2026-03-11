import csv
import random

output_file = "air_quality_data_200.csv"

tariff_per_kwh = 8.0
row_interval_sec = 2
energy_kwh = 0.0

headers = [
    "time_s", "aqi", "co2", "temp", "humidity", "nh3", "benzene", "co",
    "busVoltage", "current_mA", "power_W", "energy_kWh", "cost",
    "occupancy", "airCtrl", "mode", "dhtErr", "inaErr", "mqErr"
]

rows = []

aqi = 80
co2 = 650
temp = 30.0
humidity = 62.0
bus_voltage = 12.0
occupancy = 1
air_ctrl = 1
mode = "AUTO"

for i in range(200):
    time_s = i * row_interval_sec

    # Create realistic changing phases
    if i < 40:
        # Mild rise
        aqi += random.randint(1, 4)
        co2 += random.randint(8, 20)
        temp += random.uniform(0.00, 0.05)
        humidity += random.uniform(0.00, 0.10)
    elif i < 80:
        # Stronger rise
        aqi += random.randint(2, 6)
        co2 += random.randint(15, 30)
        temp += random.uniform(0.02, 0.07)
        humidity += random.uniform(0.00, 0.12)
    elif i < 120:
        # High pollution region
        aqi += random.randint(-2, 5)
        co2 += random.randint(5, 20)
        temp += random.uniform(-0.01, 0.05)
        humidity += random.uniform(-0.05, 0.10)
    elif i < 160:
        # Ventilation improves air
        aqi -= random.randint(2, 6)
        co2 -= random.randint(15, 35)
        temp += random.uniform(-0.03, 0.02)
        humidity += random.uniform(-0.10, 0.05)
    else:
        # Stabilizing
        aqi += random.randint(-3, 3)
        co2 += random.randint(-20, 15)
        temp += random.uniform(-0.03, 0.03)
        humidity += random.uniform(-0.08, 0.08)

    # Keep values in realistic range
    aqi = max(40, min(420, aqi))
    co2 = max(400, min(2600, co2))
    temp = max(26.0, min(38.0, temp))
    humidity = max(40.0, min(85.0, humidity))

    # Occupancy pattern
    if 50 <= i < 140:
        occupancy = 1
    elif 140 <= i < 170:
        occupancy = 0
    else:
        occupancy = 1 if random.random() > 0.25 else 0

    # AirCtrl logic
    if (occupancy == 1 and aqi > 100) or (occupancy == 0 and aqi > 200):
        air_ctrl = 1
    else:
        air_ctrl = 0

    # Power/current based on AirCtrl
    if air_ctrl == 1:
        current_mA = random.uniform(320, 620)
        power_W = random.uniform(4.0, 7.8)
    else:
        current_mA = random.uniform(80, 180)
        power_W = random.uniform(0.8, 2.2)

    # Gas approximations from AQI bands
    if aqi <= 50:
        nh3 = random.uniform(1.0, 3.0)
        benzene = random.uniform(0.3, 0.8)
        co = random.uniform(0.05, 0.15)
    elif aqi <= 100:
        nh3 = random.uniform(5.0, 12.0)
        benzene = random.uniform(1.0, 2.5)
        co = random.uniform(0.2, 0.4)
    elif aqi <= 150:
        nh3 = random.uniform(18.0, 35.0)
        benzene = random.uniform(3.0, 5.0)
        co = random.uniform(0.5, 1.0)
    elif aqi <= 200:
        nh3 = random.uniform(45.0, 65.0)
        benzene = random.uniform(5.5, 8.0)
        co = random.uniform(1.1, 1.8)
    elif aqi <= 300:
        nh3 = random.uniform(80.0, 150.0)
        benzene = random.uniform(8.5, 15.0)
        co = random.uniform(2.0, 3.8)
    else:
        nh3 = random.uniform(165.0, 320.0)
        benzene = random.uniform(16.0, 28.0)
        co = random.uniform(4.0, 7.0)

    # Energy and cost
    energy_kwh += power_W * (row_interval_sec / 3600.0) / 1000.0
    cost = energy_kwh * tariff_per_kwh

    row = [
        time_s,
        round(aqi, 2),
        round(co2, 2),
        round(temp, 2),
        round(humidity, 2),
        round(nh3, 2),
        round(benzene, 2),
        round(co, 2),
        round(bus_voltage + random.uniform(-0.2, 0.2), 2),
        round(current_mA, 2),
        round(power_W, 3),
        round(energy_kwh, 6),
        round(cost, 4),
        occupancy,
        air_ctrl,
        mode,
        0,
        0,
        0
    ]
    rows.append(row)

with open(output_file, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(headers)
    writer.writerows(rows)

print(f"Done. Created {output_file} with {len(rows)} rows.")
