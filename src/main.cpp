#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <time.h>

// WiFi Configuration - CHANGE THESE!
const char* WIFI_SSID = "Hendrawifi";
const char* WIFI_PASSWORD = "Waksolumin1234";

// Access Point Configuration
const char* AP_SSID = "ESP32-ADC";
const char* AP_PASSWORD = "12345678";  // At least 8 characters

// OLED Display Configuration for 0.42" display
#define I2C_SDA 5
#define I2C_SCL 6
#define ADC_PIN 2  // GPIO2 for ADC input (0-3.3V)
#define LED_PIN 3  // LED pin for time-based control
#define LED_PIN2 8 // Mirrors LED_PIN state

// How often to disable WiFi, take a voltage reading, and push it to Node-RED
const int VOLTAGE_COLLECT_INTERVAL_SEC = 10;
const int WIFI_RECONNECT_ATTEMPTS = 20; // x 500ms = 10s timeout

// U8g2 constructor for SSD1306 128x64 display with Hardware I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

const int DISPLAY_WIDTH = 72;
const int DISPLAY_HEIGHT = 40;
const int X_OFFSET = 28;
const int Y_OFFSET = 24;

// ADC Calibration (V = slope * ADC + offset)
float ADC_SLOPE = 0.00070978;
float ADC_OFFSET = 0.0453;

// Web server and preferences
AsyncWebServer server(80);
Preferences preferences;

// Network info for OLED display
IPAddress staIP;
IPAddress apIP;
bool staConnected = false;
bool apFallbackMode = false; // true when STA connect failed and AP is used as a fallback

// Dashboard login
const char* DASHBOARD_USER = "admin";
const char* DASHBOARD_PASS = "passadmin11!";

// LED manual control (overrides the time-based schedule once used)
bool ledOverride = false;
bool ledManualState = false;

// LED on/off schedule (time-of-day, stored in flash)
int ledOnHour = 19;
int ledOnMinute = 0;
int ledOffHour = 5;
int ledOffMinute = 0;

// Node-RED endpoint URL (stored in flash)
String noderedUrl = "";

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // UTC +7
const int   daylightOffset_sec = 0;

// Drive LED_PIN and LED_PIN2 together so they always match
void setLed(bool state) {
  digitalWrite(LED_PIN, state ? HIGH : LOW);
  digitalWrite(LED_PIN2, state ? LOW : HIGH);
}

// Calibrated voltage conversion
float getCalibratedVoltage(int adcValue) {
  return (ADC_SLOPE * adcValue) + ADC_OFFSET;
}

// Draw text horizontally centered within the visible display window,
// clamped so it never starts left of X_OFFSET (avoids right-edge clipping)
void drawCentered(int y, const char* text) {
  int w = u8g2.getStrWidth(text);
  int x = X_OFFSET + (DISPLAY_WIDTH - w) / 2;
  if (x < X_OFFSET) x = X_OFFSET;
  u8g2.drawStr(x, y, text);
}

// Load calibration from flash
void loadCalibration() {
  preferences.begin("adc-cal", true);
  ADC_SLOPE = preferences.getFloat("slope", 0.00070978);
  ADC_OFFSET = preferences.getFloat("offset", 0.0453);
  preferences.end();
  
  Serial.println("\n--- Loaded Calibration ---");
  Serial.print("Slope: ");
  Serial.println(ADC_SLOPE, 8);
  Serial.print("Offset: ");
  Serial.println(ADC_OFFSET, 4);
  Serial.println("-------------------------\n");
}

// Save calibration to flash
void saveCalibration(float slope, float offset) {
  preferences.begin("adc-cal", false);
  preferences.putFloat("slope", slope);
  preferences.putFloat("offset", offset);
  preferences.end();
  
  ADC_SLOPE = slope;
  ADC_OFFSET = offset;
  
  Serial.println("\n--- Saved Calibration ---");
  Serial.print("Slope: ");
  Serial.println(slope, 8);
  Serial.print("Offset: ");
  Serial.println(offset, 4);
  Serial.println("-------------------------\n");
}

// Load LED schedule from flash
void loadLedSchedule() {
  preferences.begin("led-cfg", true);
  ledOnHour = preferences.getInt("onHour", 19);
  ledOnMinute = preferences.getInt("onMin", 0);
  ledOffHour = preferences.getInt("offHour", 5);
  ledOffMinute = preferences.getInt("offMin", 0);
  preferences.end();

  Serial.println("\n--- Loaded LED Schedule ---");
  Serial.printf("ON:  %02d:%02d\n", ledOnHour, ledOnMinute);
  Serial.printf("OFF: %02d:%02d\n", ledOffHour, ledOffMinute);
  Serial.println("-------------------------\n");
}

// Save LED schedule to flash
void saveLedSchedule(int onHour, int onMinute, int offHour, int offMinute) {
  preferences.begin("led-cfg", false);
  preferences.putInt("onHour", onHour);
  preferences.putInt("onMin", onMinute);
  preferences.putInt("offHour", offHour);
  preferences.putInt("offMin", offMinute);
  preferences.end();

  ledOnHour = onHour;
  ledOnMinute = onMinute;
  ledOffHour = offHour;
  ledOffMinute = offMinute;

  Serial.println("\n--- Saved LED Schedule ---");
  Serial.printf("ON:  %02d:%02d\n", ledOnHour, ledOnMinute);
  Serial.printf("OFF: %02d:%02d\n", ledOffHour, ledOffMinute);
  Serial.println("-------------------------\n");
}

// Load Node-RED URL from flash
void loadNoderedConfig() {
  preferences.begin("nodered-cfg", true);
  noderedUrl = preferences.getString("url", "");
  preferences.end();

  Serial.println("\n--- Loaded Node-RED Config ---");
  Serial.print("URL: ");
  Serial.println(noderedUrl);
  Serial.println("-------------------------\n");
}

// Save Node-RED URL to flash
void saveNoderedConfig(const String &url) {
  preferences.begin("nodered-cfg", false);
  preferences.putString("url", url);
  preferences.end();

  noderedUrl = url;

  Serial.println("\n--- Saved Node-RED Config ---");
  Serial.print("URL: ");
  Serial.println(noderedUrl);
  Serial.println("-------------------------\n");
}

// Whether the configured schedule says the LED should be on right now (handles overnight wrap)
bool isLedScheduleOn(const struct tm &timeinfo) {
  int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int onMinutes = ledOnHour * 60 + ledOnMinute;
  int offMinutes = ledOffHour * 60 + ledOffMinute;

  if (onMinutes == offMinutes) return false;

  if (onMinutes < offMinutes) {
    return nowMinutes >= onMinutes && nowMinutes < offMinutes;
  } else {
    return nowMinutes >= onMinutes || nowMinutes < offMinutes;
  }
}

// Blocking connect as a WiFi station. Returns true if connected within the attempt budget.
bool connectStation() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_RECONNECT_ATTEMPTS) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    staConnected = true;
    staIP = WiFi.localIP();

    if (MDNS.begin("esp32")) {
      MDNS.addService("http", "tcp", 80);
    }
    return true;
  }

  staConnected = false;
  return false;
}

// POST a voltage reading + LED status to {noderedUrl}/battery
void sendVoltageToNodered(float voltage, bool ledOn) {
  if (noderedUrl.length() == 0) return;

  HTTPClient http;
  String url = noderedUrl + "/battery";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"data\":" + String(voltage, 3) + ",\"status\":\"" + String(ledOn ? "on" : "off") + "\"}";
  int httpCode = http.POST(payload);

  Serial.print("Node-RED POST ");
  Serial.print(url);
  Serial.print(" -> ");
  Serial.println(httpCode);

  http.end();
}

// Disable WiFi, take an isolated ADC reading, reconnect, then push it to Node-RED
void collectAndSendVoltage() {
  Serial.println("\n--- Voltage Collection Cycle ---");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  int adcValue = analogRead(ADC_PIN);
  float voltage = getCalibratedVoltage(adcValue);
  bool ledOn = digitalRead(LED_PIN) == HIGH;
  Serial.print("Voltage: ");
  Serial.println(voltage, 3);

  if (connectStation()) {
    sendVoltageToNodered(voltage, ledOn);
  } else {
    Serial.println("WiFi reconnect failed, skipping Node-RED send");
  }

  Serial.println("-------------------------\n");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32-C3 ADC Web Calibration ===");
  
  // Configure ADC
  pinMode(ADC_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_PIN2, OUTPUT);
  setLed(false);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  
  // Load calibration
  loadCalibration();
  loadLedSchedule();
  loadNoderedConfig();
  
  // Initialize OLED
  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);
  
  // Show connecting message
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13_tr);
  drawCentered(Y_OFFSET + 15, "Starting");
  drawCentered(Y_OFFSET + 30, "WiFi...");
  u8g2.sendBuffer();
  
  // Connect to WiFi as a station; only fall back to AP mode if that fails
  Serial.print("Connecting to: ");
  Serial.println(WIFI_SSID);

  if (connectStation()) {
    Serial.println("WiFi connected!");
    Serial.print("Station IP: ");
    Serial.println(staIP);
    apFallbackMode = false;
    Serial.println("mDNS started: http://esp32.local");

    // Display Station IP
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    drawCentered(Y_OFFSET + 9, "Connected!");
    u8g2.setFont(u8g2_font_4x6_tr);
    char ipStr[20];
    sprintf(ipStr, "STA:%s", staIP.toString().c_str());
    drawCentered(Y_OFFSET + 20, ipStr);
    u8g2.sendBuffer();

    // Sync time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("NTP Configured");

    delay(3000);
  } else {
    Serial.println("WiFi failed, starting Access Point fallback");
    apFallbackMode = true;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    apIP = WiFi.softAPIP();
    Serial.print("AP SSID: ");
    Serial.println(AP_SSID);
    Serial.print("AP Password: ");
    Serial.println(AP_PASSWORD);
    Serial.print("AP IP: ");
    Serial.println(apIP);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    drawCentered(Y_OFFSET + 15, "AP Mode");
    u8g2.setFont(u8g2_font_4x6_tr);
    char ipStr[20];
    sprintf(ipStr, "%s", apIP.toString().c_str());
    drawCentered(Y_OFFSET + 26, ipStr);
    u8g2.sendBuffer();
    delay(3000);
  }
  
  // Root - redirect to the login-protected dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/home");
  });

  // Handle calibration update - login protected
  server.on("/calibrate", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    if (request->hasParam("slope", true) && request->hasParam("offset", true)) {
      float newSlope = request->getParam("slope", true)->value().toFloat();
      float newOffset = request->getParam("offset", true)->value().toFloat();

      saveCalibration(newSlope, newOffset);
      request->redirect("/home");
    } else {
      request->send(400, "text/plain", "Missing parameters");
    }
  });
  
  // API endpoint - live data
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    int adc = analogRead(ADC_PIN);
    float voltage = getCalibratedVoltage(adc);

    String json = "{";
    json += "\"adc\":" + String(adc) + ",";
    json += "\"voltage\":" + String(voltage, 3) + ",";
    json += "\"led\":" + String(digitalRead(LED_PIN) == HIGH ? 1 : 0) + ",";
    json += "\"t\":" + String((unsigned long)time(nullptr));
    json += "}";

    request->send(200, "application/json", json);
  });

  // Dashboard - login protected
  server.on("/home", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    bool ledOn = digitalRead(LED_PIN) == HIGH;

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>ESP32 Dashboard</title>";
    html += "<style>";
    html += "body{font-family:Arial;margin:20px;background:#f0f0f0;}";
    html += ".container{max-width:400px;margin:auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
    html += "h1{color:#333;text-align:center;}";
    html += ".section{margin:20px 0;padding:15px;background:#f9f9f9;border-radius:5px;text-align:center;}";
    html += ".current{color:#666;font-size:18px;margin:5px 0;}";
    html += "button{padding:12px 20px;border:none;border-radius:5px;cursor:pointer;width:100%;margin-top:10px;font-size:16px;color:white;}";
    html += ".btn-row{display:flex;gap:10px;}";
    html += ".btn-on{background:#4CAF50;}";
    html += ".btn-off{background:#f44336;}";
    html += ".btn-auto{background:#2196F3;}";
    html += "label{display:block;margin:10px 0 5px;font-weight:bold;text-align:left;}";
    html += "input{width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}";
    html += ".info-btn{display:inline-flex;align-items:center;justify-content:center;width:20px;height:20px;border-radius:50%;background:#2196F3;color:white;font-size:13px;font-weight:bold;cursor:pointer;margin-left:6px;vertical-align:middle;}";
    html += ".modal-overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:100;}";
    html += ".modal-overlay.open{display:flex;align-items:center;justify-content:center;}";
    html += ".modal-box{background:white;max-width:340px;width:90%;max-height:80vh;overflow-y:auto;padding:20px;border-radius:10px;text-align:left;}";
    html += ".modal-box h3{margin-top:0;}";
    html += ".modal-box ol{padding-left:20px;margin:10px 0;}";
    html += ".modal-box li{margin:8px 0;font-size:14px;color:#444;}";
    html += ".modal-box code{background:#f0f0f0;padding:1px 4px;border-radius:3px;}";
    html += ".modal-close{background:#2196F3;margin-top:10px;}";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>Dashboard</h1>";

    html += "<div class='section'>";
    html += "<div class='current'>Voltage: <span id='voltage'>-</span> V</div>";
    html += "</div>";

    html += "<div class='section'>";
    html += "<div class='current'>LED: <span id='ledstate'>" + String(ledOn ? "ON" : "OFF") + "</span></div>";
    html += "<div class='current' style='font-size:14px;'>Mode: " + String(ledOverride ? "Manual" : "Auto (schedule)") + "</div>";
    html += "<div class='btn-row'>";
    html += "<form action='/led/on' method='POST'><button type='submit' class='btn-on'>Turn ON</button></form>";
    html += "<form action='/led/off' method='POST'><button type='submit' class='btn-off'>Turn OFF</button></form>";
    html += "</div>";
    html += "<form action='/led/auto' method='POST'><button type='submit' class='btn-auto'>Resume Auto Schedule</button></form>";
    html += "</div>";

    char onTimeStr[6], offTimeStr[6];
    sprintf(onTimeStr, "%02d:%02d", ledOnHour, ledOnMinute);
    sprintf(offTimeStr, "%02d:%02d", ledOffHour, ledOffMinute);

    html += "<div class='section'>";
    html += "<h3>LED Schedule</h3>";
    html += "<form action='/led/schedule' method='POST'>";
    html += "<label>Turn ON at:</label>";
    html += "<input type='time' name='on' value='" + String(onTimeStr) + "' required>";
    html += "<label>Turn OFF at:</label>";
    html += "<input type='time' name='off' value='" + String(offTimeStr) + "' required>";
    html += "<button type='submit' class='btn-auto'>Save Schedule</button>";
    html += "</form>";
    html += "</div>";

    html += "<div class='section'>";
    html += "<h3>🔧 ADC Calibration <span class='info-btn' onclick=\"document.getElementById('calInfo').classList.add('open')\">?</span></h3>";
    html += "<div class='current' style='font-size:14px;'>Slope: " + String(ADC_SLOPE, 8) + "</div>";
    html += "<div class='current' style='font-size:14px;'>Offset: " + String(ADC_OFFSET, 4) + "</div>";
    html += "<div class='current' style='font-size:14px;'>Formula: V = (Slope × ADC) + Offset</div>";
    html += "<div class='current' style='font-size:14px;'>ADC Value: <span id='adc'>-</span></div>";
    html += "<form action='/calibrate' method='POST'>";
    html += "<label>Slope (V per ADC unit):</label>";
    html += "<input type='text' name='slope' value='" + String(ADC_SLOPE, 8) + "' required>";
    html += "<label>Offset (V):</label>";
    html += "<input type='text' name='offset' value='" + String(ADC_OFFSET, 4) + "' required>";
    html += "<button type='submit' class='btn-auto'>Save Calibration</button>";
    html += "</form>";
    html += "</div>";

    html += "<div class='section'>";
    html += "<h3>Node-RED</h3>";
    html += "<form action='/nodered/url' method='POST'>";
    html += "<label>Node-RED URL:</label>";
    html += "<input type='text' name='url' value='" + noderedUrl + "' placeholder='http://192.168.1.10:1880/endpoint'>";
    html += "<button type='submit' class='btn-auto'>Save URL</button>";
    html += "</form>";
    html += "</div>";

    html += "<div class='modal-overlay' id='calInfo'>";
    html += "<div class='modal-box'>";
    html += "<h3>How to Calibrate</h3>";
    html += "<ol>";
    html += "<li>Measure the actual battery voltage with a multimeter. Call it <code>V1</code>.</li>";
    html += "<li>Note the current <b>ADC Value</b> shown above at the same moment. Call it <code>ADC1</code>.</li>";
    html += "<li>Let the battery charge or discharge to a different level, then repeat step 1-2 to get a second pair, <code>V2</code> and <code>ADC2</code>. The further apart the two readings, the more accurate the result.</li>";
    html += "<li>Compute:<br><code>slope = (V2-V1) / (ADC2-ADC1)</code><br><code>offset = V1 - slope * ADC1</code></li>";
    html += "<li>Enter the computed slope and offset into the fields above and tap <b>Save Calibration</b>.</li>";
    html += "<li>Check the live Voltage reading against the multimeter to confirm it matches.</li>";
    html += "</ol>";
    html += "<button class='modal-close' onclick=\"document.getElementById('calInfo').classList.remove('open')\">Close</button>";
    html += "</div></div>";

    html += "</div>";
    html += "<script>";
    html += "setInterval(function(){";
    html += "fetch('/data').then(function(r){return r.json();}).then(function(d){";
    html += "document.getElementById('voltage').innerText=d.voltage.toFixed(3);";
    html += "document.getElementById('adc').innerText=d.adc;";
    html += "});";
    html += "},1000);";
    html += "</script>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Manual LED on - login protected
  server.on("/led/on", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    ledOverride = true;
    ledManualState = true;
    setLed(true);

    request->redirect("/home");
  });

  // Manual LED off - login protected
  server.on("/led/off", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    ledOverride = true;
    ledManualState = false;
    setLed(false);

    request->redirect("/home");
  });

  // Resume automatic schedule - login protected
  server.on("/led/auto", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    ledOverride = false;

    request->redirect("/home");
  });

  // Update LED on/off schedule - login protected
  server.on("/led/schedule", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    if (request->hasParam("on", true) && request->hasParam("off", true)) {
      String onStr = request->getParam("on", true)->value();
      String offStr = request->getParam("off", true)->value();

      int onH, onM, offH, offM;
      if (sscanf(onStr.c_str(), "%d:%d", &onH, &onM) == 2 &&
          sscanf(offStr.c_str(), "%d:%d", &offH, &offM) == 2) {
        saveLedSchedule(onH, onM, offH, offM);
      }
    }

    request->redirect("/home");
  });

  // Update Node-RED URL - login protected
  server.on("/nodered/url", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    if (request->hasParam("url", true)) {
      saveNoderedConfig(request->getParam("url", true)->value());
    }

    request->redirect("/home");
  });

  Serial.println("\n=== Starting Web Server ===");
  server.begin();
  Serial.println("Web server started on port 80!");
  Serial.println("\nAccess the dashboard at:");
  if (apFallbackMode) {
    Serial.println("  AP fallback: http://192.168.4.1");
    Serial.print("  Connect to WiFi: ");
    Serial.println(AP_SSID);
  } else {
    Serial.print("  Station: http://");
    Serial.println(staIP);
    Serial.println("  mDNS: http://esp32.local");
  }
  Serial.println("==============================\n");
}

void loop() {
  static uint32_t counter = 0;
  
  delay(1000);
  counter++;
  
  // Read ADC
  int adcValue = analogRead(ADC_PIN);
  float voltage = getCalibratedVoltage(adcValue);
  
  // Get time
  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo);
  
  // LED Logic: configurable on/off schedule (skipped once manually overridden from /home)
  if (ledOverride) {
    setLed(ledManualState);
  } else if (timeValid) {
    setLed(isLedScheduleOn(timeinfo));
  }

  // Periodically disable WiFi, take an isolated reading, and push it to Node-RED.
  // Skipped in AP-fallback mode since there is no internet path to reach Node-RED.
  if (!apFallbackMode && (counter % VOLTAGE_COLLECT_INTERVAL_SEC == 0)) {
    collectAndSendVoltage();
  }

  // Update OLED
  char buf[20];

  // Cycle every 11s: IP screen for 3s, voltage screen for 3s, display off for 5s
  const int IP_SCREEN_SEC = 3;
  const int VOLTAGE_SCREEN_SEC = 3;
  const int DISPLAY_OFF_SEC = 5;
  const int CYCLE_SEC = IP_SCREEN_SEC + VOLTAGE_SCREEN_SEC + DISPLAY_OFF_SEC;

  int phase = counter % CYCLE_SEC;

  if (phase < IP_SCREEN_SEC) {
    u8g2.setPowerSave(0);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    drawCentered(Y_OFFSET + 14, "IP:");
    u8g2.setFont(u8g2_font_5x8_tr);
    if (staConnected) {
      sprintf(buf, "%s", staIP.toString().c_str());
    } else {
      sprintf(buf, "%s", apIP.toString().c_str());
    }
    drawCentered(Y_OFFSET + 28, buf);
    u8g2.sendBuffer();
  } else if (phase < IP_SCREEN_SEC + VOLTAGE_SCREEN_SEC) {
    u8g2.setPowerSave(0);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    if (timeValid) {
      strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    } else {
      sprintf(buf, "Syncing...");
    }
    drawCentered(Y_OFFSET + 14, buf);
    u8g2.setFont(u8g2_font_7x13_tr);
    sprintf(buf, "V: %.3f V", voltage);
    drawCentered(Y_OFFSET + 32, buf);
    u8g2.sendBuffer();
  } else {
    u8g2.setPowerSave(1);
  }
  
  // Serial output
  Serial.print("Counter: ");
  Serial.print(counter);
  if (timeValid) {
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    Serial.print(" | Time: ");
    Serial.print(timeStr);
  }
  Serial.print(" | ADC: ");
  Serial.print(adcValue);
  Serial.print(" | Voltage: ");
  Serial.print(voltage, 3);
  Serial.print("V | LED: ");
  Serial.println(digitalRead(LED_PIN) == HIGH ? "ON" : "OFF");
}
