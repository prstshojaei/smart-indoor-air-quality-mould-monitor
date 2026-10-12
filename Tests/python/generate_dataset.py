"""
Generates a synthetic 12-month (8760 hourly rows) dataset to test the
ASHRAE/Viitanen mould-risk algorithm.

The dataset includes:
- A seasonal sine-wave pattern for temperature and humidity (summer/winter cycle)
- A daily humidity cycle (higher at night, lower at midday)
- A short daily "shower" humidity spike
- A 3-week simulated leak event (days 100-121) with sustained high humidity (~95-98%)
- A 10-day dry spell (days 200-210) with low humidity
"""

import numpy as np
import pandas as pd
import os

np.random.seed(
    42
)  # fixed seed so the "random" values are the same every time we run this

HOURS_PER_YEAR = 365 * 24  # 8760 hours in a year
hour = np.arange(HOURS_PER_YEAR)  # hour number: 0, 1, 2, ... 8759
day = hour // 24  # which day each hour belongs to: 0, 0, ... 1, 1, ...
hour_of_day = hour % 24  # hour within the day: 0-23, repeating
day_of_year = day % 365

# ===== Seasonal temperature: yearly average ~14C, swinging +-8C (cold winter, warm summer) =====
seasonal_temp = 14.0 + 8.0 * np.sin(2 * np.pi * (day_of_year - 100) / 365.0)
daily_temp_cycle = 1.5 * np.sin(
    2 * np.pi * (hour_of_day - 15) / 24.0
)  # peaks in the afternoon
temperature_c = (
    seasonal_temp + daily_temp_cycle + np.random.normal(0, 0.3, HOURS_PER_YEAR)
)
temperature_c = np.clip(temperature_c, 2, 32)  # keep values within a realistic range

# ===== Seasonal humidity: yearly average ~55%, swinging +-12% (wetter in winter) =====
seasonal_rh = 55.0 - 12.0 * np.sin(2 * np.pi * (day_of_year - 100) / 365.0)
daily_rh_cycle = -6.0 * np.sin(
    2 * np.pi * (hour_of_day - 15) / 24.0
)  # lower at midday, higher at night
humidity_pct = seasonal_rh + daily_rh_cycle + np.random.normal(0, 1.5, HOURS_PER_YEAR)

# ===== Daily "shower" spike: every day at 7-8am, humidity jumps up briefly =====
shower_hour = 7
shower_mask = (hour_of_day == shower_hour) | (hour_of_day == shower_hour + 1)
humidity_pct[shower_mask] += 25.0

# ===== 3-week leak event: days 100-121, humidity held high (simulates a burst pipe) =====
leak_mask = (day >= 100) & (day < 121)
humidity_pct[leak_mask] = 95.0 + np.random.normal(0, 1.5, leak_mask.sum())

# ===== 10-day dry spell: days 200-210, humidity drops =====
dry_mask = (day >= 200) & (day < 210)
humidity_pct[dry_mask] -= 25.0

humidity_pct = np.clip(humidity_pct, 15, 98)  # keep values within a realistic range

# ===== Build the final table and save it as a CSV file =====
df = pd.DataFrame(
    {
        "hour": hour,
        "day": day,
        "temperature_c": np.round(temperature_c, 2),
        "humidity_pct": np.round(humidity_pct, 2),
    }
)

# Save the CSV in the same folder as this script, regardless of where it's run from
script_dir = os.path.dirname(os.path.abspath(__file__))
out_path = os.path.join(script_dir, "synthetic_12month_dataset.csv")
df.to_csv(out_path, index=False)

# ===== Print a quick summary so we can sanity-check the numbers look reasonable =====
print(f"Dataset created: {out_path}")
print(f"Row count: {len(df)}")
print(
    f"Temperature: min={df.temperature_c.min()}, max={df.temperature_c.max()}, mean={df.temperature_c.mean():.1f}"
)
print(
    f"Humidity: min={df.humidity_pct.min()}, max={df.humidity_pct.max()}, mean={df.humidity_pct.mean():.1f}"
)
