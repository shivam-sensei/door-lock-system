#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <FirebaseESP32.h>
#include <WiFi.h>
#include <time.h>

#define RST_PIN         2
#define SS_PIN          21

MFRC522 mfrc522(SS_PIN, RST_PIN);  // Create MFRC522 instance

const char* jsonFilePath = "/uids.json";

// Replace with your Wi-Fi
#define WIFI_SSID "A.T.O.M_Labs"
#define WIFI_PASSWORD "atom281121"

// Firebase Config
#define API_KEY "AIzaSyAoer09OK5EPK6oFtCjIs8FxfHUVRYppUA"
#define DATABASE_URL "https://lab-server-5128b-default-rtdb.asia-southeast1.firebasedatabase.app"  // NO trailing slash

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

void setup() {
  Serial.begin(115200);
  while (!Serial);

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
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");  // 19800 = 5.5 hrs offset in seconds

  Serial.print("Waiting for NTP time sync");
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(" Time synced.");

}

void loop() {
  // Look for new cards
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
    return;

  String scannedUID = getUIDString();
  Serial.print("Scanned UID: ");
  Serial.println(scannedUID);

  String name;
  if (checkUIDInFile(scannedUID, name)) {
    Serial.println("Access accepted: " + name);
    logToFirebase(scannedUID, name);
  } else {
    Serial.println("Access denied");
    logToFirebase(scannedUID, "Unknown");
  }

  delay(1500);  // Debounce read
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

bool checkUIDInFile(String uid, String &nameOut) {
  File file = SPIFFS.open(jsonFilePath, "r");
  if (!file) {
    Serial.println("Could not open UID file.");
    return false;
  }

  // Allocate enough buffer for JSON
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

void logToFirebase(String uid, String name) {
  String path = "/logs/" + String(millis());
  FirebaseJson json;

  json.set("uid", uid);
  json.set("name", name);
  json.set("time", getCurrentTime());

  if (Firebase.setJSON(fbdo, path.c_str(), json)) {
    Serial.println("Logged to Firebase: " + getCurrentTime());
} else {
    Serial.println("Firebase Error: " + fbdo.errorReason());
}
}

String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}
