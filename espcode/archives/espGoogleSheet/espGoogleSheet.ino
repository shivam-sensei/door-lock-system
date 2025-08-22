// RFID Access System with OLED, SPIFFS, Web Dashboard, and Cloud Logging (Google Sheets)

#include <WiFi.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// === Constants and Config ===
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* googleScriptURL = "https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec";

#define RST_PIN 22
#define SS_PIN  21
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET -1
#define LED_PIN 2

MFRC522 mfrc522(SS_PIN, RST_PIN);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);

bool wifiAvailable = false;

// === Setup ===
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  SPI.begin();
  mfrc522.PCD_Init();

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
  }

  connectToWiFi();
  setupWebServer();
}

// === Wi-Fi Connect ===
void connectToWiFi() {
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 10) {
    delay(1000);
    Serial.print(".");
    retries++;
  }
  wifiAvailable = (WiFi.status() == WL_CONNECTED);
  Serial.println(wifiAvailable ? "\nConnected to WiFi" : "\nWiFi Not Available");
}

// === Web Server ===
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", "<h2>ESP32 Dashboard</h2><button onclick=\"location.href='/unlock'\">Unlock</button><br><button onclick=\"location.href='/records'\">Show Records</button>");
  });
  server.on("/unlock", HTTP_GET, []() {
    digitalWrite(LED_PIN, HIGH);
    delay(2000);
    digitalWrite(LED_PIN, LOW);
    server.send(200, "text/plain", "Gate Unlocked");
  });
  server.on("/records", HTTP_GET, []() {
    File file = SPIFFS.open("/history.txt", "r");
    String content;
    while (file.available()) content += file.readStringUntil('\n') + "<br>";
    file.close();
    server.send(200, "text/html", content);
  });
  server.begin();
}

// === RFID Loop ===
void loop() {
  server.handleClient();

  // Handle RFID
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    String name = getNameFromSPIFFS(uid);

    display.clearDisplay();
    display.setCursor(0, 10);
    if (name != "") {
      display.print("Access: "); display.println(name);
      display.display();
      digitalWrite(LED_PIN, HIGH);
      delay(2000);
      digitalWrite(LED_PIN, LOW);
      logRecord(name);
    } else {
      display.print("Access Denied\nUID: ");
      display.println(uid);
      display.display();
    }

    mfrc522.PICC_HaltA();
  }

  // Handle Wi-Fi reconnection and pushing offline logs
  if (!wifiAvailable && WiFi.status() == WL_CONNECTED) {
    wifiAvailable = true;
    Serial.println("Wi-Fi Reconnected. Uploading offline logs...");
    pushOfflineLogs();
  }
}

// === UID Matching ===
String getNameFromSPIFFS(String uid) {
  File file = SPIFFS.open("/uids.json", "r");
  if (!file) return "";
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, file);
  file.close();
  if (doc.containsKey(uid)) return doc[uid].as<String>();
  return "";
}

// === Logging ===
void logRecord(String name) {
  String entry = name + " - " + String(millis()/1000) + "s";

  File logFile = SPIFFS.open("/history.txt", FILE_APPEND);
  logFile.println(entry);
  logFile.close();

  if (wifiAvailable) {
    HTTPClient http;
    http.begin(String(googleScriptURL) + "?entry=" + entry);
    http.GET();
    http.end();
  } else {
    File offline = SPIFFS.open("/offline.txt", FILE_APPEND);
    offline.println(entry);
    offline.close();
    trimLogFile("/offline.txt", 10);
  }
}

// === Trim Offline Log ===
void trimLogFile(const char* path, int maxLines) {
  File file = SPIFFS.open(path, "r");
  String lines[20]; int count = 0;
  while (file.available() && count < 20) {
    lines[count++] = file.readStringUntil('\n');
  }
  file.close();

  file = SPIFFS.open(path, "w");
  for (int i = max(0, count - maxLines); i < count; i++) {
    file.println(lines[i]);
  }
  file.close();
}

// === Push Offline Logs to Cloud ===
void pushOfflineLogs() {
  File offline = SPIFFS.open("/offline.txt", "r");
  if (!offline || !offline.size()) return;

  while (offline.available()) {
    String entry = offline.readStringUntil('\n');
    if (entry.length() > 0) {
      HTTPClient http;
      http.begin(String(googleScriptURL) + "?entry=" + entry);
      int code = http.GET();
      http.end();
      delay(500);
    }
  }
  offline.close();
  SPIFFS.remove("/offline.txt");
}
