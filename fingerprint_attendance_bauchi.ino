#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>   // Library Manager -> "ArduinoJson" by Benoit Blanchon (v6.x)

// ===============================
// WIFI / SERVER CONFIG
// ===============================

const char* WIFI_SSID     = "LET NETWORK";
const char* WIFI_PASSWORD = "technology123455";

const char* USERS_ENDPOINT      = "https://smart-attendance-system-21tj.onrender.com/api/users";
const char* ATTENDANCE_ENDPOINT = "https://smart-attendance-system-21tj.onrender.com/api/attendance";

unsigned long lastWifiAttempt   = 0;
const unsigned long WIFI_RETRY_INTERVAL = 30000;   // try reconnecting every 30s if down

unsigned long lastRetryAttempt  = 0;
const unsigned long RETRY_INTERVAL = 15000;         // retry pending syncs every 15s


// ===============================
// FINGERPRINT (UART2)
// ===============================
// Sensor TX -> GPIO16 (RX2)
// Sensor RX -> GPIO17 (TX2)
// If sensor is not detected, try swapping TX/RX wires.

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);


// ===============================
// LCD (I2C)
// ===============================
// SDA -> GPIO21, SCL -> GPIO22

#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);


// ===============================
// PUSH BUTTON (ENROLLMENT TRIGGER)
// ===============================

#define ENROLL_BUTTON_PIN 4   // button leg -> GPIO4, other leg -> GND

bool lastButtonReading = HIGH;
int stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;


// ===============================
// DATABASE
// ===============================

#define MAX_USERS 10

bool userEnrolled[MAX_USERS];     // true if this slot (= fingerprint ID) is in use
int attendanceCount[MAX_USERS];   // how many times each user has been validated

bool enrollmentMode = false;


// ===============================
// SYNC / PERSISTENCE STATE
// ===============================

Preferences prefs;

struct PendingEnroll {
  bool active;
  int id;
  int fingerprintId;
};
PendingEnroll pendingEnrolls[MAX_USERS];

#define ATT_QUEUE_SIZE 20
struct PendingAttendance {
  bool active;
  int id;
};
PendingAttendance pendingAttendance[ATT_QUEUE_SIZE];


// ===============================
// SETUP
// ===============================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("FINGERPRINT ATTENDANCE SYSTEM");
  Serial.println("================================");

  // Button
  pinMode(ENROLL_BUTTON_PIN, INPUT_PULLUP);

  // Initial state
  for (int i = 0; i < MAX_USERS; i++) {
    userEnrolled[i] = false;
    attendanceCount[i] = 0;
    pendingEnrolls[i].active = false;
  }
  for (int i = 0; i < ATT_QUEUE_SIZE; i++) {
    pendingAttendance[i].active = false;
  }

  // LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Attendance Sys");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  // Fingerprint sensor
  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);
  delay(500);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found!");
    finger.setSecurityLevel(2);   // 1 = most lenient, 5 = strictest. Lower if match rate is poor.
  } else {
    Serial.println("Fingerprint sensor NOT found. Check wiring.");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Finger sensor");
    lcd.setCursor(0, 1);
    lcd.print("NOT FOUND!");
    while (1) { delay(1000); }
  }

  delay(500);

  // ---- Network + server sync ----
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting");
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");

  connectWiFi();
  syncUsersFromServer();       // pull authoritative user list (survives reboot)
  loadPendingEnrollsFromNVS(); // restore any enrollments not yet confirmed by server
  retryPendingSyncs(true);     // try to flush them immediately if we're online

  Serial.println("System ready.");
  Serial.println("Press button to enroll a new user (fingerprint).");
  Serial.println("Serial command: L = list users, P = pending sync status");

  showReadyScreen();
}


// ===============================
// LOOP
// ===============================

void loop() {

  handleSerialCommands();
  handleEnrollButton();
  maintainWiFi();
  retryPendingSyncs();

  if (!enrollmentMode) {
    checkAttendanceScan();
  }

  delay(20);
}


// ===============================
// BUTTON HANDLING
// ===============================

void handleEnrollButton() {

  int reading = digitalRead(ENROLL_BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {

    if (reading != stableButtonState) {
      stableButtonState = reading;

      if (stableButtonState == LOW) {
        onEnrollButtonPressed();
      }
    }
  }

  lastButtonReading = reading;
}


void onEnrollButtonPressed() {

  if (enrollmentMode) {
    enrollmentMode = false;
    Serial.println("Enrollment cancelled.");
    showReadyScreen();
    return;
  }

  int nextID = findNextID();

  if (nextID == -1) {
    Serial.println("DATABASE FULL!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Database Full!");
    delay(5000);
    showReadyScreen();
    return;
  }

  enrollmentMode = true;
  runEnrollmentFlow(nextID);
  enrollmentMode = false;
}


// ===============================
// ENROLLMENT FLOW (FINGER ONLY)
// ===============================

void runEnrollmentFlow(int id) {

  Serial.println();
  Serial.println("================================");
  Serial.println("ENROLLMENT MODE");
  Serial.println("================================");
  Serial.print("Next User ID: ");
  Serial.println(id);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enroll ID: ");
  lcd.print(id);
  delay(1200);

  bool fingerOK = enrollFingerprint(id);

  if (!fingerOK) {
    Serial.println("Fingerprint enrollment failed. Aborting user enrollment.");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Finger enroll");
    lcd.setCursor(0, 1);
    lcd.print("FAILED. Retry.");
    delay(5000);
    showReadyScreen();
    return;
  }

  // ---- Mark user as enrolled only after the fingerprint is stored ----
  userEnrolled[id - 1] = true;
  attendanceCount[id - 1] = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("USER ENROLLED (FINGERPRINT)");
  Serial.print("USER ID: ");
  Serial.println(id);
  Serial.println("================================");

  // ---- Push to server; if it fails, persist to flash and retry in background ----
  bool synced = trySyncEnrollDirect(id, id);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enrolled!");
  lcd.setCursor(0, 1);

  if (synced) {
    Serial.println("Enrollment synced to server immediately.");
    lcd.print("Synced! ID:");
    lcd.print(id);
  } else {
    Serial.println("Server unreachable - enrollment saved locally, will sync automatically.");
    savePendingEnrollToNVS(id, id);
    pendingEnrolls[id - 1].active = true;
    pendingEnrolls[id - 1].id = id;
    pendingEnrolls[id - 1].fingerprintId = id;
    lcd.print("Saved. ID:");
    lcd.print(id);
  }

  delay(5000);
  showReadyScreen();
}


// Standard two-scan fingerprint enrollment into a given ID slot
bool enrollFingerprint(int id) {

  int p = -1;

  // --- First scan ---
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Place finger");
  lcd.setCursor(0, 1);
  lcd.print("(1st scan)");

  unsigned long start = millis();
  while ((p = finger.getImage()) != FINGERPRINT_OK) {
    if (millis() - start > 10000) {
      Serial.println("Timeout on first finger scan.");
      return false;
    }
    delay(50);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("Could not process first finger image.");
    return false;
  }

  // --- Reject a finger that is already enrolled ---
  if (finger.fingerFastSearch() == FINGERPRINT_OK) {
    Serial.print("This finger is already enrolled as ID ");
    Serial.println(finger.fingerID);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Finger already");
    lcd.setCursor(0, 1);
    lcd.print("enrolled!");
    return false;
  }

  // --- Wait for finger removal ---
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Remove finger");

  start = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (millis() - start > 5000) break;
    delay(50);
  }

  delay(500);

  // --- Second scan ---
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Place finger");
  lcd.setCursor(0, 1);
  lcd.print("(2nd scan)");

  start = millis();
  while ((p = finger.getImage()) != FINGERPRINT_OK) {
    if (millis() - start > 10000) {
      Serial.println("Timeout on second finger scan.");
      return false;
    }
    delay(50);
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("Could not process second finger image.");
    return false;
  }

  // --- Create and store model ---
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("Fingerprints did not match each other. Try again.");
    return false;
  }

  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    Serial.println("Could not store fingerprint model.");
    return false;
  }

  Serial.print("Fingerprint stored at ID: ");
  Serial.println(id);
  return true;
}


// ===============================
// NORMAL ATTENDANCE FLOW (FINGER ONLY)
// ===============================

void checkAttendanceScan() {

  // Non-blocking: only proceed if a finger is on the sensor
  if (finger.getImage() != FINGERPRINT_OK) {
    return;
  }

  if (finger.image2Tz() != FINGERPRINT_OK) {
    return;   // bad image, just try again on the next loop
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("FINGER DETECTED");

  if (finger.fingerFastSearch() != FINGERPRINT_OK) {
    Serial.println("FINGER NOT RECOGNIZED.");
    showAccessDenied("Not Recognized");
    delay(5000);
    showReadyScreen();
    return;
  }

  int user = finger.fingerID;

  if (user < 1 || user > MAX_USERS || !userEnrolled[user - 1]) {
    Serial.println("Matched a fingerprint that is not in the user list.");
    showAccessDenied("Not Enrolled");
    delay(5000);
    showReadyScreen();
    return;
  }

  attendanceCount[user - 1]++;   // optimistic local update - keeps LCD responsive offline

  Serial.print("ATTENDANCE MARKED. USER: ");
  Serial.print(user);
  Serial.print(" | LOCAL VISITS: ");
  Serial.println(attendanceCount[user - 1]);

  // Push to server; server's returned visit_count becomes the authoritative value
  bool synced = trySyncAttendanceDirect(user);

  if (synced) {
    Serial.print("Synced. SERVER VISITS: ");
    Serial.println(attendanceCount[user - 1]);
  } else {
    Serial.println("Server unreachable - attendance queued for retry.");
    queueAttendance(user);
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Attendance OK!");
  lcd.setCursor(0, 1);
  lcd.print("User ");
  lcd.print(user);
  lcd.print(" x");
  lcd.print(attendanceCount[user - 1]);

  delay(5000);
  showReadyScreen();
}


// ===============================
// SERIAL COMMANDS (OPTIONAL)
// ===============================

void handleSerialCommands() {

  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (command == "L") {
    listUsers();
  } else if (command == "P") {
    listPendingSyncs();
  } else {
    Serial.println("Unknown command. (Use push button to enroll)");
  }
}


// ===============================
// NEXT ID
// ===============================

int findNextID() {
  for (int i = 0; i < MAX_USERS; i++) {
    if (!userEnrolled[i]) {
      return i + 1;
    }
  }
  return -1;
}


// ===============================
// LIST USERS (Serial only)
// ===============================

void listUsers() {

  Serial.println();
  Serial.println("================================");
  Serial.println("STORED USERS");
  Serial.println("================================");

  int count = 0;

  for (int i = 0; i < MAX_USERS; i++) {
    Serial.print("USER ");
    Serial.print(i + 1);
    Serial.print(": ");

    if (userEnrolled[i]) {
      Serial.print("ENROLLED | Visits: ");
      Serial.println(attendanceCount[i]);
      count++;
    } else {
      Serial.println("EMPTY");
    }
  }

  Serial.println();
  Serial.print("TOTAL: ");
  Serial.print(count);
  Serial.print(" / ");
  Serial.println(MAX_USERS);
  Serial.println("================================");
}


// ===============================
// LCD SCREENS
// ===============================

void showReadyScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Place finger");
  lcd.setCursor(0, 1);
  lcd.print("System Ready");
}

void showAccessDenied(String reason) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Access Denied");
  lcd.setCursor(0, 1);
  lcd.print(reason);
}


// =====================================================================
// WIFI
// =====================================================================

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected - continuing in offline mode.");
    Serial.println("Fingerprint attendance still works; sync will resume automatically.");
  }
}

// Non-blocking background reconnect - called every loop() cycle
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL) return;

  lastWifiAttempt = millis();
  Serial.println("Attempting WiFi reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool wifiReady() {
  return WiFi.status() == WL_CONNECTED;
}


// =====================================================================
// HTTP HELPERS
// =====================================================================

bool httpPostJson(const char* url, const String& payload, String& responseOut, int& httpCode) {
  if (!wifiReady()) {
    httpCode = -1;
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000); // Render free tier can be slow to wake up

  httpCode = http.POST(payload);
  if (httpCode > 0) {
    responseOut = http.getString();
  }
  http.end();

  return (httpCode == 200 || httpCode == 201);
}

bool httpGetJson(const char* url, String& responseOut, int& httpCode) {
  if (!wifiReady()) {
    httpCode = -1;
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(20000);

  httpCode = http.GET();
  if (httpCode > 0) {
    responseOut = http.getString();
  }
  http.end();

  return (httpCode == 200);
}


// =====================================================================
// SERVER SYNC - ENROLLMENT
// =====================================================================

bool trySyncEnrollDirect(int id, int fingerprintId) {
  String payload = "{\"slot_id\":" + String(id) +
                    ",\"fingerprint_id\":" + String(fingerprintId) + "}";

  String response;
  int code;
  bool ok = httpPostJson(USERS_ENDPOINT, payload, response, code);

  Serial.print("Enroll sync -> HTTP ");
  Serial.println(code);

  return ok;
}

void savePendingEnrollToNVS(int id, int fingerprintId) {
  prefs.begin("pending", false);
  String key = "e" + String(id);
  prefs.putInt(key.c_str(), fingerprintId);
  prefs.end();
}

void clearPendingEnrollFromNVS(int id) {
  prefs.begin("pending", false);
  String key = "e" + String(id);
  prefs.remove(key.c_str());
  prefs.end();
}

// Restores any enrollments that were never confirmed by the server,
// even across a reboot - this is what guarantees no enrolled user is lost.
void loadPendingEnrollsFromNVS() {
  prefs.begin("pending", true); // read-only

  for (int id = 1; id <= MAX_USERS; id++) {
    String key = "e" + String(id);
    if (prefs.isKey(key.c_str())) {
      int fpId = prefs.getInt(key.c_str(), id);

      pendingEnrolls[id - 1].active = true;
      pendingEnrolls[id - 1].id = id;
      pendingEnrolls[id - 1].fingerprintId = fpId;

      // Make sure the device can recognize this user locally even
      // before the server confirms the sync.
      userEnrolled[id - 1] = true;

      Serial.print("Restored unsynced enrollment from flash for user ");
      Serial.println(id);
    }
  }

  prefs.end();
}


// =====================================================================
// SERVER SYNC - ATTENDANCE
// =====================================================================

bool trySyncAttendanceDirect(int id) {
  String payload = "{\"slot_id\":" + String(id) +
                    ",\"fingerprint_id\":" + String(id) +
                    ",\"source\":\"ESP32\"}";

  String response;
  int code;
  bool ok = httpPostJson(ATTENDANCE_ENDPOINT, payload, response, code);

  if (ok) {
    // Parse server's authoritative visit_count and overwrite the local value.
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err && doc["success"] == true) {
      long serverCount = doc["user"]["visit_count"] | -1;
      if (serverCount >= 0) {
        attendanceCount[id - 1] = serverCount;
      }
    }
  }

  return ok;
}

void queueAttendance(int id) {
  for (int i = 0; i < ATT_QUEUE_SIZE; i++) {
    if (!pendingAttendance[i].active) {
      pendingAttendance[i].active = true;
      pendingAttendance[i].id = id;
      return;
    }
  }
  // Queue full (very unlikely at size 20) - overwrite the oldest slot
  Serial.println("Attendance retry queue full - overwriting oldest entry.");
  pendingAttendance[0].active = true;
  pendingAttendance[0].id = id;
}


// =====================================================================
// BACKGROUND RETRY LOOP
// =====================================================================

void retryPendingSyncs(bool force) {
  if (!force && millis() - lastRetryAttempt < RETRY_INTERVAL) return;
  lastRetryAttempt = millis();

  if (!wifiReady()) return;

  // Enrollments take priority - never leave an enrolled user unsynced longer than necessary
  for (int i = 0; i < MAX_USERS; i++) {
    if (pendingEnrolls[i].active) {
      Serial.print("Retrying enrollment sync for user ");
      Serial.println(pendingEnrolls[i].id);

      if (trySyncEnrollDirect(pendingEnrolls[i].id, pendingEnrolls[i].fingerprintId)) {
        clearPendingEnrollFromNVS(pendingEnrolls[i].id);
        pendingEnrolls[i].active = false;
        Serial.println("Enrollment sync succeeded.");
      }
    }
  }

  // Then flush queued attendance events
  for (int i = 0; i < ATT_QUEUE_SIZE; i++) {
    if (pendingAttendance[i].active) {
      if (trySyncAttendanceDirect(pendingAttendance[i].id)) {
        pendingAttendance[i].active = false;
      }
    }
  }
}

void retryPendingSyncs() {
  retryPendingSyncs(false);
}


// =====================================================================
// BOOT-TIME SYNC - pull authoritative user list from the server
// =====================================================================

void syncUsersFromServer() {
  if (!wifiReady()) {
    Serial.println("No WiFi - skipping server sync, using local/pending data only.");
    return;
  }

  String response;
  int code;
  bool ok = httpGetJson(USERS_ENDPOINT, response, code);

  if (!ok) {
    Serial.print("Could not fetch user list from server, HTTP ");
    Serial.println(code);
    return;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, response);
  if (err || doc["success"] != true) {
    Serial.println("Failed to parse server user list.");
    return;
  }

  JsonArray users = doc["users"].as<JsonArray>();
  for (JsonObject u : users) {
    int slotId = u["slot_id"] | 0;
    if (slotId >= 1 && slotId <= MAX_USERS) {
      long visits = u["visit_count"] | 0;
      userEnrolled[slotId - 1] = true;
      attendanceCount[slotId - 1] = visits;
    }
  }

  Serial.println("User list synced from server.");
}


// =====================================================================
// DEBUG: list anything still waiting to sync
// =====================================================================

void listPendingSyncs() {
  Serial.println();
  Serial.println("================================");
  Serial.println("PENDING SYNCS");
  Serial.println("================================");

  bool anyEnroll = false;
  for (int i = 0; i < MAX_USERS; i++) {
    if (pendingEnrolls[i].active) {
      anyEnroll = true;
      Serial.print("Enrollment pending -> User ");
      Serial.println(pendingEnrolls[i].id);
    }
  }
  if (!anyEnroll) Serial.println("No pending enrollments.");

  bool anyAtt = false;
  for (int i = 0; i < ATT_QUEUE_SIZE; i++) {
    if (pendingAttendance[i].active) {
      anyAtt = true;
      Serial.print("Attendance pending -> User ");
      Serial.println(pendingAttendance[i].id);
    }
  }
  if (!anyAtt) Serial.println("No pending attendance events.");

  Serial.println("================================");
}


//////////////////////////