#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "HX711.h"
#include <ESP32Servo.h>
#include <SPI.h>
#include <SD.h>

// WiFi and Firebase
#define WIFI_SSID     "PILL"
#define WIFI_PASSWORD "12345678"
const char* FIREBASE_HOST = "smart-pill-594a6-default-rtdb.asia-southeast1.firebasedatabase.app";

// Pins
#define S0_PIN   26
#define S1_PIN   25
#define S2_PIN   33
#define S3_PIN   32
#define OUT_PIN  17
#define HX711_DOUT 14
#define HX711_SCK  16
#define SERVO_PIN  4
#define SD_CS      5

// Globals
HX711 scale;
Servo lidServo;
WiFiClientSecure client;
bool sdReady = false;
File logFile;
float CAL_FACTOR = 2280.0f;
const long TZ_OFFSET_SEC = 5*3600 + 30*60;

// Structs
struct RGB { uint32_t r, g, b; };
struct PillSpec { const char* color; const char* name; int minMg; int maxMg; };

PillSpec specs[] = {
  {"ORANGE",   "Orange",   150, 280},
  {"REDWHITE", "RedWhite", 180, 320},
  {"BABYPINK", "BabyPink", 120, 240},
  {"CREAM",    "Montek",   180, 280},
  {"WHITE",    "White",    200, 350}
};

String allowedMorning[]   = {"REDWHITE","ORANGE","WHITE"};
String allowedAfternoon[] = {"REDWHITE","BABYPINK","CREAM"};
String allowedNight[]     = {"REDWHITE","WHITE"};

// Function declarations
uint32_t measurePeriod(bool s2, bool s3);
RGB readColorRaw();
int toIntensity(uint32_t us);
String classifyColor(int R, int G, int B);
float readWeightGrams();
bool weightMatches(const String& color, int mg);
String isoNowUTC();
String periodNow();
bool isAllowedNow(const String& c);
bool fbPOST(const String& pathJson, const String& json);
void printDivider(int length=50);

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(S0_PIN, OUTPUT); pinMode(S1_PIN, OUTPUT);
  pinMode(S2_PIN, OUTPUT); pinMode(S3_PIN, OUTPUT);
  pinMode(OUT_PIN, INPUT);
  digitalWrite(S0_PIN, HIGH);
  digitalWrite(S1_PIN, LOW);

  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale(CAL_FACTOR);
  scale.tare();
  Serial.println("HX711 ready");

  lidServo.attach(SERVO_PIN, 500, 2400);
  lidServo.write(0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.printf("\n✅ WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  client.setInsecure();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  while (time(nullptr) < 1700000000) delay(250);
  Serial.print("Time sync done, current period: ");
  Serial.println(periodNow());

  sdReady = SD.begin(SD_CS);
  if (sdReady) {
    logFile = SD.open("/pills.txt", FILE_APPEND);
    if (logFile) {
      logFile.println("==== New Session ====");
      logFile.close();
    }
    Serial.println("SD Card ready");
  } else Serial.println("No SD Card found or failed to init");
}

void loop() {
  RGB raw = readColorRaw();
  int Ri = toIntensity(raw.r);
  int Gi = toIntensity(raw.g);
  int Bi = toIntensity(raw.b);
  String color = classifyColor(Ri, Gi, Bi);

  float grams = readWeightGrams();
  int mg = int(grams * 1000.0f + 0.5f);

  String per = periodNow();
  bool allowedColor = isAllowedNow(color);
  bool weightOK = weightMatches(color, mg);

  // Always correct if color is REDWHITE
  String status;
  if (color == "REDWHITE") status = "CORRECT";
  else if (color == "UNKNOWN") status = "UNKNOWN";
  else if (!allowedColor || !weightOK) status = "WRONG";
  else status = "CORRECT";

  if (status == "CORRECT") lidServo.write(90);
  else lidServo.write(0);

  // Pretty serial output
  printDivider();
  Serial.println("Logging pill reading:");
  printDivider();
  Serial.print("Raw RGB: ");
  Serial.print(raw.r); Serial.print(", ");
  Serial.print(raw.g); Serial.print(", ");
  Serial.println(raw.b);
  Serial.println("Intensity R: " + String(Ri) + " G: " + String(Gi) + " B: " + String(Bi));
  Serial.println("Time Period: " + per);
  Serial.println("Detected Color: " + color);
  Serial.println("Weight: " + String(mg) + " mg");
  Serial.println("Status: " + status);
  printDivider();
  Serial.println();

  // SD logging
  if (sdReady) {
    logFile = SD.open("/pills.txt", FILE_APPEND);
    if (logFile) {
      logFile.print(raw.r); logFile.print(",");
      logFile.print(raw.g); logFile.print(",");
      logFile.print(raw.b); logFile.print(",");
      logFile.print(Ri); logFile.print(",");
      logFile.print(Gi); logFile.print(",");
      logFile.print(Bi); logFile.print(",");
      logFile.print(mg); logFile.print(",");
      logFile.print(per); logFile.print(",");
      logFile.print(color); logFile.print(",");
      logFile.println(status);
      logFile.close();
    }
  }

  // Firebase log
  String json = "{";
  json += "\"timestamp\":\"" + isoNowUTC() + "\",";
  json += "\"period\":\"" + per + "\",";
  json += "\"R\":" + String(Ri) + ",";
  json += "\"G\":" + String(Gi) + ",";
  json += "\"B\":" + String(Bi) + ",";
  json += "\"weight_mg\":" + String(mg) + ",";
  json += "\"detectedColor\":\"" + color + "\",";
  json += "\"status\":\"" + status + "\"}";
  fbPOST("/SmartPillNew/Logs.json", json);

  delay(1300);
}

// Helper functions implementation
uint32_t measurePeriod(bool s2, bool s3) {
  digitalWrite(S2_PIN, s2); digitalWrite(S3_PIN, s3); delayMicroseconds(200);
  unsigned long t1 = pulseIn(OUT_PIN, LOW, 30000); unsigned long t2 = pulseIn(OUT_PIN, HIGH, 30000);
  if (t1 == 0 || t2 == 0) return 0; return (uint32_t)(t1 + t2);
}
RGB readColorRaw() {
  RGB x{}; x.r = measurePeriod(LOW, LOW); x.g = measurePeriod(HIGH, HIGH); x.b = measurePeriod(LOW, HIGH); return x;
}
int toIntensity(uint32_t us) {
  if (us == 0) return 0;
  const uint32_t MIN_US = 20, MAX_US = 800;
  if (us < MIN_US) us = MIN_US; if (us > MAX_US) us = MAX_US;
  float t = float(MAX_US - us) / float(MAX_US - MIN_US); int v = int(t * 1000.0f + 0.5f); return constrain(v, 0, 1000);
}
String classifyColor(int R, int G, int B) {
  if (R > 700 && (R - G) > 100 && (R - B) > 100)   return "RED";
  if (R > 700 && G > 450 && B < 400 && (R - B) > 200) return "ORANGE";
  if (R > 700 && B > 500 && G < 600 && (R - B) > 80)  return "BABYPINK";
  if (R > 700 && G > 700 && B > 500)                  return "REDWHITE";
  if (R > 650 && G > 650 && B < 600)                  return "CREAM";
  if (R > 800 && G > 800 && B > 800 &&
      abs(R - G) < 80 && abs(R - B) < 80 && abs(G - B) < 80) return "WHITE";
  return "UNKNOWN";
}
float readWeightGrams() { return scale.get_units(5); }
bool weightMatches(const String& color, int mg) {
  for (auto& s : specs) {
    if (color == s.color) return (mg >= s.minMg && mg <= s.maxMg);
  }
  return false;
}
String isoNowUTC() {
  time_t now = time(nullptr); struct tm tm; gmtime_r(&now, &tm); char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm); return String(buf);
}
String periodNow() {
  time_t now = time(nullptr) + TZ_OFFSET_SEC; struct tm tm; gmtime_r(&now, &tm); int h = tm.tm_hour;
  if (h >= 5 && h < 12) return "MORNING"; if (h >= 12 && h < 18) return "AFTERNOON"; return "NIGHT";
}
bool isAllowedNow(const String& c) {
  if (c == "REDWHITE") return true;
  String p = periodNow();
  if (p == "MORNING") for (auto& s : allowedMorning) if (c == s) return true;
  else if (p == "AFTERNOON") for (auto& s : allowedAfternoon) if (c == s) return true;
  else for (auto& s : allowedNight) if (c == s) return true;
  return false;
}
bool fbPOST(const String& pathJson, const String& json) {
  if (!client.connect(FIREBASE_HOST, 443)) return false;
  String req = "POST " + pathJson + " HTTP/1.1\r\n";
  req += "Host: " + String(FIREBASE_HOST) + "\r\n";
  req += "User-Agent: ESP32\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Connection: close\r\n";
  req += "Content-Length: " + String(json.length()) + "\r\n\r\n";
  req += json;
  client.print(req);
  String status = client.readStringUntil('\n');
  client.stop();
  return status.indexOf("200") > 0;
}
void printDivider(int length) {
  for (int i = 0; i < length; i++) Serial.print(".");
  Serial.println();
}
