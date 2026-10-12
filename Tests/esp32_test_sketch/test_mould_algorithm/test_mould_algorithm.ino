// ============================================================
// Mould-risk algorithm test sketch, run against a synthetic 12-month dataset
// Full, exact implementation of the Viitanen model
//
//
// How to use:
// 1. Make sure this file and dataset_12month.h are in the same folder
// 2. Upload this sketch to the ESP32
// 3. Open the Serial Monitor (115200 baud) and wait until it prints
//    "Simulation complete"
// 4. Open the IP address it prints in a browser and go to /download to get
//    the results file
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <math.h>
#include "dataset_12month.h"

// ===== WiFi (same network as the main device) =====
const char* ssid = "St Michael's Network";
const char* password = "stM1ch4els4venue*";

WebServer server(80);

#define RESULT_FILE "/test_results.csv"
#define MATERIAL_W 0.0   // wood species: 0 = pine
#define MATERIAL_SQ 0.0  // surface quality: 0 = resawn (rough-cut) wood

// ===== Equation 1: critical relative humidity threshold for a given temperature =====
float calculateRHcrit(float tempC) {
  if (tempC <= 20.0) {
    return -0.00267 * tempC * tempC * tempC + 0.160 * tempC * tempC - 3.13 * tempC + 100.0;
  } else {
    return 80.0;
  }
}

// ===== Equation 2: maximum mould index reachable at the current humidity =====
float calculateMmax(float RH, float rhCrit) {
  if (rhCrit >= 100.0) return 6.0;
  float ratio = (rhCrit - RH) / (rhCrit - 100.0);
  float mmax = 1.0 + 7.0 * ratio - 2.0 * ratio * ratio;
  if (mmax < 0) mmax = 0;
  if (mmax > 6.0) mmax = 6.0;
  return mmax;
}

// ===== Equation 3: time (weeks) to reach mould index M=1 =====
float calculateTm(float T, float RH) {
  return exp(-0.68 * log(T) - 13.9 * log(RH) + 0.14 * MATERIAL_W - 0.33 * MATERIAL_SQ + 66.02);
}

// ===== Equation 5: time (weeks) to reach M=3, the point mould becomes visible =====
float calculateTv(float T, float RH) {
  return exp(-0.74 * log(T) - 12.72 * log(RH) + 0.06 * MATERIAL_W + 61.50);
}

// ===== Equation 6: correction factor that slows growth once mould has started =====
float calculateK1(float M, float T, float RH) {
  if (M < 1.0) return 1.0;
  float tm = calculateTm(T, RH);
  float tv = calculateTv(T, RH);
  float denom = (tv / tm) - 1.0;
  if (denom <= 0) return 1.0;
  return 2.0 / denom;
}

// ===== Converts the mould index (0-6) into the 0-10 score shown to the user =====
// The score bands follow the descriptions of the mould index itself
// (Viitanen and Ojanen, 2007, Table 1):
//   M < 1       : no growth / spores not yet active       -> score 0-3   (SAFE)
//   1 <= M < 3  : microscopic growth, not yet visible      -> score 4-6   (CAUTION)
//   M >= 3      : mould visible to the eye                 -> score 7-10  (HIGH RISK)
int mouldScoreFromIndex(float M) {
  float s;
  if (M < 1.0) {
    s = 3.5 * M;                              // 0 -> 3.5
  } else if (M < 3.0) {
    s = 3.5 + 1.5 * (M - 1.0);                // 3.5 -> 6.5
  } else {
    s = 6.5 + (3.5 / 3.0) * (M - 3.0);        // 6.5 -> 10
  }
  int score = (int)round(s);
  if (score < 0) score = 0;
  if (score > 10) score = 10;
  return score;
}

// Serves the saved results file for download over WiFi
void handleDownload() {
  File f = LittleFS.open(RESULT_FILE, "r");
  if (!f) {
    server.send(404, "text/plain", "Result file not found.");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=test_results_12month.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Mould Algorithm 12-Month Simulation Test (full Viitanen model) ===");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    return;
  }

  LittleFS.remove(RESULT_FILE);
  File out = LittleFS.open(RESULT_FILE, "w");
  if (!out) {
    Serial.println("Cannot create result file!");
    return;
  }
  out.println("hour,temperature_c,humidity_pct,rhcrit,mould_index_06,mould_score_010");

  float mouldIndex = 0.0;
  float dryStreakHours = 0.0;
  float maxIndex = 0.0;  // highest mould index reached during the year
  int maxHour = 0;       // the hour at which that maximum happened

  Serial.print("Running simulation over ");
  Serial.print(DATASET_SIZE);
  Serial.println(" hourly data points...");

  unsigned long startMs = millis();

  // ===== Run the model once for every hour in the dataset =====
  for (int i = 0; i < DATASET_SIZE; i++) {
    float humidity = pgm_read_float(&datasetHumidity[i]);
    float temp = pgm_read_float(&datasetTemp[i]);
    float elapsedHours = 1.0;  // each row in this dataset represents exactly 1 hour

    float rhCrit = calculateRHcrit(temp);

    if (humidity > rhCrit && temp > 0) {
      // ===== Conditions favour growth: Equation 8 =====
      dryStreakHours = 0.0;

      float mmax = calculateMmax(humidity, rhCrit);
      float k1 = calculateK1(mouldIndex, temp, humidity);
      float k2 = 1.0 - exp(2.3 * (mouldIndex - mmax));
      if (k2 < 0) k2 = 0;

      float lnT = log(temp);
      float lnRH = log(humidity);
      float exponent = -0.68 * lnT - 13.9 * lnRH + 0.14 * MATERIAL_W - 0.33 * MATERIAL_SQ + 66.02;
      float denom = 7.0 * exp(exponent);

      // The published growth rate is 1/(7*tm) with tm in weeks, i.e. a rate per DAY
      float dM_perDay = (denom > 0) ? (k1 * k2 / denom) : 0.0;
      float dM_perHour = dM_perDay / 24.0;  // convert daily rate to hourly rate

      mouldIndex += dM_perHour * elapsedHours;
    } else {
      // ===== Too dry for growth: Equation 9 (decline) =====
      dryStreakHours += elapsedHours;

      float dM_perHour;
      if (dryStreakHours <= 6.0) {
        dM_perHour = -0.032 / 24.0;  // published rate is -0.032 per day
      } else if (dryStreakHours <= 24.0) {
        dM_perHour = 0.0;
      } else {
        dM_perHour = -0.016 / 24.0;  // published rate is -0.016 per day
      }

      mouldIndex += dM_perHour * elapsedHours;
    }

    if (mouldIndex < 0) mouldIndex = 0;
    if (mouldIndex > 6.0) mouldIndex = 6.0;

    if (mouldIndex > maxIndex) {
      maxIndex = mouldIndex;
      maxHour = i;
    }

    int mouldScore = mouldScoreFromIndex(mouldIndex);

    out.print(i); out.print(",");
    out.print(temp, 2); out.print(",");
    out.print(humidity, 2); out.print(",");
    out.print(rhCrit, 2); out.print(",");
    out.print(mouldIndex, 4); out.print(",");
    out.println(mouldScore);

    // Print progress every 1000 rows so we can see it's still running
    if (i % 1000 == 0) {
      Serial.print("Progress: ");
      Serial.print(i);
      Serial.print(" / ");
      Serial.println(DATASET_SIZE);
    }
  }

  out.close();

  unsigned long durationMs = millis() - startMs;
  Serial.println("\n=== Simulation complete! ===");
  Serial.print("Time taken: ");
  Serial.print(durationMs / 1000.0);
  Serial.println(" seconds");
  Serial.print("Final mould index (0-6): ");
  Serial.println(mouldIndex, 3);
  Serial.print("Final mould score (0-10): ");
  Serial.println(mouldScoreFromIndex(mouldIndex));
  Serial.print("Maximum mould index (0-6): ");
  Serial.print(maxIndex, 4);
  Serial.print(" at hour ");
  Serial.println(maxHour);

  // ===== Connect to WiFi so the results file can be downloaded =====
  Serial.println("\nConnecting to WiFi to allow download...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  delay(100);

  // Scan for "Michael" (without the apostrophe) instead of matching the exact
  // SSID string -- this avoids a known issue where a typographic apostrophe
  // typed elsewhere doesn't match the plain apostrophe hardcoded here.
  Serial.println("--- Scanning for WiFi network ---");
  int netCount = WiFi.scanNetworks();
  int bestIndex = -1;
  int bestRSSI = -1000;
  for (int i = 0; i < netCount; i++) {
    Serial.print(i + 1);
    Serial.print(": '");
    Serial.print(WiFi.SSID(i));
    Serial.print("' (RSSI: ");
    Serial.print(WiFi.RSSI(i));
    Serial.println(")");
    if (WiFi.SSID(i).indexOf("Michael") >= 0 && WiFi.RSSI(i) > bestRSSI) {
      bestRSSI = WiFi.RSSI(i);
      bestIndex = i;
    }
  }
  Serial.println("--- End Scan ---");

  if (bestIndex >= 0) {
    String exactSsid = WiFi.SSID(bestIndex);
    Serial.print("Found network: '");
    Serial.print(exactSsid);
    Serial.println("' -- connecting with exact scanned name...");
    WiFi.begin(exactSsid.c_str(), password);
  } else {
    Serial.println("Network not found in scan! Falling back to hardcoded SSID.");
    WiFi.begin(ssid, password);
  }

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! Open this in your browser to download results: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/download");
  } else {
    Serial.println("WiFi connection failed. Results are still saved on the device's flash.");
  }

  server.on("/download", handleDownload);
  server.begin();
}

void loop() {
  server.handleClient();
}