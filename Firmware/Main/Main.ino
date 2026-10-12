#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SensirionI2cScd4x.h>
#include <Adafruit_BME680.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <time.h>

#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

// ===== WiFi =====
const char* ssid = "St Michael's Network";
const char* password = "stM1ch4els4venue*";

// ===== OLED =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===== RGB LED =====
#define RED_PIN 25
#define GREEN_PIN 26
#define BLUE_PIN 27

// ===== PMS5003 (PM2.5) =====
#define PMS_RX_PIN 16
#define PMS_TX_PIN 17
HardwareSerial pmsSerial(2);

// ===== Sensors =====
SensirionI2cScd4x scd4x;
Adafruit_BME680 bme;

// ===== Web Server =====
WebServer server(80);

// ===== Live Sensor Data =====
float currentTemp = 0;
float currentHumidity = 0;
float currentPressure = 0;
float currentGasKOhms = 0;
uint16_t currentCO2 = 0;
uint16_t currentPM1 = 0;
uint16_t currentPM25 = 0;
uint16_t currentPM10 = 0;
int mouldScore = 0;

bool scd40Ok = false;
bool bmeOk = false;
bool pmsOk = false;

// ===== Mould risk model constant
#define MATERIAL_W 0.0
#define MATERIAL_SQ 0.0

float mouldIndex = 0.0;
float dryStreakHours = 0.0;
unsigned long lastMouldUpdateMillis = 0;

// ===== History buffer for the small home-page humidity sparkline (last 20 points) =====
#define HISTORY_SIZE 20
float humidityHistory[HISTORY_SIZE];
int historyIndex = 0;
bool historyInitialized = false;

// ===== Timing =====
unsigned long lastSensorRead = 0;
unsigned long lastDisplayChange = 0;
int displayPage = 0;

// ===== Data Logging (LittleFS) =====
#define LOG_FILE "/datalog.csv"
#define LOG_MAX_BYTES 1000000UL
#define LOG_INTERVAL_MS 30000UL
bool fsOk = false;
bool timeSynced = false;
unsigned long lastLogWrite = 0;
unsigned long logEntryCount = 0;

// ========== FUNCTIONS ==========

void setLEDColor(int r, int g, int b) {
  digitalWrite(RED_PIN, r);
  digitalWrite(GREEN_PIN, g);
  digitalWrite(BLUE_PIN, b);
}

void scanI2C() {
  Serial.println("--- I2C Scan ---");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("Found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
    }
  }
  Serial.println("--- End Scan (expect 0x3C OLED, 0x62 SCD40, 0x76 BME688) ---");
}

void updateLEDFromScore(int score) {
  if (score <= 3) {
    setLEDColor(LOW, HIGH, LOW);
  } else if (score <= 6) {
    setLEDColor(HIGH, HIGH, LOW);
  } else {
    setLEDColor(HIGH, LOW, LOW);
  }
}

bool readPMS(uint16_t &pm1, uint16_t &pm25, uint16_t &pm10out) {
  if (pmsSerial.available() < 32) return false;

  if (pmsSerial.peek() != 0x42) {
    pmsSerial.read();
    return false;
  }

  uint8_t buffer[32];
  pmsSerial.readBytes(buffer, 32);

  if (buffer[0] != 0x42 || buffer[1] != 0x4D) return false;

  uint16_t sum = 0;
  for (int i = 0; i < 30; i++) sum += buffer[i];
  uint16_t checksum = (buffer[30] << 8) | buffer[31];
  if (sum != checksum) return false;

  pm1     = (buffer[10] << 8) | buffer[11];
  pm25    = (buffer[12] << 8) | buffer[13];
  pm10out = (buffer[14] << 8) | buffer[15];

  return true;
}

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

// ===== Full mould-risk algorithm: Equation 8 (growth) + Equation 9 (decline) =====
void calculateMouldRisk() {
  unsigned long nowMillis = millis();
  double elapsedHours;
  if (lastMouldUpdateMillis == 0) {
    elapsedHours = 5.0 / 3600.0;
  } else {
    elapsedHours = (nowMillis - lastMouldUpdateMillis) / 3600000.0;
  }
  lastMouldUpdateMillis = nowMillis;

  float rhCrit = calculateRHcrit(currentTemp);

  if (currentHumidity > rhCrit && currentTemp > 0) {
    // ===== Conditions favour growth =====
    dryStreakHours = 0.0;

    float mmax = calculateMmax(currentHumidity, rhCrit);
    float k1 = calculateK1(mouldIndex, currentTemp, currentHumidity);
    float k2 = 1.0 - exp(2.3 * (mouldIndex - mmax));
    if (k2 < 0) k2 = 0;

    float lnT = log(currentTemp);
    float lnRH = log(currentHumidity);
    float exponent = -0.68 * lnT - 13.9 * lnRH + 0.14 * MATERIAL_W - 0.33 * MATERIAL_SQ + 66.02;
    float denom = 7.0 * exp(exponent);

    // The published growth rate is 1/(7*tm) with tm in weeks, i.e. a rate per DAY
    float dM_perDay = (denom > 0) ? (k1 * k2 / denom) : 0.0;
    float dM_perHour = dM_perDay / 24.0;  // convert daily rate to hourly rate

    mouldIndex += dM_perHour * elapsedHours;
  } else {
    // ===== Too dry for growth: apply the decline rate instead =====
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

  // Convert to the 0-10 scale used by the dashboard gauge and the RGB LED
  mouldScore = mouldScoreFromIndex(mouldIndex);
}

String getTimestamp() {
  if (timeSynced) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
  } else {
    return "uptime_" + String(millis() / 1000) + "s";
  }
}

void initLogFile() {
  if (!LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "w");
    if (f) {
      f.println("timestamp,temperature_c,humidity_pct,co2_ppm,voc_kohm,pm1,pm2_5,pm10,mould_score,mould_index_06");
      f.close();
    }
  }
}

void logReading() {
  if (!fsOk) return;

  File check = LittleFS.open(LOG_FILE, "r");
  if (check) {
    size_t sz = check.size();
    check.close();
    if (sz > LOG_MAX_BYTES) {
      LittleFS.remove(LOG_FILE);
      initLogFile();
    }
  }

  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return;

  f.print(getTimestamp()); f.print(",");
  f.print(currentTemp, 1); f.print(",");
  f.print(currentHumidity, 1); f.print(",");
  f.print(currentCO2); f.print(",");
  f.print(currentGasKOhms, 1); f.print(",");
  f.print(currentPM1); f.print(",");
  f.print(currentPM25); f.print(",");
  f.print(currentPM10); f.print(",");
  f.print(mouldScore); f.print(",");
  f.println(mouldIndex, 3);

  f.close();
  logEntryCount++;
}

void handleDownload() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) {
    server.send(404, "text/plain", "No log file yet.");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=air_quality_log.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleClearLog() {
  LittleFS.remove(LOG_FILE);
  initLogFile();
  logEntryCount = 0;
  server.sendHeader("Location", "/");
  server.send(303);
}

void countExistingLogEntries() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;
  unsigned long count = 0;
  bool firstLine = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (firstLine) { firstLine = false; continue; }
    if (line.length() > 0) count++;
  }
  f.close();
  logEntryCount = count;
}

void readSensors() {
  if (bme.performReading()) {
    currentTemp = bme.temperature;
    currentHumidity = bme.humidity;
    currentPressure = bme.pressure / 100.0;
    currentGasKOhms = bme.gas_resistance / 1000.0;
    bmeOk = true;
  } else {
    bmeOk = false;
  }

  bool scdReady = false;
  int16_t err = scd4x.getDataReadyStatus(scdReady);
  if (err == NO_ERROR && scdReady) {
    uint16_t co2 = 0;
    float scdTemp = 0, scdHumidity = 0;
    err = scd4x.readMeasurement(co2, scdTemp, scdHumidity);
    if (err == NO_ERROR && co2 != 0) {
      currentCO2 = co2;
      scd40Ok = true;
    }
  }

  uint16_t p1, p25, p10;
  if (readPMS(p1, p25, p10)) {
    currentPM1 = p1;
    currentPM25 = p25;
    currentPM10 = p10;
    pmsOk = true;
  }

  calculateMouldRisk();

  // The first time we get a real humidity reading, fill the whole history
  // buffer with it (instead of a fixed placeholder like 50), so the sparkline
  // doesn't show a fake jump right at startup
  if (!historyInitialized) {
    for (int i = 0; i < HISTORY_SIZE; i++) {
      humidityHistory[i] = currentHumidity;
    }
    historyInitialized = true;
  }

  humidityHistory[historyIndex] = currentHumidity;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;

  updateLEDFromScore(mouldScore);

  // Sensors are read every 5 seconds (for algorithm accuracy), but we only
  // write a new log line every 30 seconds (to keep more history in less space)
  if (millis() - lastLogWrite >= LOG_INTERVAL_MS || lastLogWrite == 0) {
    lastLogWrite = millis();
    logReading();
  }
}

// ===== OLED Pages =====
void showMainReadingsPage() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("Air Quality Monitor");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  display.setCursor(0, 15);
  display.print("Temp: ");
  display.print(currentTemp, 1);
  display.print(" C   RH: ");
  display.print(currentHumidity, 0);
  display.println("%");

  display.setCursor(0, 27);
  display.print("CO2: ");
  display.print(currentCO2);
  display.println(" ppm");

  display.setCursor(0, 39);
  display.print("Gas: ");
  display.print(currentGasKOhms, 1);
  display.println(" KOhm");

  display.setCursor(0, 51);
  display.print("PM2.5: ");
  display.print(currentPM25);
  display.println(" ug/m3");

  display.display();
}

void showMouldScorePage() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("Mould Risk Score");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  display.setTextSize(3);
  display.setCursor(30, 20);
  display.print(mouldScore);
  display.setTextSize(1);
  display.print(" /10");

  display.setCursor(0, 50);
  if (mouldScore <= 3) {
    display.println("Status: SAFE");
  } else if (mouldScore <= 6) {
    display.println("Status: CAUTION");
  } else {
    display.println("Status: HIGH RISK");
  }

  display.display();
}

void showNetworkInfoPage() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("Network Info");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  display.setCursor(0, 20);
  if (WiFi.status() == WL_CONNECTED) {
    display.println("WiFi: Connected");
  } else {
    display.println("WiFi: NOT connected");
  }

  display.setCursor(0, 32);
  display.print("IP: ");
  display.println(WiFi.localIP());

  display.setCursor(0, 48);
  display.print("SCD:"); display.print(scd40Ok ? "OK " : "ERR ");
  display.print("BME:"); display.print(bmeOk ? "OK " : "ERR ");
  display.print("PM:"); display.println(pmsOk ? "OK" : "ERR");

  display.display();
}

void updateDisplay() {
  if (displayPage == 0) showMainReadingsPage();
  else if (displayPage == 1) showMouldScorePage();
  else showNetworkInfoPage();
}

// ===== Small humidity sparkline shown on the home dashboard page 
String buildTrendChart() {
  String svg = "<svg width='100%' height='70' viewBox='0 0 300 70' preserveAspectRatio='none'>";
  svg += "<defs><linearGradient id='fadeFill' x1='0' y1='0' x2='0' y2='1'>";
  svg += "<stop offset='0%' stop-color='#E4A576' stop-opacity='0.35'/>";
  svg += "<stop offset='100%' stop-color='#E4A576' stop-opacity='0'/>";
  svg += "</linearGradient></defs>";

  float maxVal = 100, minVal = 0;
  int stepX = 300 / (HISTORY_SIZE - 1);
  String points = "";
  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (historyIndex + i) % HISTORY_SIZE;
    float val = humidityHistory[idx];
    int x = i * stepX;
    int y = 60 - (int)((val - minVal) / (maxVal - minVal) * 55);
    points += String(x) + "," + String(y) + " ";
  }

  svg += "<polyline points='" + points + "' fill='none' stroke='#E4A576' stroke-width='2.5' stroke-linejoin='round' stroke-linecap='round'/>";
  svg += "<polyline points='" + points + "300,70 0,70' fill='url(#fadeFill)' stroke='none'/>";
  svg += "</svg>";
  return svg;
}

// ===== Full-page trend chart 
#define TREND_POINTS 100
float trendBuffer[TREND_POINTS];
time_t trendTimes[TREND_POINTS];
int trendCount = 0;

// Quickly turns a log timestamp string into an epoch number (no String object,
// just a char pointer, so this stays fast even with a lot of log lines)
time_t parseLogTimestampFast(const char *tsStr) {
  if (strncmp(tsStr, "uptime_", 7) == 0) return 0;
  int y, mo, d, h, mi, s;
  if (sscanf(tsStr, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
  }
  return 0;
}

// Quickly pulls one column out of a CSV line
float extractColumnFast(char *line, int columnIndex) {
  int col = 0;
  char *tokenStart = line;
  char *ptr = line;
  while (*ptr) {
    if (*ptr == ',') {
      if (col == columnIndex) {
        *ptr = '\0';
        return atof(tokenStart);
      }
      col++;
      tokenStart = ptr + 1;
    }
    ptr++;
  }
  if (col == columnIndex) return atof(tokenStart);
  return 0;
}

// Reads the log file ONCE , with no String objects, and uses
// "reservoir sampling" to pick a representative set of points if there are
// more than TREND_POINTS matching rows -- this keeps the chart fast even
// with weeks of logged history.
// windowSeconds: 0 means "all history", a positive number means "only the
// last N seconds"
#define LINE_BUF_SIZE 96
void loadTrendData(int columnIndex, long windowSeconds) {
  trendCount = 0;
  time_t nowEpoch = time(nullptr);

  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;

  char lineBuf[LINE_BUF_SIZE];
  bool firstLine = true;
  unsigned long matchedIdx = 0;
  randomSeed(micros());

  while (f.available()) {
    int len = f.readBytesUntil('\n', lineBuf, LINE_BUF_SIZE - 1);
    if (len <= 0) continue;
    lineBuf[len] = '\0';
    if (len > 0 && lineBuf[len - 1] == '\r') lineBuf[len - 1] = '\0';

    if (firstLine) { firstLine = false; continue; }
    if (lineBuf[0] == '\0') continue;

    char *commaPos = strchr(lineBuf, ',');
    if (!commaPos) continue;
    *commaPos = '\0';
    time_t epoch = parseLogTimestampFast(lineBuf);
    *commaPos = ',';

    bool matches = (windowSeconds <= 0) || (epoch > 0 && (nowEpoch - epoch) <= windowSeconds && epoch <= nowEpoch);
    if (!matches) continue;

    float value = extractColumnFast(lineBuf, columnIndex);

    if (trendCount < TREND_POINTS) {
      trendBuffer[trendCount] = value;
      trendTimes[trendCount] = epoch;
      trendCount++;
    } else {
      unsigned long r = random(0, matchedIdx + 1);
      if (r < TREND_POINTS) {
        trendBuffer[r] = value;
        trendTimes[r] = epoch;
      }
    }
    matchedIdx++;
  }
  f.close();

  // Reservoir sampling scrambles the time order, so we sort back into
  // chronological order before drawing the chart
  for (int i = 0; i < trendCount - 1; i++) {
    for (int j = 0; j < trendCount - i - 1; j++) {
      if (trendTimes[j] > trendTimes[j + 1]) {
        time_t tmpT = trendTimes[j]; trendTimes[j] = trendTimes[j + 1]; trendTimes[j + 1] = tmpT;
        float tmpV = trendBuffer[j]; trendBuffer[j] = trendBuffer[j + 1]; trendBuffer[j + 1] = tmpV;
      }
    }
  }
}

String buildGenericTrendChart(const String& unitSuffix) {
  if (trendCount < 2) return "<p style='color:#9AAAB2;font-size:13px;'>Not enough logged data yet.</p>";

  float minVal = trendBuffer[0], maxVal = trendBuffer[0];
  for (int i = 1; i < trendCount; i++) {
    if (trendBuffer[i] < minVal) minVal = trendBuffer[i];
    if (trendBuffer[i] > maxVal) maxVal = trendBuffer[i];
  }
  if (maxVal == minVal) { maxVal += 1; minVal -= 1; }

  const int SVG_W = 600;
  const int SVG_H = 300;
  const int TOP_Y = 30;
  const int BOTTOM_Y = 270;
  const int RANGE_Y = BOTTOM_Y - TOP_Y;
  const int MID_Y = TOP_Y + RANGE_Y / 2;

  int stepX = SVG_W / (trendCount - 1);

  // ===== Build the main line + area fill, and at the same time build a JS
  // array literal with every point's pixel position, value, and a readable
  // date/time label =====
  String points = "";
  String jsPoints = "[";
  char labelBuf[24];

  for (int i = 0; i < trendCount; i++) {
    int x = i * stepX;
    int y = BOTTOM_Y - (int)((trendBuffer[i] - minVal) / (maxVal - minVal) * RANGE_Y);
    points += String(x) + "," + String(y) + " ";

    struct tm timeinfo;
    localtime_r(&trendTimes[i], &timeinfo);
    strftime(labelBuf, sizeof(labelBuf), "%a %d/%m, %H:%M", &timeinfo);

    jsPoints += "{x:" + String(x) + ",y:" + String(y) + ",v:" + String(trendBuffer[i], 1) + ",l:'" + String(labelBuf) + "'}";
    if (i < trendCount - 1) jsPoints += ",";
  }
  jsPoints += "]";

  // ===== Draw the axis grid lines and their number labels =====
  float midVal = (minVal + maxVal) / 2.0;

  String svg = "<svg id='trendSvg' width='100%' height='300' viewBox='0 0 " + String(SVG_W) + " " + String(SVG_H) + "' preserveAspectRatio='none' style='touch-action:none;'>";
  svg += "<defs><linearGradient id='fadeFill2' x1='0' y1='0' x2='0' y2='1'>";
  svg += "<stop offset='0%' stop-color='#E4A576' stop-opacity='0.35'/>";
  svg += "<stop offset='100%' stop-color='#E4A576' stop-opacity='0'/>";
  svg += "</linearGradient></defs>";

  svg += "<line x1='0' y1='" + String(TOP_Y) + "' x2='" + String(SVG_W) + "' y2='" + String(TOP_Y) + "' stroke='#E4D9C8' stroke-width='1' stroke-dasharray='4,4'/>";
  svg += "<line x1='0' y1='" + String(MID_Y) + "' x2='" + String(SVG_W) + "' y2='" + String(MID_Y) + "' stroke='#E4D9C8' stroke-width='1' stroke-dasharray='4,4'/>";
  svg += "<line x1='0' y1='" + String(BOTTOM_Y) + "' x2='" + String(SVG_W) + "' y2='" + String(BOTTOM_Y) + "' stroke='#E4D9C8' stroke-width='1' stroke-dasharray='4,4'/>";
  svg += "<text x='6' y='" + String(TOP_Y - 8) + "' font-size='16' font-weight='600' fill='#698EA2'>" + String(maxVal, 1) + "</text>";
  svg += "<text x='6' y='" + String(MID_Y - 8) + "' font-size='16' font-weight='600' fill='#698EA2'>" + String(midVal, 1) + "</text>";
  svg += "<text x='6' y='" + String(BOTTOM_Y - 8) + "' font-size='16' font-weight='600' fill='#698EA2'>" + String(minVal, 1) + "</text>";

  // ===== The line itself and its soft gradient fill underneath =====
  svg += "<polyline points='" + points + "' fill='none' stroke='#E4A576' stroke-width='2.5' stroke-linejoin='round' stroke-linecap='round'/>";
  svg += "<polyline points='" + points + String(SVG_W) + "," + String(BOTTOM_Y) + " 0," + String(BOTTOM_Y) + "' fill='url(#fadeFill2)' stroke='none'/>";

  svg += "<line id='chartGuideLine' x1='0' y1='" + String(TOP_Y) + "' x2='0' y2='" + String(BOTTOM_Y) + "' stroke='#152935' stroke-width='1' stroke-dasharray='3,3' opacity='0'/>";
  svg += "<circle id='chartHighlightDot' cx='0' cy='0' r='7' fill='#E4A576' stroke='#FFFFFF' stroke-width='3' opacity='0'/>";

  svg += "</svg>";

  // ===== The floating tooltip box
  String html = svg;
  html += "<div id='chartTooltip' style=\"";
  html += "display:none;position:absolute;background:#152935;color:#FFFFFF;";
  html += "padding:8px 12px;border-radius:10px;font-size:12px;line-height:1.5;";
  html += "pointer-events:none;white-space:nowrap;box-shadow:0 4px 12px rgba(0,0,0,0.25);";
  html += "transform:translate(-50%,-115%);z-index:10;\">";
  html += "<div id='chartTooltipLabel' style='color:#C9D4DA;'></div>";
  html += "<div id='chartTooltipValue' style='font-size:16px;font-weight:800;'></div>";
  html += "</div>";

  // ===== The script that makes the tooltip follow the user's finger/mouse =====
  html += "<script>";
  html += "(function(){";
  html += "var pts=" + jsPoints + ";";
  html += "var unit='" + unitSuffix + "';";
  html += "var svgEl=document.getElementById('trendSvg');";
  html += "var wrap=svgEl.parentNode;";
  html += "wrap.style.position='relative';";
  html += "var tip=document.getElementById('chartTooltip');";
  html += "var dot=document.getElementById('chartHighlightDot');";
  html += "var guide=document.getElementById('chartGuideLine');";
  html += "var lbl=document.getElementById('chartTooltipLabel');";
  html += "var val=document.getElementById('chartTooltipValue');";

  // Finds the data point whose x-position (in SVG units) is closest to
  // the pointer's x-position
  html += "function nearestPoint(svgX){";
  html += "var best=pts[0],bestDist=Math.abs(pts[0].x-svgX);";
  html += "for(var i=1;i<pts.length;i++){var d=Math.abs(pts[i].x-svgX);if(d<bestDist){bestDist=d;best=pts[i];}}";
  html += "return best;";
  html += "}";

  // Converts a real pixel position on screen into SVG viewBox units
  html += "function clientXtoSvgX(clientX){";
  html += "var rect=svgEl.getBoundingClientRect();";
  html += "var ratio=(clientX-rect.left)/rect.width;";
  html += "return ratio*" + String(SVG_W) + ";";
  html += "}";

  html += "function showAt(clientX,clientY){";
  html += "var svgX=clientXtoSvgX(clientX);";
  html += "var p=nearestPoint(svgX);";
  html += "dot.setAttribute('cx',p.x);dot.setAttribute('cy',p.y);dot.setAttribute('opacity','1');";
  html += "guide.setAttribute('x1',p.x);guide.setAttribute('x2',p.x);guide.setAttribute('opacity','1');";
  html += "lbl.textContent=p.l;";
  html += "val.textContent=p.v.toFixed(1)+unit;";
  html += "var rect=svgEl.getBoundingClientRect();";
  html += "var pixelX=rect.left+(p.x/" + String(SVG_W) + ")*rect.width;";
  html += "var pixelY=rect.top+(p.y/" + String(SVG_H) + ")*rect.height;";
  html += "tip.style.display='block';";
  html += "tip.style.left=(pixelX-wrap.getBoundingClientRect().left)+'px';";
  html += "tip.style.top=(pixelY-wrap.getBoundingClientRect().top)+'px';";
  html += "}";

  html += "function hide(){tip.style.display='none';dot.setAttribute('opacity','0');guide.setAttribute('opacity','0');}";

  // Mouse (desktop) events
  html += "svgEl.addEventListener('mousemove',function(e){showAt(e.clientX,e.clientY);});";
  html += "svgEl.addEventListener('mouseleave',hide);";
  // Touch (mobile) events
  html += "svgEl.addEventListener('touchstart',function(e){e.preventDefault();var t=e.touches[0];showAt(t.clientX,t.clientY);},{passive:false});";
  html += "svgEl.addEventListener('touchmove',function(e){e.preventDefault();var t=e.touches[0];showAt(t.clientX,t.clientY);},{passive:false});";

  // Show the most recent point by default, so the chart isn't empty on first load
  html += "showAt(svgEl.getBoundingClientRect().right,0);";
  html += "})();";
  html += "</script>";

  return html;
}

void handleTrend() {
  String param = server.hasArg("param") ? server.arg("param") : "humidity";
  String range = server.hasArg("range") ? server.arg("range") : "24h";

  int columnIndex = 2;
  String title = "Humidity";
  String unitSuffix = "%";

  if (param == "temperature") { columnIndex = 1; title = "Temperature"; unitSuffix = "\u00B0C"; }
  else if (param == "humidity") { columnIndex = 2; title = "Humidity"; unitSuffix = "%"; }
  else if (param == "co2") { columnIndex = 3; title = "CO2"; unitSuffix = " ppm"; }
  else if (param == "voc") { columnIndex = 4; title = "VOC (raw resistance)"; unitSuffix = " k\u03A9"; }
  else if (param == "pm25") { columnIndex = 6; title = "PM2.5"; unitSuffix = " \u00B5g/m\u00B3"; }
  else if (param == "mould") { columnIndex = 8; title = "Mould Risk Score"; unitSuffix = "/10"; }

  long windowSeconds = 24L * 3600L;
  if (range == "24h") windowSeconds = 24L * 3600L;
  else if (range == "7d") windowSeconds = 7L * 24L * 3600L;
  else if (range == "all") windowSeconds = 0;

  loadTrendData(columnIndex, windowSeconds);

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" + title + " Trend</title><style>";
  html += "*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:#F5EEE4;color:#152935;padding:20px 16px 32px;max-width:480px;margin:0 auto;}";
  html += ".back{display:inline-block;margin-bottom:16px;color:#698EA2;text-decoration:none;font-size:14px;font-weight:600;}";
  html += ".card{background:#FFFFFF;border:1px solid #E4D9C8;border-radius:18px;padding:19px;box-shadow:0 3px 10px rgba(21,41,53,0.04);position:relative;}";
  html += ".title{font-size:18px;font-weight:700;margin-bottom:4px;}";
  html += ".subtitle{font-size:12px;color:#9AAAB2;margin-bottom:16px;}";
  html += ".range-tabs{display:flex;gap:8px;margin-bottom:16px;}";
  html += ".range-tab{flex:1;text-align:center;padding:8px 0;border-radius:10px;font-size:13px;font-weight:700;text-decoration:none;background:#F5EEE4;color:#5C7180;}";
  html += ".range-tab.active{background:#E4A576;color:#FFFFFF;}";
  html += ".axis-labels{display:flex;justify-content:space-between;margin-top:6px;font-size:10px;color:#9AAAB2;}";
  html += ".hint{text-align:center;font-size:11px;color:#9AAAB2;margin-top:10px;}";
  html += "</style></head><body>";
  html += "<a class='back' href='/'>&larr; Back to dashboard</a>";
  html += "<div class='card'><div class='title'>" + title + " Trend</div>";

  html += "<div class='range-tabs'>";
  html += "<a class='range-tab" + String(range == "24h" ? " active" : "") + "' href='/trend?param=" + param + "&range=24h'>24h</a>";
  html += "<a class='range-tab" + String(range == "7d" ? " active" : "") + "' href='/trend?param=" + param + "&range=7d'>7d</a>";
  html += "<a class='range-tab" + String(range == "all" ? " active" : "") + "' href='/trend?param=" + param + "&range=all'>All</a>";
  html += "</div>";

  html += buildGenericTrendChart(unitSuffix);

  html += "<div class='axis-labels'>";
  if (trendCount >= 2) {
    int labelIdxs[5] = {0, trendCount / 4, trendCount / 2, (3 * trendCount) / 4, trendCount - 1};
    for (int li = 0; li < 5; li++) {
      int idx = labelIdxs[li];
      char buf[16];
      struct tm timeinfo;
      localtime_r(&trendTimes[idx], &timeinfo);
      strftime(buf, sizeof(buf), "%d/%m %H:%M", &timeinfo);
      html += "<span>" + String(buf) + "</span>";
    }
  }
  html += "</div>";

  html += "<div class='hint'>Tap or drag on the chart to see exact readings</div>";

  html += "</div>";
  html += "<script>";
  html += "var __refreshTimer=setTimeout(function(){location.reload();},30000);";
  html += "document.addEventListener('touchstart',function(){clearTimeout(__refreshTimer);},{passive:true});";
  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

String statusTag(float value, float goodMax, float fairMax) {
  if (value <= goodMax) return "<span class='tile-tag tag-good'>GOOD</span>";
  if (value <= fairMax) return "<span class='tile-tag tag-fair'>FAIR</span>";
  return "<span class='tile-tag tag-poor'>POOR</span>";
}

// For VOC: unlike the other sensors, a HIGHER resistance means CLEANER air
// (not dirtier), so the good/fair/poor logic is reversed here
String statusTagInverted(float value, float poorMax, float fairMax) {
  if (value <= poorMax) return "<span class='tile-tag tag-poor'>POOR</span>";
  if (value <= fairMax) return "<span class='tile-tag tag-fair'>FAIR</span>";
  return "<span class='tile-tag tag-good'>GOOD</span>";
}

void handleRoot() {
  String statusText = "SAFE";
  String statusClass = "status-safe";
  String statusIcon = "&#10003;";
  String statusMsg = "Conditions are within a healthy range";
  String gaugeColor = "#4A9B6E";

  if (mouldScore > 6) {
    statusText = "HIGH RISK"; statusClass = "status-risk"; statusIcon = "&#9888;";
    statusMsg = "Ventilate the room to reduce mould risk"; gaugeColor = "#B6482F";
  } else if (mouldScore > 3) {
    statusText = "CAUTION"; statusClass = "status-caution"; statusIcon = "&#9888;";
    statusMsg = "Humidity is trending high, monitor closely"; gaugeColor = "#B9762E";
  }

  int offset = 490 - (int)((mouldScore / 10.0) * 490);

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Parastoo's Air Quality Monitor</title>";
  html += "<style>";
  html += "*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:#F5EEE4;color:#152935;padding:20px 16px 32px;max-width:480px;margin:0 auto;}";
  html += ".header{display:flex;align-items:center;justify-content:space-between;padding:8px 4px 22px;}";
  html += ".brand{display:flex;align-items:center;gap:12px;}";
  html += ".brand-icon{width:38px;height:38px;background:#E4A576;border-radius:11px;box-shadow:0 3px 8px rgba(228,165,118,0.4);}";
  html += ".brand-name{font-size:19px;font-weight:700;letter-spacing:-0.2px;color:#152935;}";
  html += ".brand-sub{font-size:11px;color:#698EA2;font-weight:600;letter-spacing:0.3px;}";
  html += ".live-dot{width:8px;height:8px;background:#4A9B6E;border-radius:50%;display:inline-block;box-shadow:0 0 6px #4A9B6E;}";
  html += ".gauge-card{background:#FFFFFF;border:1px solid #E4D9C8;border-radius:22px;padding:30px 20px;text-align:center;margin-bottom:16px;box-shadow:0 4px 14px rgba(21,41,53,0.06);}";
  html += ".gauge-label{font-size:11px;text-transform:uppercase;letter-spacing:1.6px;color:#698EA2;font-weight:700;margin-bottom:18px;}";
  html += ".gauge-wrap{position:relative;width:180px;height:180px;margin:0 auto 14px;}";
  html += ".gauge-score{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);}";
  html += ".gauge-score .num{font-size:50px;font-weight:800;line-height:1;color:#152935;}";
  html += ".gauge-score .denom{font-size:12px;color:#698EA2;font-weight:600;}";
  html += ".status-badge{display:inline-block;padding:7px 18px;border-radius:999px;font-size:13px;font-weight:700;letter-spacing:0.3px;}";
  html += ".status-safe{background:#E3F0E7;color:#3D7A56;}";
  html += ".status-caution{background:#FBEBD8;color:#B9762E;}";
  html += ".status-risk{background:#F8E0DC;color:#B6482F;}";
  html += ".status-msg{font-size:12.5px;color:#5C7180;margin-top:12px;}";
  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px;}";
  html += ".tile{background:#FFFFFF;border:1px solid #E4D9C8;border-radius:18px;padding:17px;box-shadow:0 3px 10px rgba(21,41,53,0.04);}";
  html += ".tile-top{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;}";
  html += ".tile-icon{font-size:18px;}";
  html += ".tile-tag{font-size:10px;font-weight:700;padding:3px 9px;border-radius:7px;letter-spacing:0.2px;}";
  html += ".tag-good{background:#E3F0E7;color:#3D7A56;}";
  html += ".tag-fair{background:#FBEBD8;color:#B9762E;}";
  html += ".tag-poor{background:#F8E0DC;color:#B6482F;}";
  html += ".tile-value{font-size:27px;font-weight:800;letter-spacing:-0.3px;color:#152935;}";
  html += ".tile-unit{font-size:13px;color:#9AAAB2;font-weight:500;}";
  html += ".tile-label{font-size:12px;color:#698EA2;margin-top:5px;font-weight:600;}";
  html += ".trend-card{background:#FFFFFF;border:1px solid #E4D9C8;border-radius:18px;padding:19px;margin-bottom:16px;box-shadow:0 3px 10px rgba(21,41,53,0.04);}";
  html += ".trend-header{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px;}";
  html += ".trend-title{font-size:13px;font-weight:700;color:#152935;}";
  html += ".trend-range{font-size:11px;color:#9AAAB2;}";
  html += ".tile-link{text-decoration:none;color:inherit;display:block;}";
  html += ".tile-link:active .tile{opacity:0.7;}";
  html += ".footer{text-align:center;font-size:11px;color:#9AAAB2;padding-top:6px;}";
  html += ".log-card{background:#FFFFFF;border:1px solid #E4D9C8;border-radius:18px;padding:17px 19px;margin-bottom:16px;display:flex;align-items:center;justify-content:space-between;box-shadow:0 3px 10px rgba(21,41,53,0.04);}";
  html += ".log-text{font-size:12.5px;color:#5C7180;}";
  html += ".log-actions{display:flex;gap:8px;}";
  html += ".btn{font-size:12px;font-weight:700;padding:9px 15px;border-radius:11px;text-decoration:none;display:inline-block;}";
  html += ".btn-primary{background:#E4A576;color:#FFFFFF;}";
  html += ".btn-secondary{background:#F5EEE4;color:#5C7180;border:1px solid #E4D9C8;}";
  html += "@media(min-width:700px){body{max-width:900px;}.grid{grid-template-columns:repeat(4,1fr);}.gauge-card{padding:36px 40px;}}";
  html += "</style></head><body>";

  html += "<div class='header'><div class='brand'>";
  html += "<div class='brand-icon'></div>";
  html += "<div><div class='brand-name'>Parastoo's Air Quality Monitor</div><div class='brand-sub'>Living Room Monitor</div></div>";
  html += "</div><div><span class='live-dot'></span> <span style='font-size:11px;color:#698EA2;'>Live</span></div></div>";

  html += "<div class='gauge-card'><div class='gauge-label'>Mould Risk Score</div>";
  html += "<div class='gauge-wrap'><svg width='180' height='180' viewBox='0 0 180 180'>";
  html += "<circle cx='90' cy='90' r='78' fill='none' stroke='#EDE3D3' stroke-width='14'/>";
  html += "<circle cx='90' cy='90' r='78' fill='none' stroke='" + gaugeColor + "' stroke-width='14' stroke-linecap='round' stroke-dasharray='490' stroke-dashoffset='" + String(offset) + "' transform='rotate(-90 90 90)'/>";
  html += "</svg><div class='gauge-score'><div class='num'>" + String(mouldScore) + "</div><div class='denom'>out of 10</div></div></div>";
  html += "<div class='status-badge " + statusClass + "'>" + statusIcon + " " + statusText + "</div>";
  html += "<div class='status-msg'>" + statusMsg + "</div></div>";

  html += "<div class='grid'>";

  html += "<a class='tile-link' href='/trend?param=temperature'><div class='tile'><div class='tile-top'><span class='tile-icon'>&#127777;</span>" + statusTag(currentTemp, 26, 30) + "</div>";
  html += "<div class='tile-value'>" + String(currentTemp, 1) + "<span class='tile-unit'> &deg;C</span></div><div class='tile-label'>Temperature</div></div></a>";

  html += "<a class='tile-link' href='/trend?param=humidity'><div class='tile'><div class='tile-top'><span class='tile-icon'>&#128167;</span>" + statusTag(currentHumidity, calculateRHcrit(currentTemp) - 10, calculateRHcrit(currentTemp)) + "</div>";
  html += "<div class='tile-value'>" + String(currentHumidity, 0) + "<span class='tile-unit'> %</span></div><div class='tile-label'>Humidity</div></div></a>";

  html += "<a class='tile-link' href='/trend?param=co2'><div class='tile'><div class='tile-top'><span class='tile-icon'>&#127760;</span>" + statusTag(currentCO2, 800, 1200) + "</div>";
  html += "<div class='tile-value'>" + String(currentCO2) + "<span class='tile-unit'> ppm</span></div><div class='tile-label'>CO2</div></div></a>";

  html += "<a class='tile-link' href='/trend?param=pm25'><div class='tile'><div class='tile-top'><span class='tile-icon'>&#127787;</span>" + statusTag(currentPM25, 12, 35) + "</div>";
  html += "<div class='tile-value'>" + String(currentPM25) + "<span class='tile-unit'> &micro;g/m&sup3;</span></div><div class='tile-label'>PM2.5</div></div></a>";

  html += "<a class='tile-link' href='/trend?param=voc'><div class='tile'><div class='tile-top'><span class='tile-icon'>&#128168;</span>" + statusTagInverted(currentGasKOhms, 20, 50) + "</div>";
  html += "<div class='tile-value'>" + String(currentGasKOhms, 1) + "<span class='tile-unit'> k&Omega;</span></div><div class='tile-label'>VOC (raw)</div></div></a>";

  html += "</div>";

  html += "<div class='trend-card'><div class='trend-header'><span class='trend-title'>Humidity Trend</span><span class='trend-range'>Last 20 readings</span></div>";
  html += buildTrendChart();
  html += "</div>";

  html += "<div class='log-card'><div class='log-text'>Data Log<br>" + String(logEntryCount) + " readings recorded</div>";
  html += "<div class='log-actions'><a class='btn btn-primary' href='/download'>Download CSV</a>";
  html += "<a class='btn btn-secondary' href='/clearlog' onclick=\"return confirm('Clear all logged data?');\">Clear</a></div></div>";

  html += "<div class='footer'>Auto-refreshing every 5 seconds &middot; v0.9 (Live sensors + Logging + Time-based Mould Risk + Interactive Trend Charts)</div>";

  html += "<script>setTimeout(()=>location.reload(),5000);</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  Wire.begin();
  Wire.setClock(50000);
  Wire.setTimeOut(1000);

  scanI2C();

  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    fsOk = false;
  } else {
    fsOk = true;
    initLogFile();
    // Remove the leftover 12-month test results file
    if (LittleFS.exists("/test_results.csv")) LittleFS.remove("/test_results.csv");
    countExistingLogEntries();
    Serial.print("LittleFS ready. Existing log entries: ");
    Serial.println(logEntryCount);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Starting sensors...");
  display.display();

  scd4x.begin(Wire, SCD41_I2C_ADDR_62);

  int16_t scdErr = 1;
  for (int attempt = 1; attempt <= 4 && scdErr != NO_ERROR; attempt++) {
    Serial.print("SCD40 init attempt ");
    Serial.println(attempt);

    delay(30);
    scd4x.wakeUp();
    delay(1000);
    scd4x.stopPeriodicMeasurement();
    delay(1000);
    scd4x.reinit();
    delay(1000);
    scdErr = scd4x.startPeriodicMeasurement();

    if (scdErr != NO_ERROR) {
      char errorMessage[256];
      errorToString(scdErr, errorMessage, 256);
      Serial.print("SCD40 start error: ");
      Serial.print(scdErr);
      Serial.print(" (");
      Serial.print(errorMessage);
      Serial.println(")");
      delay(500);
    } else {
      Serial.println("SCD40 started successfully.");
    }
  }

  if (!bme.begin(0x76)) {
    if (!bme.begin(0x77)) {
      Serial.println("BME688 not found!");
    }
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  delay(100);

  // ===== Scan for "Michael" (without the apostrophe) instead of matching the
  // exact SSID string -- this avoids a known issue where a typographic
  // apostrophe elsewhere doesn't match the plain apostrophe hardcoded here =====
  Serial.println("\n--- Scanning for hotspot ---");
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
    Serial.print("Found hotspot: '");
    Serial.print(exactSsid);
    Serial.println("' -- connecting with exact scanned name...");
    WiFi.begin(exactSsid.c_str(), password);
  } else {
    Serial.println("Hotspot not found in scan! Falling back to hardcoded SSID.");
    WiFi.begin(ssid, password);
  }

  Serial.print("Connecting to WiFi");
  int wifiTries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTries < 40) {
    delay(500);
    Serial.print(".");
    wifiTries++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("WiFi FAILED. Status code: ");
    Serial.println(WiFi.status());
    Serial.println("(0=IDLE, 1=NO_SSID_AVAIL, 4=CONNECT_FAILED, 6=WRONG_PASSWORD, 7=DISCONNECTED)");
  } else {
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
  }

  configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.nist.gov", "time.google.com");
  Serial.print("Syncing time");
  int ntpTries = 0;
  time_t now = time(nullptr);
  while (now < 100000 && ntpTries < 40) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    ntpTries++;
  }
  if (now >= 100000) {
    timeSynced = true;
    Serial.println("\nTime synced.");
  } else {
    Serial.println("\nTime sync failed, will log uptime instead.");
  }

  server.on("/", handleRoot);
  server.on("/trend", handleTrend);
  server.on("/download", handleDownload);
  server.on("/clearlog", handleClearLog);
  server.begin();
  Serial.println("Web server started.");

  readSensors();
  updateDisplay();
}

// ===== Loop =====
void loop() {
  server.handleClient();

  if (millis() - lastSensorRead >= 5000) {
    lastSensorRead = millis();
    readSensors();
  }

  if (millis() - lastDisplayChange >= 3000) {
    lastDisplayChange = millis();
    displayPage = (displayPage + 1) % 3;
    updateDisplay();
  }
}
