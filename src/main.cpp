#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
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

// Hardware Pins (realigned for mic/speaker and D13 button)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define TOUCH_PIN 26
#define BUTTON_PIN 13

// Onboard status LED
#ifdef LED_BUILTIN
const int ledPin = LED_BUILTIN;
#else
const int ledPin = 2;
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
void setupI2SSpeaker();
void setupI2SMic();
void playTestTone();
void setupDeviceId();

// --- WebSocket Event Callback ---

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WebSocketClient] Disconnected from Cloud Relay");
      break;
    case WStype_CONNECTED:
      Serial.println("[WebSocketClient] Connected to Cloud Relay!");
      break;
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

void setupI2SSpeaker() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = 14,
    .ws_io_num = 27,
    .data_out_num = 19,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

void setupI2SMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = 32,
    .ws_io_num = 25,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = 33
  };
  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pin_config);
}

// --- original DeskMate display and timer routines ---

void drawAIText() {
  if (aiReplyText.length() > 0 && millis() - aiReplyDisplayStart < 6000) {
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(4, 48); // Fits 2 lines at the bottom (Y=48 to Y=64)
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

void connectWiFi() {
  WiFiManager wm;
  if (wm.autoConnect("DeskMate_Setup")) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--";

  char buffer[6];
  strftime(buffer, sizeof(buffer), "%I:%M", &timeinfo);
  return String(buffer);
}

void drawTopInfo() {
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
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED Failed");
    while (1);
  }

  randomSeed(millis());
  
  // Show starting screen
  drawEyes(0, 0);

  // Connect via WiFiManager config portal
  connectWiFi();

  // Initialize device ID
  setupDeviceId();

  // Load API Key from flash preferences
  preferences.begin("deskbuddy", false);
  geminiApiKey = preferences.getString("apiKey", "AQ.Ab8RN6IO_s5pTZaB2bOUIXs5yKpu_SF6cBi0TaSo9dR4Gva2eA");
  preferences.end();
  Serial.printf("[Preferences] Active Gemini API Key: %s\n", geminiApiKey.c_str());

  // Setup I2S Audio Speaker and Mic
  setupI2SSpeaker();
  setupI2SMic();

  // Play diagnostic test tone to verify physical speaker is working
  playTestTone();

  // Connect to Cloud WebSocket Relay
  String wsUrl = "/esp32?device_id=" + deviceId;
  webSocket.beginSslWithCA(CLOUD_RELAY_HOST, CLOUD_RELAY_PORT, wsUrl.c_str());
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);

  // Show connected IP and Device ID on screen
  if (WiFi.status() == WL_CONNECTED) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Wi-Fi Connected!");
    display.println("");
    display.println("Pair on Dashboard:");
    display.println(CLOUD_RELAY_HOST);
    display.println("");
    display.print("Device ID: ");
    display.setTextSize(2);
    display.println(deviceId);
    display.display();
    delay(5000); // Display for 5 seconds
  }
}

// Loop
void loop() {
  static unsigned long lastTimePrint = 0;

  // Handle WebSocket client connections and processing
  webSocket.loop();

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
    esp_err_t result = i2s_read(I2S_NUM_1, micBuffer, sizeof(micBuffer), &bytesRead, 10);
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