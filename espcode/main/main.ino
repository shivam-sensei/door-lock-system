#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <FirebaseESP32.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define RST_PIN         4
#define SS_PIN          5

MFRC522 mfrc522(SS_PIN, RST_PIN);  // Create MFRC522 instance

const char* jsonFilePath = "/uids.json";

// Replace with your Wi-Fi
#define WIFI_SSID "A.T.O.M_Labs"
#define WIFI_PASSWORD "atom281121"
#define doorRelay 27
#define irSensor 33

// Firebase Config
#define API_KEY "AIzaSyAoer09OK5EPK6oFtCjIs8FxfHUVRYppUA"
#define DATABASE_URL "https://lab-server-5128b-default-rtdb.asia-southeast1.firebasedatabase.app"  // NO trailing slash

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// -------- Async Logging Setup --------
struct LogEntry {
  String uid;
  String name;
  String time;
};

QueueHandle_t logQueue;
TaskHandle_t logTaskHandle;

void logTaskFunction(void *param) {
  LogEntry entry;
  while (true) {
    if (xQueueReceive(logQueue, &entry, portMAX_DELAY)) {
      sendDiscordNotification(entry.name + " has entered at Time: " + entry.time);
      //entry.UID has the uid
      String path = "/logs/" + generateLogID(entry.uid,entry.time);
      FirebaseJson json;
      json.set("uid", entry.uid);
      json.set("name", entry.name);
      json.set("time", entry.time);

      if (Firebase.setJSON(fbdo, path.c_str(), json)) {
        Serial.println("Logged to Firebase: " + entry.time);
      } else {
        Serial.println("Firebase Error: " + fbdo.errorReason());
      }
    }
  }
}

uint64_t fnv1a64(const char* data, size_t len) {
    uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    const uint64_t prime = 1099511628211ULL; // FNV prime
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= prime;
    }
    return hash;
}

String generateLogID(const String& rfidUid, const String& timeStr) {
    // Combine UID + Time string
    String input = rfidUid + "_" + timeStr;

    // Hash input
    uint64_t hash = fnv1a64(input.c_str(), input.length());

    // Convert to hex string (16 chars)
    char buf[17];
    sprintf(buf, "%016llx", (unsigned long long)hash);

    return String(buf);
}

// -------------------------------------

void setup() {
  Serial.begin(115200);
  while (!Serial);
  pinMode(doorRelay, OUTPUT);
  pinMode(irSensor, INPUT);
  // Init SPI and MFRC522
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("RFID reader initialized.");

  // Init SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
    return;
  }

  // Create dummy UID file if not exists
  if (!SPIFFS.exists(jsonFilePath)) {
    File file = SPIFFS.open(jsonFilePath, FILE_WRITE);
    if (file) {
      file.print("{}");
      file.close();
      Serial.println("Created empty UID file.");
    }
  }

  // addUID("4E 12 31 03", "Sanidhya Jain");
  // addUID("A3 DB 59 FB", "Shivam Gupta");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(" Connected!");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase signup successful");
  } else {
    Serial.printf("Firebase signup failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // NTP Config (IST = UTC+5:30)
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync");
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(" Time synced.");

  // Create queue to hold logs
  logQueue = xQueueCreate(10, sizeof(LogEntry));
  if (logQueue == NULL) {
    Serial.println("Failed to create log queue");
    return;
  }

  // Start the background logging task
  xTaskCreatePinnedToCore(
    logTaskFunction,    // Task function
    "LogTask",          // Task name
    8192,               // Stack size
    NULL,               // Parameter
    1,                  // Priority
    &logTaskHandle,     // Handle
    1                   // Core 1
  );
}

void loop() {
  if(digitalRead(irSensor) == HIGH){
    unlockdoor();
  }
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
    return;

  String scannedUID = getUIDString();
  Serial.print("Scanned UID: ");
  Serial.println(scannedUID);

  String name;
  if (checkUIDInFile(scannedUID, name)) {
    Serial.println("Access accepted: " + name);
    unlockdoor();
  } else {
    name = "Unknown";
    Serial.println("Access denied");
  }

  LogEntry entry = { scannedUID, name, getCurrentTime() };
  if (xQueueSend(logQueue, &entry, 0) != pdPASS) {
    Serial.println("Log queue full, entry dropped.");
  }

  delay(1500);  // Debounce
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

String getUIDString() {
  String uidStr = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10)
      uidStr += "0";
    uidStr += String(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1) uidStr += " ";
  }
  uidStr.toUpperCase();
  return uidStr;
}
void unlockdoor(){
  digitalWrite(doorRelay, HIGH);
  Serial.println("door unlocked");
  delay(4000);
  digitalWrite(doorRelay,LOW);
}
bool checkUIDInFile(String uid, String &nameOut) {
  File file = SPIFFS.open(jsonFilePath, "r");
  if (!file) {
    Serial.println("Could not open UID file.");
    return false;
  }

  const size_t capacity = 1024;
  StaticJsonDocument<capacity> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Failed to parse JSON");
    return false;
  }

  if (doc.containsKey(uid)) {
    nameOut = doc[uid].as<String>();
    return true;
  }

  return false;
}

void addUID(String uid, String name) {
  File file = SPIFFS.open(jsonFilePath, "r");
  StaticJsonDocument<1024> doc;
  if (file) {
    deserializeJson(doc, file);
    file.close();
  }

  doc[uid] = name;

  file = SPIFFS.open(jsonFilePath, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("UID saved: " + uid);
  }
}

String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

void sendDiscordNotification(String message) {
  HTTPClient http;
  String webhookUrl = "https://discord.com/api/webhooks/1369580386606387221/tlfN0ha-OPRweMG0Art3HREDREAEHkToJC5nTvmJjzEuGaZBisp310lZycZKaViJR9Ew";

  http.begin(webhookUrl);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"content\": \"" + message + "\"}";

  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    Serial.print("Discord response code: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("Error sending to Discord: ");
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}
