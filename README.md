# Smart Indoor Air Quality Monitor with Mould Risk Prediction

An ESP32-based indoor air quality monitor that predicts mould growth risk in real time, using the published Hukka–Viitanen / ASHRAE Standard 160 mould-risk model — run continuously on a low-cost microcontroller, with a self-hosted web dashboard.

**MSc Computing (Software Engineering) Dissertation — University of Northampton**
Parastoo Shojaei (Student ID: 25828319) · Supervisor: Dr Suraj Ajit

---

## What this project does

Most commercial air-quality monitors either don't predict mould risk at all, or do so with a closed, unpublished algorithm. This project combines full indoor air-quality sensing with an **open, scientifically validated mould-risk prediction model**, running live on a low-cost ESP32 device — with every equation, every line of code and every test fully documented and reproducible.

- Reads temperature, humidity, CO2, VOCs and PM2.5 every 5 seconds
- Calculates a continuous mould growth risk score (0–10) using the ASHRAE-standardised Hukka–Viitanen model
- Shows live status on an onboard OLED display and RGB LED — no phone or WiFi required to read it
- Serves a self-hosted web dashboard (no cloud, no login) with live gauges, trend charts, and CSV log download
- Validated against an independent Python re-implementation and benchmarked against a commercial device (Airthings Wave Mini)
- Designed as a manufacturable two-layer PCB (DRC-clean), costed at £62 for one unit / £30 at 100 units

## Repository structure

```
firmware/
  Main/Main.ino                  Main device firmware (Arduino/ESP32)

hardware/
  Altium_project/                Schematic and PCB design (Altium Designer)
    Project/Project.PrjPcb         Project file — open this first
    Project/Schematic.SchDoc       Schematic
    Project/PCB.PcbDoc             PCB layout (46.43 x 35.59 mm, 2-layer, 0 DRC violations)

tests/
  python/                        Independent Python re-implementation of the mould-risk algorithm
    generate_dataset.py            Generates a 12-month synthetic dataset (fixed seed)
    reference_model.py             Reference algorithm implementation
    synthetic_12month_dataset.csv
    reference_ashrae_output.csv    Python model output
  esp32_test_sketch/
    test_mould_algorithm/          Runs the same 12-month dataset on actual ESP32 firmware
      test_mould_algorithm.ino
      dataset_12month.h              Dataset as C++ arrays
      generate_header.py             Builds dataset_12month.h from the CSV
      test_results_12month.csv       ESP32 output, compared against the Python reference

data/
  air_quality_log.csv            Real sensor log from the prototype, 14–19 September 2026

docs/
  Dissertation_Report.docx       Full dissertation write-up

evidence/
  Evidence_of_system.docx        Photos of the prototype, dashboard, test runs and PCB
```

## Hardware

- **ESP32-WROOM-32E** — microcontroller running the sensors, algorithm, logging and dashboard
- **BME688** — temperature, humidity, VOCs
- **SCD40** — CO2
- **PMS5003** — PM2.5
- OLED display + RGB status LED for on-device status (green = safe, red = high risk)
- USB power with battery backup

## Running the firmware

Requires an ESP32 prototype wired as described in Chapter 4 of the dissertation.

1. Install **Arduino IDE 2.3.2** and the **esp32 by Espressif Systems** board package (v3.3.11).
2. Install these libraries via the Arduino Library Manager: `Adafruit GFX Library`, `Adafruit SSD1306`, `Adafruit BME680 Library`, `Sensirion I2C SCD4x`.
3. Open `firmware/Main/Main.ino` in the Arduino IDE — keep it inside its `Main` folder.
4. At the top of the file, set your own WiFi `ssid` and `password`.
5. Select **Tools > Board > ESP32 Dev Module**, choose the port, and upload.
6. Open the Serial Monitor at 115200 baud to see the WiFi connection and device IP address.
7. On a device on the same network, open `http://<IP-ADDRESS>/` in a browser.

The dashboard shows a live mould-risk gauge (0–10, SAFE/CAUTION/HIGH RISK), five sensor tiles with GOOD/FAIR/POOR status, and tap-to-expand trend charts (24h / 7d / all-time). `/download` exports the full log as CSV; the page itself refreshes every 5 seconds with no login required.

## Validating the algorithm

The mould-risk algorithm is checked two ways (see Chapter 5 of the dissertation):

1. **Real-world comparison** — the device ran side by side with an Airthings Wave Mini in the same room for several days.
2. **Synthetic year validation** — because real testing conditions never got risky enough to trigger high mould scores, the algorithm was also run against a full simulated year (8,760 hourly readings, fixed random seed) including a deliberate 3-week leak and a dry-spell recovery. The same dataset was run through two independent implementations — Python and the actual ESP32 firmware — and compared hour by hour. Results agreed to within 0.000077 across all 8,760 hours.

To reproduce:

```bash
cd tests/python
pip install numpy pandas
python generate_dataset.py     # builds synthetic_12month_dataset.csv
python reference_model.py      # builds reference_ashrae_output.csv
```

```bash
cd tests/esp32_test_sketch/test_mould_algorithm
python generate_header.py      # builds dataset_12month.h from the CSV
```

Then open `test_mould_algorithm.ino` in the Arduino IDE (set your own WiFi credentials near the top), upload to an ESP32, and read the results from `http://<IP-ADDRESS>/download`.

## Hardware design files

`hardware/Altium_project/Project/` is the full Altium Designer project — open `Project.PrjPcb` to load it. The PCB is a two-layer, 46.43 × 35.59 mm board that passes Design Rule Check with zero warnings and zero violations.

## License

This project was produced as part of an MSc dissertation at the University of Northampton. Please get in touch before reusing it commercially.
