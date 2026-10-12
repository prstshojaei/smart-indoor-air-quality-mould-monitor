"""
Converts the 12-month synthetic dataset (CSV) into a C++ header file that the
ESP32 can read directly from its own program memory

"""

import pandas as pd
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
input_path = os.path.join(script_dir, 'synthetic_12month_dataset.csv')
output_path = os.path.join(script_dir, 'dataset_12month.h')

print(f"Reading dataset from: {input_path}")
df = pd.read_csv(input_path)

humidity = df['humidity_pct'].values
temperature = df['temperature_c'].values
row_count = len(df)

print(f"Row count: {row_count}")
print("Writing C++ header file...")

with open(output_path, 'w') as f:
    f.write("// Auto-generated from synthetic_12month_dataset.csv -- do not edit by hand.\n")
    f.write("// Contains the 12-month hourly humidity and temperature data as PROGMEM\n")
    f.write("// arrays, so the ESP32 can run the validation test without needing a real\n")
    f.write("// sensor or an SD card.\n\n")
    f.write(f"#define DATASET_SIZE {row_count}\n\n")

    f.write("const float datasetHumidity[DATASET_SIZE] PROGMEM = {\n")
    f.write(",".join(f"{v:.2f}" for v in humidity))
    f.write("\n};\n\n")

    f.write("const float datasetTemp[DATASET_SIZE] PROGMEM = {\n")
    f.write(",".join(f"{v:.2f}" for v in temperature))
    f.write("\n};\n")

print(f"\nDone! Header file saved to: {output_path}")
print(f"File size: {os.path.getsize(output_path) / 1024:.1f} KB")