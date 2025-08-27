#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <FirebaseESP32.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <secrets.h>


#define RST_PIN 4
#define SS_PIN 5
#define WIFI_TIMEOUT 30000     // 30 seconds
#define NTP_TIMEOUT 20000      // 20 seconds
#define FIREBASE_TIMEOUT 5000  // 5 seconds
#define doorRelay 27
#define irSensor 33

MFRC522 mfrc522(SS_PIN, RST_PIN);

const char* jsonFilePath = "/uids.json";
const char* offlineLogPath = "/offline.json";
bool wifiConnected = false;
long int previousMillis = 0;
bool firebaseReady = false;
bool ntpSynced = false;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

TaskHandle_t TaskCore0;
TaskHandle_t TaskCore1;
TaskHandle_t unlockTaskHandle = NULL;

// ====== Queue for log transfer ======
struct LogEntry {
  String uid;
  String name;
  bool accessGranted;
};
QueueHandle_t logQueue;

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;
  pinMode(doorRelay, OUTPUT);
  pinMode(irSensor, INPUT);
  // Init SPI and MFRC522 - This always works offline
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("RFID reader initialized.");

  logQueue = xQueueCreate(15, sizeof(LogEntry));

  // Start tasks
  xTaskCreatePinnedToCore(TaskCore0code, "TaskCore0", 10000, NULL, 1, &TaskCore0, 0);
  xTaskCreatePinnedToCore(TaskCore1code, "TaskCore1", 10000, NULL, 1, &TaskCore1, 1);
  WiFi.onEvent(WiFiStationConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);


  // Init SPIFFS - This always works offline
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
  if (!SPIFFS.exists(offlineLogPath)) {
    File file = SPIFFS.open(offlineLogPath, FILE_WRITE);
    if (file) {
      file.print("[]");
      file.close();
      Serial.println("Created empty offline log file");
    }
  }
  addUID("4E 12 31 03", "Sanidhya Jain");

  // Try WiFi connection with timeout
  setupWiFi();

  if (wifiConnected) {
    setupFirebase();
    setupNTP();
  } else {
    Serial.println("Starting in OFFLINE MODE - Core functionality available");
  }

  Serial.println("System ready! RFID access control active.");
}

void unlockDoorTask(void* pvParameters) {
  Serial.println("Door Unlocked!");
  digitalWrite(doorRelay, HIGH);  // Unlock

  vTaskDelay(4000 / portTICK_PERIOD_MS);  // Non-blocking wait for 4 sec

  digitalWrite(doorRelay, LOW);  // Lock again
  Serial.println("Door Locked!");

  unlockTaskHandle = NULL;  // Mark task as free
  vTaskDelete(NULL);        // Kill this task
}
bool wifidisconnect;
void setupWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > WIFI_TIMEOUT) {
      Serial.println("\nWiFi connection timeout! Starting offline mode...");
      wifiConnected = false;
      return;
    }
    Serial.print(".");
    delay(500);
  }

  Serial.println(" Connected!");
  wifiConnected = true;
}
void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  wifiConnected = true;
  wifidisconnect = false;
}



void setupFirebase() {
  if (!wifiConnected) return;

  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DB_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase signup successful");
    firebaseReady = true;
  } else {
    Serial.printf("Firebase signup failed: %s\n", config.signer.signupError.message.c_str());
    firebaseReady = false;
    return;
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Set Firebase timeout using the actual available method
  Firebase.setReadTimeout(fbdo, FIREBASE_TIMEOUT);  // 5 seconds
}

void setupNTP() {
  if (!wifiConnected) return;

  // NTP Config (IST = UTC+5:30)
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync");
  unsigned long ntpStart = millis();

  while (time(nullptr) < 100000) {
    if (millis() - ntpStart > NTP_TIMEOUT) {
      Serial.println("\nNTP sync timeout! Using system time...");
      ntpSynced = false;
      return;
    }
    Serial.print(".");
    delay(500);
  }

  Serial.println(" Time synced.");
  ntpSynced = true;
}

#include <Arduino.h>
#include <MFRC522.h>

// Assuming you already have: irSensor pin, mfrc522 object, unlockdoor(),
// checkUIDInFile(), getUIDString(), tryOnlineLogging() implemented

void handleIRSensor() {
  if (digitalRead(irSensor) == HIGH) {
    Serial.println("IR Sensor triggered, unlocking door");
    if (unlockTaskHandle == NULL) {
      xTaskCreatePinnedToCore(
        unlockDoorTask,     // Task function
        "UnlockTask",       // Task name
        2048,               // Stack size
        NULL,               // Parameter
        1,                  // Priority
        &unlockTaskHandle,  // Task handle
        0                   // Core 0 (RFID side)
      );
    }
  }
}

void handleRFID() {
  // If no new card, skip
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
    return;

  String scannedUID = getUIDString();
  Serial.print("Scanned UID: ");
  Serial.println(scannedUID);

  String name;
  bool accessGranted = checkUIDInFile(scannedUID, name);

  if (accessGranted) {
    Serial.println("Access GRANTED: " + name);
    if (unlockTaskHandle == NULL) {
      xTaskCreatePinnedToCore(
        unlockDoorTask,     // Task function
        "UnlockTask",       // Task name
        2048,               // Stack size
        NULL,               // Parameter
        1,                  // Priority
        &unlockTaskHandle,  // Task handle
        0                   // Core 0 (RFID side)
      );
    }
  } else {
    Serial.println("Access DENIED: Unknown UID");
    name = "Unknown";
  }
  LogEntry entry = { scannedUID, name, accessGranted };
  xQueueSend(logQueue, &entry, 0);

  // Cleanup card
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void TaskCore0code(void* pvParameters) {
  pinMode(irSensor, INPUT);

  for (;;) {
    handleIRSensor();
    handleRFID();
    vTaskDelay(50 / portTICK_PERIOD_MS);  // small yield for Wi-Fi system tasks
  }
}

void TaskCore1code(void* pvParameters) {
  LogEntry entry;
  for (;;) {
    unsigned long currentMillis = millis();
    // if WiFi is down, try reconnecting
    if ((currentMillis - previousMillis >= 5000)) {
      bool wificheck;
      if (WiFi.status() == WL_CONNECTED) {
        wificheck = false;
      } else wificheck == true;
      if (wificheck) {
        Serial.println("Trying to Reconnect");
        WiFi.reconnect();
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED) {
          if (millis() - startTime > 5000) {
            Serial.println("\nWiFi connection timeout! Starting offline mode...");
            wifiConnected = false;
            wifidisconnect = true;
            return;
          }
          Serial.print(".");
          delay(500);
        }
      }
      previousMillis = currentMillis;
    }
    // Wait for a log entry
    if (xQueueReceive(logQueue, &entry, portMAX_DELAY)) {
      // Try logging online (non-blocking for Core 0)
      tryOnlineLogging(entry.uid, entry.name, entry.accessGranted);
    }
  }
}

void loop() {
}

void tryOnlineLogging(String uid, String name, bool accessGranted) {
  // Check if we can go online
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println("Offline mode - storing log locally");

    // Store offline
    LogEntry entry = { uid, name, accessGranted };
    offlineLogEntry(entry);
    return;
  }

  if (!firebaseReady) {
    Serial.println("Firebase not ready - storing log locally");

    // Store offline
    LogEntry entry = { uid, name, accessGranted };
    offlineLogEntry(entry);
    return;
  }
  if (!wifidisconnect) {
    syncOfflineLogsToFirebase();
  }


  // Try online logging
  logToFirebaseWithTimeout(uid, name);
}

void logToFirebaseWithTimeout(String uid, String name) {
  Serial.println("Attempting cloud logging...");

  sendDiscordNotification(name + " has entered with UID: " + uid + " at " + getCurrentTime());

  String path = "/logs/" + generateLogID(uid);
  FirebaseJson json;
  json.set("uid", uid);
  json.set("name", name);
  json.set("time", getCurrentTime());

  // The timeout is already set in setupFirebase() using Firebase.setReadTimeout()
  if (Firebase.setJSON(fbdo, path.c_str(), json)) {
    Serial.println("Firebase logged: " + getCurrentTime());
  } else {
    Serial.println("Firebase error (timeout/connection): " + fbdo.errorReason());
  }
}
uint64_t fnv1a64(const char* data, size_t len) {
  uint64_t hash = 1469598103934665603ULL;   // FNV offset basis
  const uint64_t prime = 1099511628211ULL;  // FNV prime

  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)data[i];
    hash *= prime;
  }

  return hash;
}
String generateLogID(const String& rfidUid) {
  // Combine UID + Time string
  String input = rfidUid + "_" + getCurrentTime();

  // Hash the combined input
  uint64_t hash = fnv1a64(input.c_str(), input.length());

  // Convert to 16-character hex string
  char buf[17];
  sprintf(buf, "%016llx", (unsigned long long)hash);

  return String(buf);
}

String getCurrentTime() {
  if (!ntpSynced) {
    // Fallback to millis-based timestamp if NTP failed
    return "System_" + String(millis());
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

void sendDiscordNotification(String message) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    return;  // Skip if offline
  }

  HTTPClient http;
  http.setTimeout(5000);  // 5 second timeout
  http.begin(DISCORD_WEBHOOK);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"content\": \"" + message + "\"}";
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    Serial.println("✓ Discord notified: " + String(httpResponseCode));
  } else {
    Serial.println("✗ Discord failed: " + http.errorToString(httpResponseCode));
  }

  http.end();
}

// Existing functions remain the same
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

bool checkUIDInFile(String uid, String& nameOut) {
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
void offlineLogEntry(LogEntry entry) {
  // Open existing file or create a new one
  File file = SPIFFS.open(offlineLogPath, "r");
  StaticJsonDocument<4096> doc;  // Bigger buffer for multiple logs

  if (file) {
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
      Serial.println("Failed to parse existing offline log, creating new one");
    }
  }

  // Ensure it's an array
  if (!doc.is<JsonArray>()) {
    doc.clear();
    doc.to<JsonArray>();
  }

  // Create a new log object
  JsonObject log = doc.createNestedObject();
  log["uid"] = entry.uid;
  log["name"] = entry.name;
  log["time"] = getCurrentTime();  // reuse your existing function

  // Write back to file
  file = SPIFFS.open(offlineLogPath, "w");
  if (!file) {
    Serial.println("Failed to open offline log file for writing!");
    return;
  }
  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write offline log!");
  } else {
    Serial.println("Offline log stored: " + entry.uid);
  }
  file.close();
}

// Function to sync offline logs to Firebase when WiFi returns
void syncOfflineLogsToFirebase() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED || !firebaseReady) {
    Serial.println("Cannot sync - WiFi or Firebase not ready");
    return;
  }

  File file = SPIFFS.open(offlineLogPath, "r");
  if (!file) {
    Serial.println("No offline log file found");
    return;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Failed to parse offline log file");
    return;
  }

  if (!doc.is<JsonArray>()) {
    Serial.println("Offline log is not an array");
    return;
  }

  JsonArray logs = doc.as<JsonArray>();
  int totalLogs = logs.size();

  if (totalLogs == 0) {
    Serial.println("No offline logs to sync");
    return;
  }

  Serial.println("Starting sync of " + String(totalLogs) + " offline logs...");
  int successCount = 0;
  int failCount = 0;

  // Process each log entry
  for (JsonVariant logVar : logs) {
    JsonObject log = logVar.as<JsonObject>();

    String uid = log["uid"].as<String>();
    String name = log["name"].as<String>();
    String time = log["time"].as<String>();

    // Create Firebase path and JSON
    String path = "/logs/" + generateLogID(uid + "_offline_" + time);
    FirebaseJson fbJson;
    fbJson.set("uid", uid);
    fbJson.set("name", name);
    fbJson.set("time", time);

    // Try to push to Firebase
    if (Firebase.setJSON(fbdo, path.c_str(), fbJson)) {
      successCount++;
      Serial.println("✓ Synced: " + name + " (" + uid + ")");

      // Send Discord notification for offline entry
      sendDiscordNotification("OFFLINE SYNC " + name + " entry synced - Original time: " + time);
    } else {
      failCount++;
      Serial.println("✗ Failed to sync: " + uid + " - " + fbdo.errorReason());
    }

    // Small delay between requests to avoid overwhelming Firebase
    delay(100);
  }

  Serial.println("Sync complete: " + String(successCount) + " success, " + String(failCount) + " failed");

  // If all logs synced successfully, clear the offline log file
  if (failCount == 0) {
    clearOfflineLogFile();
    Serial.println("All offline logs synced successfully - offline log cleared");
  } else {
    Serial.println("Some logs failed to sync - keeping offline log file");
  }
}

void clearOfflineLogFile() {
  File file = SPIFFS.open(offlineLogPath, "w");
  if (file) {
    file.print("[]");  // Empty JSON array
    file.close();
    Serial.println("Offline log file cleared");
  } else {
    Serial.println("Failed to clear offline log file");
  }
}
