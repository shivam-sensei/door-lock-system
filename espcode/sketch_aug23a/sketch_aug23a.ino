#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <SPIFFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <time.h>
#include <freertos/semphr.h>

// WiFi credentials
const char* ssid = "Laxmipg";
const char* password = "12345678";

// Firebase configuration
const String FIREBASE_HOST = "https://lab-server-5128b-default-rtdb.asia-southeast1.firebasedatabase.app";
const String FIREBASE_AUTH = "AIzaSyAoer09OK5EPK6oFtCjIs8FxfHUVRYppUA";

// Pin definitions
#define SS_PIN    5
#define RST_PIN   4
#define RELAY_PIN 27
#define IR_SENSOR_PIN 33
#define BUZZER_PIN 39
#define LED_GREEN_PIN 12
#define LED_RED_PIN 13

// RFID instance
MFRC522 rfid(SS_PIN, RST_PIN);

// Queue handles for inter-core communication
QueueHandle_t logQueue;
QueueHandle_t accessQueue;

// Mutex for shared resources
SemaphoreHandle_t spiffsMutex;
SemaphoreHandle_t relayMutex;

// Data structures
struct LogEntry {
  String uid;
  String action;      // "GRANTED", "DENIED", "IR_EXIT"
  String timestamp;
  String userName;
};

struct AccessResult {
  bool granted;
  String uid;
  String userName;
};

// Global variables
volatile bool doorUnlocked = false;
unsigned long doorUnlockTime = 0;
const unsigned long DOOR_UNLOCK_DURATION = 3000; // 3 seconds

// ==================== CORE 0 TASKS (WiFi & Logging) ====================

void wifiTask(void *parameter) {
  // Initialize WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync");
  unsigned long ntpStart = millis();
  
  while (time(nullptr) < 100000) {
    if (millis() - ntpStart > 10000) {
      Serial.println("\nNTP sync timeout! Using system time...");
      return;
    }
    Serial.print(".");
    delay(500);
  }
  
  Serial.println(" Time synced.");

  LogEntry logEntry;
  
  while(1) {
    // Handle WiFi reconnection
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      WiFi.reconnect();
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    // Process log queue
    if(xQueueReceive(logQueue, &logEntry, 100 / portTICK_PERIOD_MS) == pdTRUE) {
      uploadLogToFirebase(logEntry);
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void uploadLogToFirebase(const LogEntry& entry) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping log upload");
    return;
  }

  HTTPClient http;
  http.begin(FIREBASE_HOST + "/logs.json?auth=" + FIREBASE_AUTH);
  http.addHeader("Content-Type", "application/json");

  // Create JSON payload
  DynamicJsonDocument doc(1024);
  doc["uid"] = entry.uid;
  doc["action"] = entry.action;
  doc["time"] = getCurrentTime();
  doc["name"] = entry.userName;
  doc["deviceId"] = WiFi.macAddress();

  String jsonString;
  serializeJson(doc, jsonString);

  int httpResponseCode = http.POST(jsonString);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Log uploaded successfully: " + String(httpResponseCode));
  } else {
    Serial.println("Error uploading log: " + String(httpResponseCode));
  }
  
  http.end();
}

// ==================== CORE 1 TASKS (Real-time Operations) ====================

void securityTask(void *parameter) {
  // Initialize RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID Reader initialized");

  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);

  // Initial state
  digitalWrite(RELAY_PIN, LOW);  // Door locked
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_GREEN_PIN, LOW);

  AccessResult result;
  
  while(1) {
    // Check for RFID card
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = getCardUID();
      Serial.println("Card detected: " + uid);
      
      result = checkAccess(uid);
      
      if (result.granted) {
        grantAccess(result.uid, result.userName);
      } else {
        denyAccess(result.uid);
      }
      
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }

    // Check IR sensor for inside exit
    if (digitalRead(IR_SENSOR_PIN) == LOW) {
      Serial.println("IR sensor triggered - Inside exit");
      grantAccess("IR_SENSOR", "Inside Exit");
      delay(1000); // Prevent multiple triggers
    }

    // Handle door auto-lock
    if (doorUnlocked && (millis() - doorUnlockTime > DOOR_UNLOCK_DURATION)) {
      lockDoor();
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void systemMonitorTask(void *parameter) {
  while(1) {
    // Monitor system health
    Serial.printf("Free heap: %d bytes\n", esp_get_free_heap_size());
    Serial.printf("WiFi status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    Serial.printf("Door status: %s\n", doorUnlocked ? "UNLOCKED" : "LOCKED");
    
    // Check queue levels
    UBaseType_t logQueueCount = uxQueueMessagesWaiting(logQueue);
    if (logQueueCount > 8) {
      Serial.println("WARNING: Log queue nearly full!");
    }

    vTaskDelay(10000 / portTICK_PERIOD_MS); // Every 10 seconds
  }
}

// ==================== HELPER FUNCTIONS ====================

String getCardUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}
String getCurrentTime() {
  
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}
AccessResult checkAccess(const String& uid) {
  AccessResult result;
  result.granted = false;
  result.uid = uid;
  result.userName = "Unknown";

  // Take SPIFFS mutex
  if(xSemaphoreTake(spiffsMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
    File file = SPIFFS.open("/authorized_uids.json", "r");
    if (file) {
      DynamicJsonDocument doc(2048);
      deserializeJson(doc, file);
      
      if (doc.containsKey(uid)) {
        result.granted = true;
        result.userName = doc[uid]["name"].as<String>();
      }
      
      file.close();
    }
    xSemaphoreGive(spiffsMutex);
  }

  return result;
}

void grantAccess(const String& uid, const String& userName) {
  Serial.println("ACCESS GRANTED for: " + userName);
  
  // Take relay mutex
  if(xSemaphoreTake(relayMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
    // Unlock door
    digitalWrite(RELAY_PIN, HIGH);
    doorUnlocked = true;
    doorUnlockTime = millis();
    
    // Visual feedback
    digitalWrite(LED_GREEN_PIN, HIGH);
    digitalWrite(LED_RED_PIN, LOW);
    
    // Audio feedback
    beep(2, 100); // 2 short beeps
    
    xSemaphoreGive(relayMutex);
  }

  // Log the access
  LogEntry entry;
  entry.uid = uid;
  entry.action = uid == "IR_SENSOR" ? "IR_EXIT" : "GRANTED";
  entry.timestamp = getCurrentTime();
  entry.userName = userName;
  
  xQueueSend(logQueue, &entry, 0); // Non-blocking send
}

void denyAccess(const String& uid) {
  Serial.println("ACCESS DENIED for UID: " + uid);
  
  // Visual feedback
  for(int i = 0; i < 3; i++) {
    digitalWrite(LED_RED_PIN, LOW);
    delay(200);
    digitalWrite(LED_RED_PIN, HIGH);
    delay(200);
  }
  
  // Audio feedback
  beep(1, 500); // 1 long beep

  // Log the denied access
  LogEntry entry;
  entry.uid = uid;
  entry.action = "DENIED";
  entry.timestamp = millis();
  entry.userName = "Unauthorized";
  
  xQueueSend(logQueue, &entry, 0);
}

void lockDoor() {
  if(xSemaphoreTake(relayMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
    digitalWrite(RELAY_PIN, LOW);
    doorUnlocked = false;
    
    // Visual feedback
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, HIGH);
    
    Serial.println("Door locked automatically");
    
    xSemaphoreGive(relayMutex);
  }
}

void beep(int times, int duration) {
  for(int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    if(i < times - 1) delay(100);
  }
}

bool initializeSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");
    return false;
  }
  
  // Create default authorized users file if it doesn't exist
  if (!SPIFFS.exists("/authorized_uids.json")) {
    createDefaultUsersFile();
  }
  
  Serial.println("SPIFFS initialized successfully");
  return true;
}

void createDefaultUsersFile() {
  File file = SPIFFS.open("/authorized_uids.json", "w");
  if (file) {
    DynamicJsonDocument doc(1024);
    
    // Add some default authorized UIDs (replace with your actual card UIDs)
    doc["4E123103"]["name"] = "Admin";
    doc["4E123103"]["level"] = "admin";
    
    doc["E5F6G7H8"]["name"] = "User1";
    doc["E5F6G7H8"]["level"] = "user";
    
    serializeJson(doc, file);
    file.close();
    Serial.println("Default users file created");
  }
}

// ==================== MAIN SETUP ====================

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Door Security System Starting...");

  // Initialize SPIFFS
  if (!initializeSPIFFS()) {
    Serial.println("System halted due to SPIFFS error");
    while(1) delay(1000);
  }


  // Create mutexes
  spiffsMutex = xSemaphoreCreateMutex();
  relayMutex = xSemaphoreCreateMutex();
  
  if (spiffsMutex == NULL || relayMutex == NULL) {
    Serial.println("Failed to create mutexes!");
    while(1) delay(1000);
  }

  // Create queues
  logQueue = xQueueCreate(10, sizeof(LogEntry));
  
  if (logQueue == NULL) {
    Serial.println("Failed to create queues!");
    while(1) delay(1000);
  }

  // Create tasks pinned to specific cores
  
  // Core 0: WiFi and cloud operations
  xTaskCreatePinnedToCore(
    wifiTask,           // Task function
    "WiFiTask",         // Task name
    8192,               // Stack size
    NULL,               // Parameters
    2,                  // Priority
    NULL,               // Task handle
    0                   // Core 0
  );

  // Core 1: Real-time security operations
  xTaskCreatePinnedToCore(
    securityTask,       // Task function
    "SecurityTask",     // Task name
    8192,               // Stack size
    NULL,               // Parameters
    3,                  // Higher priority for security
    NULL,               // Task handle
    1                   // Core 1
  );

  // Core 1: System monitoring (lower priority)
  xTaskCreatePinnedToCore(
    systemMonitorTask,  // Task function
    "MonitorTask",      // Task name
    4096,               // Stack size
    NULL,               // Parameters
    1,                  // Lower priority
    NULL,               // Task handle
    1                   // Core 1
  );

  Serial.println("All tasks created successfully!");
  Serial.println("System ready - Present your RFID card or use IR sensor");
}

void loop() {
  // Empty - all work is done in tasks
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}