#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <time.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#define BLACK SH110X_BLACK
#define WHITE SH110X_WHITE

#define CLOUD_RELAY_HOST "deskbuddy-relay.onrender.com"
#define CLOUD_RELAY_PORT 443

// Hardware Pins (realigned for ESP32-C3 SuperMini)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define TOUCH_PIN 2
#define BUTTON_PIN 3
#define ONBOARD_BOOT_PIN 9
#define VIBRATION_PIN 10

// Onboard status LED (GPIO 8 is standard on C3 SuperMini)
#ifdef LED_BUILTIN
const int ledPin = LED_BUILTIN;
#else
const int ledPin = 8;
#endif

// NTP configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // India UTC+5:30
const int daylightOffset_sec = 0;

// Display Instance
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Preferences for persistent flash storage
Preferences preferences;

// WebSockets Client
WebSocketsClient webSocket;

// Pairing and API keys
String deviceId = "";

// PC Stats State
int pcCpuUsage = -1;
int pcCpuTemp = -1;
int pcRamUsage = -1;
int pcGpuUsage = -1;
int pcGpuTemp = -1;
unsigned long lastPcStatsTime = 0;

enum DisplayMode {
  MODE_BUDDY,
  MODE_PC_STATS
};
DisplayMode currentMode = MODE_BUDDY;
bool manualStatsMode = false;

// Auto-cycling timer state
unsigned long lastModeCycleTime = 0;
const unsigned long CYCLE_BUDDY_MS = 15000; // 15 seconds face
const unsigned long CYCLE_STATS_MS = 10000; // 10 seconds stats

// Global State Variables
bool sleeping = false;
unsigned long touchStart = 0;
int pupilX = 0;
int pupilY = 0;
unsigned long lastBlink = 0;
unsigned long lastMove = 0;
int zY = 45;
bool lastTouched = false;
int tapCount = 0;
unsigned long lastTapTime = 0;
const unsigned long TAP_TIMEOUT = 250; // ms to wait for double tap

// POMODORO State
bool pomodoroRunning = false;
bool breakScreen = false;
unsigned long pomodoroStart = 0;
unsigned long breakStart = 0;
const unsigned long POMODORO_TIME = 25UL * 60UL * 1000UL; // 25 mins
bool lastButtonState = HIGH;
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool longPressTriggered = false;

// Vibration State & Controller
unsigned long vibrationEnd = 0;
void triggerVibration(int durationMs) {
  digitalWrite(VIBRATION_PIN, HIGH);
  vibrationEnd = millis() + durationMs;
}

// Function Declarations
String getTimeString();
void drawTopInfo();
void drawWiFiStatus();
void drawTimer();
void drawEyes(int px, int py);
void drawClosedEyes();
void drawHappyEyes();
void drawWinkEyes();
void drawHeartEyes();
void drawSadEyes();
void drawAngryEyes();
void drawDizzyEyes();
void blinkEyes();
void sleepAnimation();
void wakeAnimation();
void sleepingScreen();
void showBreakTime();
void recoverI2C();
void drawPCStats();
void setupDeviceId();
void drawFullscreenPomodoro();

// --- Display and Timer Routines ---

void drawTimer() {
  if (!pomodoroRunning) return;

  unsigned long elapsed = millis() - pomodoroStart;

  if (elapsed >= POMODORO_TIME) {
    pomodoroRunning = false;
    breakScreen = true;
    breakStart = millis();
    return;
  }

  unsigned long remaining = (POMODORO_TIME - elapsed) / 1000;
  int mins = remaining / 60;
  int secs = remaining % 60;

  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);

  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.print(secs);
}

void showBreakTime() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(8, 15);
  display.print("BREAK");
  display.setCursor(18, 40);
  display.print("TIME!");
  display.display();
}

void drawFullscreenPomodoro() {
  display.clearDisplay();
  
  // Self-healing display check
  static unsigned long lastOledCheck = 0;
  if (millis() - lastOledCheck > 1000) {
    lastOledCheck = millis();
    Wire.beginTransmission(OLED_ADDR);
    if (Wire.endTransmission() != 0) {
      recoverI2C();
      Wire.begin(0, 1);
      Wire.setTimeOut(100);
      display.begin(OLED_ADDR, true);
      display.oled_command(SH110X_DISPLAYON);
    }
  }

  unsigned long elapsed = millis() - pomodoroStart;
  if (elapsed >= POMODORO_TIME) {
    pomodoroRunning = false;
    breakScreen = true;
    breakStart = millis();
    showBreakTime();
    return;
  }

  unsigned long remaining = (POMODORO_TIME - elapsed) / 1000;
  int mins = remaining / 60;
  int secs = remaining % 60;

  // Title: FOCUS
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(49, 4); // "FOCUS" is 5 chars * 6px = 30px. (128 - 30)/2 = 49.
  display.print("FOCUS");

  // Counter: MM:SS (Size 3)
  display.setTextSize(3);
  display.setCursor(19, 20); // 5 chars * 18px = 90px. (128 - 90)/2 = 19.
  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.print(secs);

  // Progress Bar
  display.drawRoundRect(14, 52, 100, 6, 3, WHITE);
  float progress = (float)elapsed / (float)POMODORO_TIME;
  int fillWidth = (int)(progress * 96.0);
  if (fillWidth > 0) {
    display.fillRoundRect(16, 54, fillWidth, 2, 1, WHITE);
  }

  // Draw WiFi status in top right corner (shifted down)
  drawWiFiStatus();

  display.display();
}

void drawEyes(int px, int py) {
  display.clearDisplay();
  drawTopInfo();

  display.fillRoundRect(20, 36, 30, 24, 10, WHITE);
  display.fillRoundRect(78, 36, 30, 24, 10, WHITE);

  display.fillCircle(35 + px, 48 + py, 5, BLACK);
  display.fillCircle(93 + px, 48 + py, 5, BLACK);

  display.display();
}

void drawClosedEyes() {
  display.clearDisplay();
  drawTopInfo();

  display.drawLine(20, 42, 50, 42, WHITE);
  display.drawLine(78, 42, 108, 42, WHITE);

  display.display();
}

void drawHappyEyes() {
  display.clearDisplay();
  drawTopInfo();

  display.drawLine(20, 42, 35, 32, WHITE);
  display.drawLine(35, 32, 50, 42, WHITE);

  display.drawLine(78, 42, 93, 32, WHITE);
  display.drawLine(93, 32, 108, 42, WHITE);

  display.display();
}

void drawWinkEyes() {
  display.clearDisplay();
  drawTopInfo();

  // Left eye normal
  display.fillRoundRect(20, 36, 30, 24, 10, WHITE);
  display.fillCircle(35 + pupilX, 48 + pupilY, 5, BLACK);

  // Right eye winking/closed
  display.drawLine(78, 48, 108, 48, WHITE);

  display.display();
}

void drawHeartEyes() {
  display.clearDisplay();
  drawTopInfo();

  // Left Eye Heart
  display.fillCircle(20 + 8, 36 + 8, 8, WHITE);
  display.fillCircle(20 + 22, 36 + 8, 8, WHITE);
  display.fillTriangle(20, 44, 50, 44, 35, 60, WHITE);

  // Right Eye Heart
  display.fillCircle(78 + 8, 36 + 8, 8, WHITE);
  display.fillCircle(78 + 22, 36 + 8, 8, WHITE);
  display.fillTriangle(78, 44, 108, 44, 93, 60, WHITE);

  display.display();
}

void drawSadEyes() {
  display.clearDisplay();
  drawTopInfo();

  // Base white rounded eyes
  display.fillRoundRect(20, 36, 30, 24, 10, WHITE);
  display.fillRoundRect(78, 36, 30, 24, 10, WHITE);

  // Pupils shifted down a bit
  display.fillCircle(35, 51, 5, BLACK);
  display.fillCircle(93, 51, 5, BLACK);

  // Draw slant covers to make eyes look sad
  display.fillTriangle(20, 36, 38, 36, 20, 46, BLACK);
  display.fillTriangle(108, 36, 90, 36, 108, 46, BLACK);

  display.display();
}

void drawAngryEyes() {
  display.clearDisplay();
  drawTopInfo();

  // Base white rounded eyes
  display.fillRoundRect(20, 36, 30, 24, 10, WHITE);
  display.fillRoundRect(78, 36, 30, 24, 10, WHITE);

  // Pupils shifted towards the middle
  display.fillCircle(38, 48, 5, BLACK);
  display.fillCircle(90, 48, 5, BLACK);

  // Draw slant covers to make eyes look angry
  display.fillTriangle(50, 36, 32, 36, 50, 46, BLACK);
  display.fillTriangle(78, 36, 96, 36, 78, 46, BLACK);

  display.display();
}

void drawDizzyEyes() {
  display.clearDisplay();
  drawTopInfo();

  // Left Eye X
  display.drawLine(22, 38, 48, 58, WHITE);
  display.drawLine(48, 38, 22, 58, WHITE);

  // Right Eye X
  display.drawLine(80, 38, 106, 58, WHITE);
  display.drawLine(106, 38, 80, 58, WHITE);

  display.display();
}

void blinkEyes() {
  drawClosedEyes();
  delay(150);
  drawEyes(pupilX, pupilY);
}

void sleepAnimation() {
  for (int h = 24; h >= 2; h -= 2) {
    display.clearDisplay();
    display.fillRoundRect(20, 48 - h / 2, 30, h, 8, WHITE);
    display.fillRoundRect(78, 48 - h / 2, 30, h, 8, WHITE);
    display.display();
    delay(80);
  }
  drawClosedEyes();
}

void wakeAnimation() {
  for (int h = 2; h <= 24; h += 2) {
    display.clearDisplay();
    drawTopInfo();
    display.fillRoundRect(20, 48 - h / 2, 30, h, 8, WHITE);
    display.fillRoundRect(78, 48 - h / 2, 30, h, 8, WHITE);
    display.display();
    delay(80);
  }
  drawEyes(0, 0);
}

void sleepingScreen() {
  display.clearDisplay();
  drawTopInfo();

  display.drawLine(20, 42, 50, 42, WHITE);
  display.drawLine(78, 42, 108, 42, WHITE);

  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(100, zY);
  display.print("Z");

  display.setTextSize(1);
  display.setCursor(114, zY - 10);
  display.print("z");

  display.setCursor(121, zY - 18);
  display.print(".");

  display.display();

  zY--;
  if (zY < 5) zY = 45;
}

void recoverI2C() {
  Serial.println("[I2C] Running bus recovery...");
  Wire.end(); // Detach I2C peripheral from pins to allow manual GPIO control
  pinMode(0, OUTPUT); // SDA
  pinMode(1, OUTPUT); // SCL
  
  // Force SCL high initially
  digitalWrite(1, HIGH);
  delayMicroseconds(5);
  
  // Clock 9 times to free SDA
  for (int i = 0; i < 9; i++) {
    digitalWrite(1, LOW);
    delayMicroseconds(5);
    digitalWrite(1, HIGH);
    delayMicroseconds(5);
  }
  
  // Generate STOP condition
  digitalWrite(0, LOW);
  delayMicroseconds(5);
  digitalWrite(1, HIGH);
  delayMicroseconds(5);
  digitalWrite(0, HIGH);
  delayMicroseconds(5);
  
  Serial.println("[I2C] Bus recovery complete.");
}

void onWiFiEvent(WiFiEvent_t event) {
  // Set low TX power for ALL WiFi events to prevent the driver from resetting it to max during connection/scanning
  esp_wifi_set_max_tx_power(WIFI_POWER_8_5dBm);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  // Force low TX power during AP mode to prevent brownouts
  esp_wifi_set_max_tx_power(WIFI_POWER_8_5dBm);
  
  // Re-initialize display for setup mode to recover from any transients
  Serial.println("[Setup] Re-initializing display for config portal AP mode...");
  recoverI2C();
  Wire.begin(0, 1);
  display.begin(OLED_ADDR, true);
  display.oled_command(SH110X_DISPLAYON);
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("  Wi-Fi Setup Mode");
  display.println("---------------------");
  display.println("1. Connect to Wi-Fi:");
  display.println("   DeskMate_Setup");
  display.println("2. Open browser to:");
  display.println("   192.168.4.1");
  display.println("3. Configure new AP");
  display.display();
}

void connectWiFi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 20);
  display.println("Connecting to Wi-Fi...");
  display.display();
  delay(100); // Give screen time to draw

  // Turn off display charge pump to protect from WiFi power transients
  Serial.println("[WiFi] Turning display off during connection...");
  display.oled_command(SH110X_DISPLAYOFF);
  delay(50);

  Serial.println("[WiFi] Registering Event Listener...");
  WiFi.onEvent(onWiFiEvent);

  Serial.println("[WiFi] Setting mode STA...");
  WiFi.mode(WIFI_STA);
  Serial.println("[WiFi] Setting max TX power...");
  esp_wifi_set_max_tx_power(WIFI_POWER_8_5dBm); // Prevent brownout/crash on ESP32-C3 SuperMini during WiFi operations
  
  Serial.println("[WiFi] Creating WiFiManager instance...");
  WiFiManager wm;
  Serial.println("[WiFi] Registering AP Callback...");
  wm.setAPCallback(configModeCallback);
  
  // Set timeouts to prevent getting stuck in portals forever
  wm.setConfigPortalTimeout(180); // 3 minutes timeout to configure
  wm.setConnectTimeout(30);       // 30 seconds connection timeout
  
  Serial.println("[WiFi] Calling autoConnect...");
  if (wm.autoConnect("DeskMate_Setup")) {
    Serial.println("[WiFi] autoConnect returned true, syncing time...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("[WiFi] Time sync initiated.");
  } else {
    Serial.println("[WiFi] autoConnect returned false.");
    // If it timed out, restart ESP to try again
    ESP.restart();
  }
  Serial.println("[WiFi] connectWiFi completed.");
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--";

  char buffer[6];
  strftime(buffer, sizeof(buffer), "%I:%M", &timeinfo);
  return String(buffer);
}

void drawTopInfo() {
  // Self-healing display check: Ping the OLED once per second.
  // If it doesn't respond (e.g. because of a transient power crash), recover and re-initialize it.
  static unsigned long lastOledCheck = 0;
  if (millis() - lastOledCheck > 1000) {
    lastOledCheck = millis();
    Wire.beginTransmission(OLED_ADDR);
    if (Wire.endTransmission() != 0) {
      Serial.println("[Display] Crash/lockup detected! Attempting recovery...");
      recoverI2C();
      Wire.begin(0, 1);
      Wire.setTimeOut(100);
      display.begin(OLED_ADDR, true);
      display.oled_command(SH110X_DISPLAYON);
    }
  }

  if (!pomodoroRunning) {
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(0, 12);
    display.print(getTimeString());
  }
  drawWiFiStatus();
}

void drawWiFiStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    display.setTextSize(1);
    display.setCursor(112, 12);
    display.print("X");
    return;
  }

  int rssi = WiFi.RSSI();
  int bars = 1;
  if (rssi > -55) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else bars = 1;

  int x = 112;
  for (int i = 0; i < bars; i++) {
    display.fillRect(x + i * 4, 20 - (i * 2), 3, 2 + (i * 2), WHITE);
  }
}

void setupDeviceId() {
  preferences.begin("deskbuddy", false);
  deviceId = preferences.getString("deviceId", "");
  if (deviceId.isEmpty()) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[12];
    sprintf(buf, "db-%02X%02X", mac[4], mac[5]);
    deviceId = String(buf);
    preferences.putString("deviceId", deviceId);
  }
  preferences.end();
  Serial.printf("[DeviceID] Active Device ID: %s\n", deviceId.c_str());
}

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WebSocket] Disconnected");
      break;
    case WStype_CONNECTED: {
      Serial.println("[WebSocket] Connected to relay server");
      // Register with relay server
      JsonDocument regDoc;
      regDoc["event"] = "register_esp32";
      String regStr;
      serializeJson(regDoc, regStr);
      webSocket.sendTXT(regStr);
      break;
    }
    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.println("[WebSocket] JSON Deserialization failed");
        return;
      }
      
      String event = doc["event"];
      if (event == "get_status") {
        int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
        JsonDocument resDoc;
        resDoc["event"] = "status";
        resDoc["time"] = getTimeString();
        resDoc["sleeping"] = sleeping;
        resDoc["pomodoroRunning"] = pomodoroRunning;
        resDoc["wifiRSSI"] = rssi;
        
        String resStr;
        serializeJson(resDoc, resStr);
        webSocket.sendTXT(resStr);
      }
      else if (event == "action") {
        String actionType = doc["type"];
        if (actionType == "mode_buddy") {
          currentMode = MODE_BUDDY;
          manualStatsMode = false;
          lastModeCycleTime = millis();
          triggerVibration(100);
          drawEyes(pupilX, pupilY);
        }
        else if (actionType == "mode_stats") {
          currentMode = MODE_PC_STATS;
          manualStatsMode = true;
          lastModeCycleTime = millis();
          triggerVibration(100);
          drawPCStats();
        }
        else if (actionType == "wink") {
          triggerVibration(100);
          drawWinkEyes();
          delay(1500);
          if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        }
        else if (actionType == "happy") {
          triggerVibration(100);
          drawHappyEyes();
          delay(1500);
          if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        }
        else if (actionType == "blink") {
          triggerVibration(100);
          blinkEyes();
        }
        else if (actionType == "heart") {
          triggerVibration(100);
          drawHeartEyes();
          delay(1500);
          if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        }
        else if (actionType == "sad") {
          triggerVibration(100);
          drawSadEyes();
          delay(1500);
          if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        }
        else if (actionType == "angry") {
          triggerVibration(100);
          drawAngryEyes();
          delay(1500);
          if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        }
        else if (actionType == "dizzy") {
          triggerVibration(100);
          drawDizzyEyes();
          delay(1500);
          if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        }
        else if (actionType == "sleep") {
          if (!sleeping) {
            sleepAnimation();
            sleeping = true;
          }
        }
        else if (actionType == "wake") {
          if (sleeping) {
            sleeping = false;
            wakeAnimation();
          }
        }
        else if (actionType == "toggle_pomodoro" || actionType == "start_pomodoro") {
          if (!pomodoroRunning) {
            pomodoroRunning = true;
            pomodoroStart = millis();
            drawFullscreenPomodoro();
            Serial.println("Pomodoro Started");
          } else {
            pomodoroRunning = false;
            if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
            Serial.println("Pomodoro Stopped");
          }
          triggerVibration(100);
        }
        else if (actionType == "vibrate") {
          triggerVibration(400);
        }
      }
      else if (event == "pc_stats") {
        // Parse PC Stats!
        if (doc.containsKey("cpu")) pcCpuUsage = doc["cpu"];
        if (doc.containsKey("cpuTemp")) pcCpuTemp = doc["cpuTemp"];
        if (doc.containsKey("ram")) pcRamUsage = doc["ram"];
        if (doc.containsKey("gpu")) pcGpuUsage = doc["gpu"];
        if (doc.containsKey("gpuTemp")) pcGpuTemp = doc["gpuTemp"];
        lastPcStatsTime = millis();
        manualStatsMode = false; // PC agent is online, resume auto-cycling!
        
        // If we are currently in Stats Mode, redraw immediately to look super responsive
        if (currentMode == MODE_PC_STATS) {
          drawPCStats();
        }
      }
      break;
    }
    case WStype_BIN:
      break;
    default:
      break;
  }
}

void drawPCStats() {
  display.clearDisplay();

  // Top Bar: Time and Wi-Fi RSSI
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print(getTimeString());

  // Draw WiFi status on the right side of the top bar
  drawWiFiStatus();

  // Draw divider line under the top bar
  display.drawLine(0, 10, 128, 10, WHITE);

  // Check if stats are active (updated within the last 15 seconds)
  bool statsActive = (millis() - lastPcStatsTime < 15000);

  if (!statsActive) {
    display.setCursor(0, 10);
    display.println("   PC Stats Agent");
    display.println("    is Offline.");
    display.println("");
    display.println(" Run pc_agent.js or");
    display.println("  double-click .bat");
    display.println("  file on your PC!");
    display.display();
    return;
  }

  // --- CPU Section ---
  display.setCursor(0, 14);
  display.print("CPU: ");
  if (pcCpuUsage >= 0) {
    display.print(pcCpuUsage);
    display.print("%");
  } else {
    display.print("--");
  }
  
  if (pcCpuTemp >= 0) {
    display.setCursor(85, 14);
    display.print(pcCpuTemp);
    display.print("C");
  }
  
  // Progress Bar for CPU
  display.drawRect(0, 23, 128, 5, WHITE);
  if (pcCpuUsage > 0) {
    int fillW = map(pcCpuUsage, 0, 100, 0, 126);
    display.fillRect(1, 24, fillW, 3, WHITE);
  }

  // --- RAM Section ---
  display.setCursor(0, 31);
  display.print("RAM: ");
  if (pcRamUsage >= 0) {
    display.print(pcRamUsage);
    display.print("%");
  } else {
    display.print("--");
  }
  
  // Progress Bar for RAM
  display.drawRect(0, 40, 128, 5, WHITE);
  if (pcRamUsage > 0) {
    int fillW = map(pcRamUsage, 0, 100, 0, 126);
    display.fillRect(1, 41, fillW, 3, WHITE);
  }

  // --- GPU Section ---
  display.setCursor(0, 48);
  display.print("GPU: ");
  if (pcGpuUsage >= 0) {
    display.print(pcGpuUsage);
    display.print("%");
  } else {
    display.print("--");
  }
  
  if (pcGpuTemp >= 0) {
    display.setCursor(85, 48);
    display.print(pcGpuTemp);
    display.print("C");
  }
  
  // Progress Bar for GPU
  display.drawRect(0, 57, 128, 5, WHITE);
  if (pcGpuUsage > 0) {
    int fillW = map(pcGpuUsage, 0, 100, 0, 126);
    display.fillRect(1, 58, fillW, 3, WHITE);
  }

  display.display();
}

// Setup
void setup() {
  Serial.begin(115200);
  Serial.println("DeskMate Started");
  setupDeviceId();

  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(ONBOARD_BOOT_PIN, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);

  Wire.begin(0, 1);
  Wire.setTimeOut(100); // Prevent blocking lockups if OLED crashes

  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED Failed");
    while (1);
  }

  randomSeed(millis());
  
  // Show starting screen
  drawEyes(0, 0);

  // Interactive WiFi reset prompt on boot
  bool triggerWiFiReset = false;
  unsigned long startWait = millis();
  const unsigned long WAIT_TIME = 4000; // 4 seconds window to trigger reset
  unsigned long activeStart = 0;
  bool wasActive = false;
  
  while (millis() - startWait < WAIT_TIME) {
    // Check if any of the reset triggers are active:
    // 1. TOUCH_PIN is active HIGH (touched)
    // 2. BUTTON_PIN is active LOW (pressed)
    // 3. ONBOARD_BOOT_PIN is active LOW (pressed)
    bool touchActive = (digitalRead(TOUCH_PIN) == HIGH);
    bool buttonActive = (digitalRead(BUTTON_PIN) == LOW);
    bool bootActive = (digitalRead(ONBOARD_BOOT_PIN) == LOW);
    
    bool currentlyActive = (touchActive || buttonActive || bootActive);
    
    if (currentlyActive) {
      if (!wasActive) {
        activeStart = millis();
        wasActive = true;
      } else if (millis() - activeStart > 2000) { // Held down continuously for 2 seconds
        triggerWiFiReset = true;
        break;
      }
    } else {
      wasActive = false;
    }
    
    // Draw prompt and progress bar
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("     [ DeskBuddy ]");
    display.println("");
    if (wasActive && (millis() - activeStart > 100)) {
      display.println("  KEEP HOLDING TO RESET");
      int holdProgress = map(millis() - activeStart, 0, 2000, 0, 120);
      if (holdProgress > 120) holdProgress = 120;
      display.fillRect(4, 32, holdProgress, 6, WHITE);
    } else {
      display.println("Hold Touch sensor or");
      display.println("press Boot/Button now");
      display.println("to reset Wi-Fi settings");
    }
    
    // Draw countdown progress bar
    int progressWidth = map(millis() - startWait, 0, WAIT_TIME, 120, 0);
    display.drawRect(4, 52, 120, 8, WHITE);
    display.fillRect(4, 52, progressWidth, 8, WHITE);
    
    display.display();
    delay(50);
  }

  if (triggerWiFiReset) {
    Serial.println("WiFi Reset Triggered by user!");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 15);
    display.println("   Resetting WiFi...");
    display.println("");
    display.println("   Please wait...");
    display.display();
    
    WiFiManager wm;
    wm.resetSettings();
    delay(2000);
    
    // Wait until user releases all buttons/touch to avoid re-triggering or skipping
    while (digitalRead(TOUCH_PIN) == HIGH || digitalRead(BUTTON_PIN) == LOW || digitalRead(ONBOARD_BOOT_PIN) == LOW) {
      delay(50);
    }
  }

  // Connect via WiFiManager config portal
  Serial.println("[Setup] Calling connectWiFi...");
  connectWiFi();
  Serial.println("[Setup] connectWiFi returned.");

  // Wait 500ms for power rails to stabilize after Wi-Fi activation
  delay(500);

  // Re-initialize the OLED display to recover from any Wi-Fi power-up brownout/freeze.
  // We first perform I2C bus recovery (clocking SCL pin) to clear any slave lockups.
  Serial.println("[Setup] Re-initializing display post-WiFi...");
  recoverI2C();
  Wire.begin(0, 1);
  Wire.setTimeOut(100);
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("[Setup] OLED Re-init Failed!");
  } else {
    Serial.println("[Setup] OLED Re-init Success!");
  }

  // Show "DeskBuddy by Hari" name on screen during startup
  Serial.println("[Setup] Displaying startup screen name...");
  display.clearDisplay();
  display.setTextColor(WHITE);
  
  // Draw "DeskBuddy" at size 2 (108px wide)
  display.setTextSize(2);
  display.setCursor(10, 17);
  display.print("DeskBuddy");
  
  // Draw "by Hari" at size 1 (42px wide)
  display.setTextSize(1);
  display.setCursor(43, 39);
  display.print("by Hari");
  
  display.display();
  delay(2000); // Display name for 2 seconds

  // Show connected IP and Device ID on screen
  if (WiFi.status() == WL_CONNECTED) {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    
    display.setCursor(0, 0);
    display.println("  Wi-Fi Connected!");
    display.println("");
    display.println("  Pair on Dashboard:");
    display.println("  Device ID:");
    
    display.setTextSize(2);
    display.setCursor(10, 35);
    display.println(deviceId);
    display.display();
    delay(4000); // Display for 4 seconds so the user can read the pairing ID
  }

  // WebSocket setup

  webSocket.beginSSL(CLOUD_RELAY_HOST, CLOUD_RELAY_PORT, "/esp32?device_id=" + deviceId);
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);

  // Initialize cycle timer
  lastModeCycleTime = millis();

  // Draw eyes back after startup completes
  Serial.println("[Setup] Drawing eyes...");
  drawEyes(0, 0);
  Serial.println("[Setup] Eyes drawn.");
}

// Loop
void loop() {
  // Process WebSocket events
  webSocket.loop();

  static unsigned long lastTimePrint = 0;

  // Handle non-blocking vibration
  if (vibrationEnd > 0 && millis() >= vibrationEnd) {
    digitalWrite(VIBRATION_PIN, LOW);
    vibrationEnd = 0;
  }

  if (millis() - lastTimePrint > 1000) {
    Serial.println(getTimeString());
    lastTimePrint = millis();
  }

  // Auto-cycling display modes if PC stats are active
  if (!manualStatsMode) {
    if (millis() - lastPcStatsTime < 15000) {
      unsigned long currentCycleTime = millis() - lastModeCycleTime;
      if (currentMode == MODE_BUDDY && currentCycleTime >= CYCLE_BUDDY_MS) {
        currentMode = MODE_PC_STATS;
        lastModeCycleTime = millis();
        Serial.println("[AutoCycle] Switched to PC Stats Mode");
      } 
      else if (currentMode == MODE_PC_STATS && currentCycleTime >= CYCLE_STATS_MS) {
        currentMode = MODE_BUDDY;
        lastModeCycleTime = millis();
        Serial.println("[AutoCycle] Switched to Buddy Mode");
        drawEyes(pupilX, pupilY);
      }
    } else {
      // If stats are stale, force back to Buddy Mode
      if (currentMode == MODE_PC_STATS && (millis() - lastPcStatsTime >= 15000)) {
        currentMode = MODE_BUDDY;
        Serial.println("[AutoCycle] PC Stats stale. Reverted to Buddy Mode.");
        drawEyes(pupilX, pupilY);
      }
    }
  }

  // BREAK SCREEN
  if (breakScreen) {
    showBreakTime();
    
    // Buzz 3 times during the 5 second break screen
    static unsigned long lastBuzzTime = 0;
    if (millis() - breakStart < 3500) {
      if (millis() - lastBuzzTime > 1200) {
        triggerVibration(400);
        lastBuzzTime = millis();
      }
    }
    
    if (millis() - breakStart > 5000) {
      breakScreen = false;
      drawEyes(pupilX, pupilY);
    }
    delay(1);
    return;
  }

  // BUTTON / Timer switch click & double press detection
  static int buttonClickCount = 0;
  static unsigned long lastButtonClickTime = 0;
  static bool lastButtonReading = HIGH;
  static unsigned long buttonPressTime = 0;
  static bool buttonIsHeld = false;
  static bool buttonLongPressed = false;

  bool buttonReading = digitalRead(BUTTON_PIN);

  if (lastButtonReading == HIGH && buttonReading == LOW) {
    buttonPressTime = millis();
    buttonIsHeld = true;
    buttonLongPressed = false;
  }
  else if (buttonReading == HIGH && lastButtonReading == LOW) {
    buttonIsHeld = false;
    unsigned long pressDuration = millis() - buttonPressTime;
    if (!buttonLongPressed && pressDuration > 50 && pressDuration < 400) {
      buttonClickCount++;
      lastButtonClickTime = millis();
    }
  }

  // Handle long press (continuous hold for 1 second)
  if (buttonIsHeld && !buttonLongPressed && (millis() - buttonPressTime > 1000)) {
    buttonLongPressed = true;
    buttonClickCount = 0; // Cancel normal clicks
    pomodoroRunning = false;
    breakScreen = false;
    pomodoroStart = 0;
    if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
    Serial.println("Pomodoro Reset via Long Press");
    triggerVibration(300);
  }

  lastButtonReading = buttonReading;

  // Process button clicks after timeout
  if (buttonClickCount > 0 && (millis() - lastButtonClickTime > TAP_TIMEOUT)) {
    if (buttonClickCount == 1) {
      // Single press: Toggle Pomodoro
      if (!pomodoroRunning) {
        pomodoroRunning = true;
        pomodoroStart = millis();
        drawFullscreenPomodoro();
        Serial.println("Pomodoro Started");
      } else {
        pomodoroRunning = false;
        if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
        Serial.println("Pomodoro Stopped");
      }
      triggerVibration(100);
    }
    else if (buttonClickCount == 2) {
      // Double press: Toggle PC Stats screen
      currentMode = (currentMode == MODE_BUDDY) ? MODE_PC_STATS : MODE_BUDDY;
      manualStatsMode = (currentMode == MODE_PC_STATS);
      lastModeCycleTime = millis(); // Reset cycle timer to prevent immediate cycling
      triggerVibration(150);
      Serial.println("[Button] Double Press: Toggled PC Stats Display");
      if (currentMode == MODE_BUDDY) {
        drawEyes(pupilX, pupilY);
      } else {
        drawPCStats();
      }
    }
    buttonClickCount = 0;
  }

  bool touched = (digitalRead(TOUCH_PIN) == HIGH);

  // SLEEP MODE
  if (sleeping) {
    sleepingScreen();

    if (touched) {
      if (touchStart == 0) touchStart = millis();

      if (millis() - touchStart > 1500) { // 1.5s to wake
        sleeping = false;
        wakeAnimation();
        while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
        touchStart = 0;
        lastTouched = false;
        tapCount = 0;
      }
    } else {
      touchStart = 0;
    }

    delay(100);
    return;
  }

  // AWAKE Touch & Tap detection
  if (touched && !lastTouched) {
    touchStart = millis();
    lastTouched = true;
  }
  else if (!touched && lastTouched) {
    unsigned long touchDuration = millis() - touchStart;
    lastTouched = false;
    
    // Extreme sensitivity to soft touches (2ms to 300ms)
    if (touchDuration > 2 && touchDuration < 300) {
      tapCount++;
      lastTapTime = millis();
    }
  }

  // Long press to sleep (1.5 seconds instead of 2.0s)
  if (touched && lastTouched && (millis() - touchStart > 1500)) {
    tapCount = 0;
    lastTouched = false;
    sleepAnimation();
    sleeping = true;
    while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
    touchStart = 0;
  }

  // Process tap actions with ultra-fast timeout (100ms)
  if (tapCount > 0 && (millis() - lastTapTime > 100)) {
    if (tapCount == 1) {
      // Single Tap: Fun reactive animations
      int randAnim = random(0, 4); // 0=Happy, 1=Wink, 2=Heart, 3=Dizzy
      if (randAnim == 0) {
        triggerVibration(80);
        drawHappyEyes();
        delay(800); // Snappier display block (800ms)
      } else if (randAnim == 1) {
        triggerVibration(80);
        drawWinkEyes();
        delay(800);
      } else if (randAnim == 2) {
        triggerVibration(80);
        drawHeartEyes();
        delay(800);
      } else {
        triggerVibration(80);
        drawDizzyEyes();
        delay(800);
      }
      if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
    } 
    else if (tapCount >= 2) {
      // Double Tap or more: Angry or Sad expressions
      int randExpression = random(0, 2);
      if (randExpression == 0) {
        // Double quick buzz
        triggerVibration(60);
        delay(100);
        triggerVibration(60);
        drawAngryEyes();
        delay(1000); // 1.0s display block
      } else {
        // Double slow buzz
        triggerVibration(120);
        delay(150);
        triggerVibration(120);
        drawSadEyes();
        delay(1000);
      }
      if (currentMode == MODE_BUDDY) drawEyes(pupilX, pupilY);
    }
    tapCount = 0;
  }

  // Draw appropriate screen state
  if (pomodoroRunning) {
    // Redraw Fullscreen Pomodoro screen periodically
    static unsigned long lastPomoDraw = 0;
    if (millis() - lastPomoDraw > 500) {
      drawFullscreenPomodoro();
      lastPomoDraw = millis();
    }
  }
  else if (currentMode == MODE_PC_STATS) {
    // Redraw PC Stats screen periodically
    static unsigned long lastStatsDraw = 0;
    if (millis() - lastStatsDraw > 1000) {
      drawPCStats();
      lastStatsDraw = millis();
    }
  } else {
    // RANDOM EYE MOVEMENT
    if (millis() - lastMove > 1200) {
      pupilX = random(-3, 4);
      pupilY = random(-2, 3);
      drawEyes(pupilX, pupilY);
      lastMove = millis();
    }

    // BLINK animation
    if (millis() - lastBlink > random(3000, 7000)) {
      blinkEyes();
      lastBlink = millis();
    }
  }

  delay(1);
}