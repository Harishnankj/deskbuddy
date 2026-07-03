#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <time.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "driver/i2s.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "startup_sound.h"

// Hardware Pins (realigned for ESP32-C3 SuperMini)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define TOUCH_PIN 2
#define BUTTON_PIN 3
#define ONBOARD_BOOT_PIN 9

// I2S Audio Pins (Shared clocks configuration)
#define I2S_WS_PIN 4
#define I2S_BCLK_PIN 5
#define I2S_DIN_PIN 6   // Mic input
#define I2S_DOUT_PIN 7  // Speaker output

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

// Cloud WebSocket Relay Configuration
// NOTE: Change this to your deployed Render app host (e.g. "my-deskbuddy.onrender.com")
#define CLOUD_RELAY_HOST "deskbuddy-relay.onrender.com"
#define CLOUD_RELAY_PORT 443

// Display Instance
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// WebSockets Client to connect to the cloud relay
WebSocketsClient webSocket;

// Unique Device ID generated on boot
String deviceId = "";

// Preferences for persistent flash storage
Preferences preferences;
String geminiApiKey = "";

// Global State Variables (from wifiindeskmate.ino)
bool sleeping = false;
unsigned long touchStart = 0;
int pupilX = 0;
int pupilY = 0;
unsigned long lastBlink = 0;
unsigned long lastMove = 0;
int zY = 45;

// POMODORO State
bool pomodoroRunning = false;
bool breakScreen = false;
unsigned long pomodoroStart = 0;
unsigned long breakStart = 0;
const unsigned long POMODORO_TIME = 25UL * 60UL * 1000UL; // 25 mins
bool lastButtonState = HIGH;

// Audio Streaming State
bool isRecording = false;
enum RecordingState {
  REC_IDLE,
  REC_PENDING,
  REC_ACTIVE
};
RecordingState recState = REC_IDLE;
unsigned long recPendingStart = 0;
const unsigned long REC_DELAY_MS = 1200; // 1.2s delay for "listening" prompt to play


// AI speech display variables
String aiReplyText = "";
unsigned long aiReplyDisplayStart = 0;

// Function Declarations
String getTimeString();
void drawTopInfo();
void drawWiFiStatus();
void drawTimer();
void drawAIText();
void drawEyes(int px, int py);
void drawClosedEyes();
void drawHappyEyes();
void drawWinkEyes();
void blinkEyes();
void sleepAnimation();
void wakeAnimation();
void sleepingScreen();
void showBreakTime();
void executeAction(String action);
void setupI2SFullDuplex();
void playTestTone();
void setupDeviceId();
void recoverI2C();

// --- WebSocket Event Callback ---

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WebSocketClient] Disconnected from Cloud Relay");
      break;
    case WStype_CONNECTED: {
      Serial.println("[WebSocketClient] Connected to Cloud Relay!");
      // Send registration payload with saved Gemini API Key
      JsonDocument regDoc;
      regDoc["event"] = "register_esp32";
      regDoc["apiKey"] = geminiApiKey;
      String regStr;
      serializeJson(regDoc, regStr);
      webSocket.sendTXT(regStr);
      Serial.println("[WebSocketClient] Registration sent to server.");
      break;
    }
    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.print("[WebSocketClient] Deserialization failed: ");
        Serial.println(error.c_str());
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
        resDoc["apiKey"] = geminiApiKey;
        
        String resStr;
        serializeJson(resDoc, resStr);
        webSocket.sendTXT(resStr);
      } 
      else if (event == "set_key") {
        String key = doc["key"];
        preferences.begin("deskbuddy", false);
        preferences.putString("apiKey", key);
        preferences.end();
        geminiApiKey = key;
        
        JsonDocument resDoc;
        resDoc["event"] = "set_key_response";
        resDoc["status"] = "ok";
        
        String resStr;
        serializeJson(resDoc, resStr);
        webSocket.sendTXT(resStr);
        Serial.println("[WebSocketClient] Gemini API key updated.");

        // Update server registration with new key
        JsonDocument regDoc;
        regDoc["event"] = "register_esp32";
        regDoc["apiKey"] = geminiApiKey;
        String regStr;
        serializeJson(regDoc, regStr);
        webSocket.sendTXT(regStr);
        Serial.println("[WebSocketClient] Re-registered key with server.");
      }
      else if (event == "action") {
        String actionType = doc["type"];
        if (doc.containsKey("reply")) {
          aiReplyText = doc["reply"].as<String>();
          aiReplyDisplayStart = millis();
        }
        executeAction(actionType);
      }
      break;
    }
    case WStype_BIN: {
      size_t bytesWritten = 0;
      i2s_write(I2S_NUM_0, payload, length, &bytesWritten, portMAX_DELAY);
      break;
    }
    case WStype_ERROR:
      Serial.println("[WebSocketClient] Socket error occurred!");
      break;
  }
}

// --- Action Execution Logic ---

void executeAction(String action) {
  Serial.printf("[Action Executer] Triggering: %s\n", action.c_str());

  if (action == "led_on") {
    digitalWrite(ledPin, HIGH);
  } 
  else if (action == "led_off") {
    digitalWrite(ledPin, LOW);
  } 
  else if (action == "happy") {
    drawHappyEyes();
    delay(700);
    drawEyes(pupilX, pupilY);
  } 
  else if (action == "wink") {
    drawWinkEyes();
    delay(1000);
    drawEyes(pupilX, pupilY);
  } 
  else if (action == "blink") {
    blinkEyes();
  } 
  else if (action == "sleep") {
    if (!sleeping) {
      sleepAnimation();
      sleeping = true;
    }
  } 
  else if (action == "wake" || action == "normal") {
    if (sleeping) {
      sleeping = false;
      wakeAnimation();
    } else {
      drawEyes(0, 0);
    }
  } 
  else if (action == "start_pomodoro" || action == "toggle_pomodoro") {
    if (!pomodoroRunning) {
      pomodoroRunning = true;
      pomodoroStart = millis();
      Serial.println("Pomodoro Started");
    } else {
      pomodoroRunning = false;
      Serial.println("Pomodoro Stopped");
    }
  } 
  else if (action == "stop_pomodoro") {
    if (pomodoroRunning) {
      pomodoroRunning = false;
      Serial.println("Pomodoro Stopped");
    }
  } 
  else if (action == "trigger_voice") {
    if (recState == REC_IDLE) {
      recState = REC_PENDING;
      recPendingStart = millis();
      aiReplyText = "Listening...";
      aiReplyDisplayStart = millis();
      drawEyes(0, 0);
      webSocket.sendTXT("{\"event\":\"speak_listening\"}");
      Serial.println("[Action] Recording Pending triggered via Web API");
    }
  } 
  else if (action == "stop_voice") {
    if (recState == REC_ACTIVE || recState == REC_PENDING) {
      isRecording = false;
      recState = REC_IDLE;
      aiReplyText = "Thinking...";
      aiReplyDisplayStart = millis();
      drawEyes(0, 0);
      webSocket.sendTXT("{\"event\":\"stop_recording\"}");
      Serial.println("[Action] Recording Stopped via Web API");
    }
  }
}

// --- I2S Configuration Functions ---

void playTestTone() {
  Serial.println("[Speaker Test] Playing startup voice greeting...");
  
  const int chunkSize = 256;
  int16_t ramBuffer[chunkSize];
  
  size_t samplesRemaining = startup_sound_len;
  size_t offset = 0;
  
  while (samplesRemaining > 0) {
    size_t samplesToRead = (samplesRemaining > chunkSize) ? chunkSize : samplesRemaining;
    
    // Read from flash memory PROGMEM to internal RAM
    memcpy_P(ramBuffer, &startup_sound[offset], samplesToRead * sizeof(int16_t));
    
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, ramBuffer, samplesToRead * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    
    offset += samplesToRead;
    samplesRemaining -= samplesToRead;
  }
  
  Serial.println("[Speaker Test] Startup voice greeting complete.");
}

void setupI2SFullDuplex() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 16,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_DIN_PIN
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// --- original DeskMate display and timer routines ---

void drawAIText() {
  if (aiReplyText.length() > 0 && millis() - aiReplyDisplayStart < 6000) {
    display.setTextSize(1);
    display.setTextColor(WHITE);
    
    int textWidth = aiReplyText.length() * 6;
    int viewWidth = 120;
    int scrollRange = textWidth - viewWidth;
    
    int xPos = 4;
    if (scrollRange > 0) {
      unsigned long elapsed = millis() - aiReplyDisplayStart;
      unsigned long scrollDuration = scrollRange * 25; // 25ms per pixel scroll speed
      unsigned long cycleTime = scrollDuration + 2000; // 1s start pause + 1s end pause
      
      unsigned long phase = elapsed % cycleTime;
      if (phase < 1000) {
        xPos = 4;
      } else if (phase < 1000 + scrollDuration) {
        int pixelsScrolled = (phase - 1000) / 25;
        xPos = 4 - pixelsScrolled;
      } else {
        xPos = 4 - scrollRange;
      }
    } else {
      // Center small text
      xPos = (128 - textWidth) / 2;
    }
    
    display.setCursor(xPos, 56); // Shifted down from 48 to 56 to avoid eyes
    display.print(aiReplyText);
  }
}

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

void drawEyes(int px, int py) {
  display.clearDisplay();
  drawTopInfo();

  display.fillRoundRect(20, 24, 30, 24, 10, WHITE);
  display.fillRoundRect(78, 24, 30, 24, 10, WHITE);

  display.fillCircle(35 + px, 36 + py, 5, BLACK);
  display.fillCircle(93 + px, 36 + py, 5, BLACK);

  drawTimer();
  drawAIText();
  display.display();
}

void drawClosedEyes() {
  display.clearDisplay();
  drawTopInfo();

  display.drawLine(20, 30, 50, 30, WHITE);
  display.drawLine(78, 30, 108, 30, WHITE);

  drawTimer();
  drawAIText();
  display.display();
}

void drawHappyEyes() {
  display.clearDisplay();
  drawTopInfo();

  display.drawLine(20, 30, 35, 20, WHITE);
  display.drawLine(35, 20, 50, 30, WHITE);

  display.drawLine(78, 30, 93, 20, WHITE);
  display.drawLine(93, 20, 108, 30, WHITE);

  drawTimer();
  drawAIText();
  display.display();
}

void drawWinkEyes() {
  display.clearDisplay();
  drawTopInfo();

  // Left eye normal
  display.fillRoundRect(20, 24, 30, 24, 10, WHITE);
  display.fillCircle(35 + pupilX, 36 + pupilY, 5, BLACK);

  // Right eye winking/closed
  display.drawLine(78, 36, 108, 36, WHITE);

  drawTimer();
  drawAIText();
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
    display.fillRoundRect(20, 30 - h / 2, 30, h, 8, WHITE);
    display.fillRoundRect(78, 30 - h / 2, 30, h, 8, WHITE);
    display.display();
    delay(80);
  }
  drawClosedEyes();
}

void wakeAnimation() {
  for (int h = 2; h <= 24; h += 2) {
    display.clearDisplay();
    drawTopInfo();
    display.fillRoundRect(20, 30 - h / 2, 30, h, 8, WHITE);
    display.fillRoundRect(78, 30 - h / 2, 30, h, 8, WHITE);
    display.display();
    delay(80);
  }
  drawEyes(0, 0);
}

void sleepingScreen() {
  display.clearDisplay();
  drawTopInfo();

  display.drawLine(20, 30, 50, 30, WHITE);
  display.drawLine(78, 30, 108, 30, WHITE);

  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(100, zY);
  display.print("Z");

  display.setTextSize(1);
  display.setCursor(114, zY - 10);
  display.print("z");

  display.setCursor(121, zY - 18);
  display.print(".");

  drawTimer();
  drawAIText();
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
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.ssd1306_command(SSD1306_DISPLAYON);
  
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
  display.ssd1306_command(SSD1306_DISPLAYOFF);
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
      display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
      display.ssd1306_command(SSD1306_DISPLAYON);
    }
  }

  if (!pomodoroRunning) {
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.print(getTimeString());
  }
  drawWiFiStatus();
}

void drawWiFiStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    display.setTextSize(1);
    display.setCursor(112, 0);
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
    display.fillRect(x + i * 4, 8 - (i * 2), 3, 2 + (i * 2), WHITE);
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  Serial.println("DeskMate Started");

  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(ONBOARD_BOOT_PIN, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  Wire.begin(0, 1);
  Wire.setTimeOut(100); // Prevent blocking lockups if OLED crashes

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
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
  
  while (millis() - startWait < WAIT_TIME) {
    // Check if any of the reset triggers are active:
    // 1. TOUCH_PIN is active HIGH (touched)
    // 2. BUTTON_PIN is active LOW (pressed)
    // 3. ONBOARD_BOOT_PIN is active LOW (pressed)
    bool touchActive = (digitalRead(TOUCH_PIN) == HIGH);
    bool buttonActive = (digitalRead(BUTTON_PIN) == LOW);
    bool bootActive = (digitalRead(ONBOARD_BOOT_PIN) == LOW);
    
    if (touchActive || buttonActive || bootActive) {
      triggerWiFiReset = true;
      break;
    }
    
    // Draw prompt and progress bar
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("     [ DeskBuddy ]");
    display.println("");
    display.println("Hold Touch sensor or");
    display.println("press Boot/Button now");
    display.println("to reset Wi-Fi settings");
    
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
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[Setup] OLED Re-init Failed!");
  } else {
    Serial.println("[Setup] OLED Re-init Success!");
  }

  // Load API Key and Device ID from flash preferences in a single block to prevent lockups
  Serial.println("[Setup] Loading settings from flash preferences...");
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
  Serial.printf("[Preferences] Active Device ID: %s\n", deviceId.c_str());

  geminiApiKey = preferences.getString("apiKey", "AQ.Ab8RN6IO_s5pTZaB2bOUIXs5yKpu_SF6cBi0TaSo9dR4Gva2eA");
  preferences.end();
  Serial.printf("[Preferences] Active Gemini API Key: %s\n", geminiApiKey.c_str());

  // Setup I2S Audio in Full Duplex Mode (Shared clocks)
  Serial.println("[Setup] Calling setupI2SFullDuplex...");
  setupI2SFullDuplex();
  Serial.println("[Setup] setupI2SFullDuplex returned.");

  // Show "DeskBuddy by Hari" name on screen during startup sound
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
  Serial.println("[Setup] Display output pushed.");

  // Play diagnostic test tone to verify physical speaker is working
  Serial.println("[Setup] Calling playTestTone...");
  playTestTone();
  Serial.println("[Setup] playTestTone returned.");

  // Re-initialize the OLED display to recover from any speaker power-up brownout/freeze.
  // We first perform I2C bus recovery (clocking SCL pin) to clear any slave lockups.
  Serial.println("[Setup] Re-initializing display post-startup sound...");
  recoverI2C();
  Wire.begin(0, 1);
  Wire.setTimeOut(100);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[Setup] OLED Post-Sound Re-init Failed!");
  } else {
    Serial.println("[Setup] OLED Post-Sound Re-init Success!");
  }

  // Draw eyes back after startup sound completes
  Serial.println("[Setup] Drawing eyes...");
  drawEyes(0, 0);
  Serial.println("[Setup] Eyes drawn.");

  // Connect to WebSocket Relay
  Serial.println("[Setup] Configuring WebSocket connection...");
  String wsUrl = "/esp32?device_id=" + deviceId;

  // Check if a local relay server is running on the gateway IP on port 3000
  IPAddress gateway = WiFi.gatewayIP();
  WiFiClient client;
  bool localServerFound = false;

  Serial.printf("[Setup] Probing local relay server at %s:3000...\n", gateway.toString().c_str());
  if (client.connect(gateway, 3000)) {
    Serial.println("[Setup] Local relay server detected!");
    localServerFound = true;
    client.stop();
  } else {
    Serial.println("[Setup] No local relay server found. Using Cloud Relay fallback.");
  }

  if (localServerFound) {
    // Local WebSocket connection (no SSL)
    webSocket.begin(gateway, 3000, wsUrl.c_str());
  } else {
    // Cloud WebSocket connection (with SSL)
    webSocket.beginSslWithCA(CLOUD_RELAY_HOST, CLOUD_RELAY_PORT, wsUrl.c_str());
  }

  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);
  Serial.println("[Setup] WebSocket configured.");

  // Show connected IP and Device ID on screen
  if (WiFi.status() == WL_CONNECTED) {
    display.clearDisplay();
    display.setTextColor(WHITE);
    
    // Title
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Wi-Fi Connected!");
    
    // URL
    display.setCursor(0, 10);
    display.println("Go to Dashboard:");
    display.println("harishnankj.github.io");
    display.println("/deskbuddy");
    
    // Device ID Label
    display.setCursor(0, 38);
    display.println("Device ID:");
    
    // Device ID (Centered, size 2)
    int textWidth = deviceId.length() * 12; // char width at size 2 is 12px
    int xPos = (128 - textWidth) / 2;
    if (xPos < 0) xPos = 0;
    display.setCursor(xPos, 48);
    display.setTextSize(2);
    display.print(deviceId);
    
    display.display();
    delay(5000); // Display for 5 seconds
  }
}

// Loop
void loop() {
  static unsigned long lastTimePrint = 0;

  // Handle WebSocket client connections and processing
  webSocket.loop();

  // Smoothly redraw eyes during scrolling animation (20 FPS)
  if (aiReplyText.length() > 0 && millis() - aiReplyDisplayStart < 6000) {
    static unsigned long lastScrollTime = 0;
    if (millis() - lastScrollTime > 50) {
      if (!sleeping && !breakScreen) {
        drawEyes(pupilX, pupilY);
      }
      lastScrollTime = millis();
    }
  }

  if (millis() - lastTimePrint > 1000) {
    Serial.println(getTimeString());
    lastTimePrint = millis();
  }

  // Handle pending recording start
  if (recState == REC_PENDING) {
    if (millis() - recPendingStart >= REC_DELAY_MS) {
      recState = REC_ACTIVE;
      isRecording = true;
      webSocket.sendTXT("{\"event\":\"start_recording\"}");
      Serial.println("[Touch] Recording Started (Mic Active)");
    }
  }

  // Stream Mic Data to Web Client if active recording
  if (isRecording) {
    int16_t micBuffer[128];
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_NUM_0, micBuffer, sizeof(micBuffer), &bytesRead, 10);
    if (result == ESP_OK && bytesRead > 0) {
      webSocket.sendBIN((uint8_t*)micBuffer, bytesRead);
    }
  }

  // BREAK SCREEN
  if (breakScreen) {
    showBreakTime();
    if (millis() - breakStart > 5000) {
      breakScreen = false;
      drawEyes(0, 0);
    }
    delay(1);
    return;
  }

  // BUTTON Input
  bool buttonState = digitalRead(BUTTON_PIN);
  if (lastButtonState == HIGH && buttonState == LOW) {
    if (!pomodoroRunning) {
      pomodoroRunning = true;
      pomodoroStart = millis();
      Serial.println("Pomodoro Started");
    } else {
      pomodoroRunning = false;
      Serial.println("Pomodoro Stopped");
    }
    delay(200);
  }
  lastButtonState = buttonState;

  bool touched = digitalRead(TOUCH_PIN);

  // SLEEP MODE
  if (sleeping) {
    sleepingScreen();

    if (touched) {
      if (touchStart == 0) touchStart = millis();

      if (millis() - touchStart > 2000) {
        sleeping = false;
        wakeAnimation();
        while (digitalRead(TOUCH_PIN)) delay(10);
        touchStart = 0;
      }
    } else {
      touchStart = 0;
    }

    delay(100);
    return;
  }

  // TOUCH / MIC trigger Input
  if (touched) {
    if (touchStart == 0) {
      touchStart = millis();
    }

    if (millis() - touchStart > 2000) {
      // Long press: Sleep Animation
      sleepAnimation();
      sleeping = true;
      while (digitalRead(TOUCH_PIN)) delay(10);
      touchStart = 0;
    }
  } else {
    if (touchStart > 0) {
      unsigned long touchDuration = millis() - touchStart;
      touchStart = 0;
      
      if (touchDuration > 50 && touchDuration < 1500) {
        // Short Press: Toggle voice recording state
        if (recState == REC_IDLE) {
          recState = REC_PENDING;
          recPendingStart = millis();
          aiReplyText = "Listening...";
          aiReplyDisplayStart = millis();
          drawEyes(pupilX, pupilY);
          webSocket.sendTXT("{\"event\":\"speak_listening\"}");
          Serial.println("[Touch] Recording Pending (Speaking Prompt)");
        } else {
          isRecording = false;
          recState = REC_IDLE;
          aiReplyText = "Thinking...";
          aiReplyDisplayStart = millis();
          drawEyes(pupilX, pupilY);
          webSocket.sendTXT("{\"event\":\"stop_recording\"}");
          Serial.println("[Touch] Recording Stopped");
        }
      }
    }
  }

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

  delay(1);
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