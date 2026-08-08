#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
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

// Dashboard login
const char* DASHBOARD_USER = "admin";
const char* DASHBOARD_PASS = "rfw24rfv243rfsd";

// LED manual control (overrides the time-based schedule once used)
bool ledOverride = false;
bool ledManualState = false;

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // UTC +7
const int   daylightOffset_sec = 0;

// Calibrated voltage conversion
float getCalibratedVoltage(int adcValue) {
  return (ADC_SLOPE * adcValue) + ADC_OFFSET;
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32-C3 ADC Web Calibration ===");
  
  // Configure ADC
  pinMode(ADC_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  
  // Load calibration
  loadCalibration();
  
  // Initialize OLED
  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);
  
  // Show connecting message
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13_tr);
  u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 15, "Starting");
  u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 25, "WiFi...");
  u8g2.sendBuffer();
  
  // Start Access Point
  Serial.println("\n=== Starting Access Point ===");
  WiFi.mode(WIFI_AP_STA);  // Both AP and Station mode
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apIP = WiFi.softAPIP();
  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("AP Password: ");
  Serial.println(AP_PASSWORD);
  Serial.print("AP IP: ");
  Serial.println(apIP);
  Serial.println("================================\n");
  
  // Connect to WiFi
  Serial.print("Connecting to: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("Station IP: ");
    Serial.println(WiFi.localIP());
    staConnected = true;
    staIP = WiFi.localIP();

    // Start mDNS
    if (MDNS.begin("esp32")) {
      Serial.println("mDNS started: http://esp32.local");
      MDNS.addService("http", "tcp", 80);
    } else {
      Serial.println("Error starting mDNS");
    }
    
    // Display Station IP
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13_tr);
    u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 10, "Connected!");
    u8g2.setFont(u8g2_font_6x10_tr);
    char ipStr[20];
    sprintf(ipStr, "STA:%s", WiFi.localIP().toString().c_str());
    u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 20, ipStr);
    sprintf(ipStr, "AP:%s", apIP.toString().c_str());
    u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 30, ipStr);
    u8g2.sendBuffer();
    
    // Sync time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("NTP Configured");
    
    delay(3000);
  } else {
    Serial.println("WiFi failed, AP mode only");
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13_tr);
    u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 10, "AP Mode");
    u8g2.setFont(u8g2_font_6x10_tr);
    char ipStr[20];
    sprintf(ipStr, "%s", apIP.toString().c_str());
    u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 23, ipStr);
    u8g2.sendBuffer();
    delay(3000);
  }
  
  // Web server - Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>ESP32 ADC Calibration</title>";
    html += "<style>";
    html += "body{font-family:Arial;margin:20px;background:#f0f0f0;}";
    html += ".container{max-width:600px;margin:auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
    html += "h1{color:#333;text-align:center;}";
    html += ".section{margin:20px 0;padding:15px;background:#f9f9f9;border-radius:5px;}";
    html += "label{display:block;margin:10px 0 5px;font-weight:bold;}";
    html += "input{width:100%;padding:8px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}";
    html += "button{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:5px;cursor:pointer;width:100%;margin-top:10px;}";
    html += "button:hover{background:#45a049;}";
    html += ".info{background:#e3f2fd;padding:10px;border-left:4px solid #2196F3;margin:10px 0;}";
    html += ".current{color:#666;font-size:14px;margin:5px 0;}";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>🔧 ADC Calibration</h1>";
    
    html += "<div class='section'>";
    html += "<h3>Current Settings</h3>";
    html += "<div class='current'>Slope: " + String(ADC_SLOPE, 8) + "</div>";
    html += "<div class='current'>Offset: " + String(ADC_OFFSET, 4) + "</div>";
    html += "<div class='current'>Formula: V = (Slope × ADC) + Offset</div>";
    html += "</div>";
    
    html += "<div class='section'>";
    html += "<h3>Live Reading</h3>";
    html += "<div class='current'>ADC Value: <span id='adc'>-</span></div>";
    html += "<div class='current'>Voltage: <span id='voltage'>-</span> V</div>";
    html += "</div>";
    
    html += "<div class='section'>";
    html += "<h3>Update Calibration</h3>";
    html += "<form action='/calibrate' method='POST'>";
    html += "<label>Slope (V per ADC unit):</label>";
    html += "<input type='text' name='slope' value='" + String(ADC_SLOPE, 8) + "' required>";
    html += "<label>Offset (V):</label>";
    html += "<input type='text' name='offset' value='" + String(ADC_OFFSET, 4) + "' required>";
    html += "<button type='submit'>Save Calibration</button>";
    html += "</form>";
    html += "</div>";
    
    html += "<div class='info'>";
    html += "<strong>How to calibrate:</strong><br>";
    html += "1. Apply a known voltage (e.g., 1.0V)<br>";
    html += "2. Note the ADC reading<br>";
    html += "3. Repeat with 2-3 different voltages<br>";
    html += "4. Calculate slope = ΔV / ΔADC<br>";
    html += "5. Calculate offset: V = slope×ADC + offset";
    html += "</div>";
    
    html += "</div>";
    html += "<script>";
    html += "setInterval(function(){";
    html += "fetch('/data').then(r=>r.json()).then(d=>{";
    html += "document.getElementById('adc').innerText=d.adc;";
    html += "document.getElementById('voltage').innerText=d.voltage.toFixed(3);";
    html += "});";
    html += "},1000);";
    html += "</script>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });
  
  // Handle calibration update
  server.on("/calibrate", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("slope", true) && request->hasParam("offset", true)) {
      float newSlope = request->getParam("slope", true)->value().toFloat();
      float newOffset = request->getParam("offset", true)->value().toFloat();
      
      saveCalibration(newSlope, newOffset);
      request->redirect("/");
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
    json += "\"led\":" + String(digitalRead(LED_PIN) == HIGH ? 1 : 0);
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
    html += ".on{background:#f44336;}";
    html += ".off{background:#4CAF50;}";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>Dashboard</h1>";

    html += "<div class='section'>";
    html += "<div class='current'>Voltage: <span id='voltage'>-</span> V</div>";
    html += "</div>";

    html += "<div class='section'>";
    html += "<div class='current'>LED: <span id='ledstate'>" + String(ledOn ? "ON" : "OFF") + "</span></div>";
    html += "<form action='/led/toggle' method='POST'>";
    html += "<button type='submit' class='" + String(ledOn ? "on" : "off") + "'>Turn LED " + String(ledOn ? "OFF" : "ON") + "</button>";
    html += "</form>";
    html += "</div>";

    html += "</div>";
    html += "<script>";
    html += "setInterval(function(){";
    html += "fetch('/data').then(r=>r.json()).then(d=>{";
    html += "document.getElementById('voltage').innerText=d.voltage.toFixed(3);";
    html += "});";
    html += "},1000);";
    html += "</script>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Manual LED toggle - login protected
  server.on("/led/toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    ledOverride = true;
    ledManualState = !(digitalRead(LED_PIN) == HIGH);
    digitalWrite(LED_PIN, ledManualState ? HIGH : LOW);

    request->redirect("/home");
  });

  Serial.println("\n=== Starting Web Server ===");
  server.begin();
  Serial.println("Web server started on port 80!");
  Serial.println("\nAccess the calibration page at:");
  Serial.println("  Option 1 (Direct AP): http://192.168.4.1");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("  Option 2 (Station): http://");
    Serial.println(WiFi.localIP());
    Serial.println("  Option 3 (mDNS): http://esp32.local");
  }
  Serial.println("\n** Connect your device to WiFi: ESP32-ADC **");
  Serial.println("** Password: 12345678 **");
  Serial.println("** Then open: http://192.168.4.1 **");
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
  
  // LED Logic: 19:00 to 05:00 (skipped once manually overridden from /home)
  if (ledOverride) {
    digitalWrite(LED_PIN, ledManualState ? HIGH : LOW);
  } else if (timeValid) {
    if (timeinfo.tm_hour >= 19 || timeinfo.tm_hour < 5) {
      digitalWrite(LED_PIN, HIGH);
    } else {
      digitalWrite(LED_PIN, LOW);
    }
  }

  // Update OLED
  u8g2.clearBuffer();

  char buf[20];

  // Cycle every 10s: show IP screen for 3s, then time/voltage for 7s
  bool showIpScreen = (counter % 10) < 3;

  if (showIpScreen) {
    u8g2.setFont(u8g2_font_6x10_tr);
    if (staConnected) {
      sprintf(buf, "%s", staIP.toString().c_str());
    } else {
      sprintf(buf, "%s", apIP.toString().c_str());
    }
    u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 22, buf);
  } else {
    // Display Time (Large & Centered)
    if (timeValid) {
      u8g2.setFont(u8g2_font_helvB12_tr);
      strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
      u8g2.drawStr(X_OFFSET + 4, Y_OFFSET + 18, buf);
    } else {
      u8g2.setFont(u8g2_font_7x13_tr);
      u8g2.drawStr(X_OFFSET + 5, Y_OFFSET + 18, "Syncing...");
    }

    // Display Voltage (Centered below)
    u8g2.setFont(u8g2_font_7x13_tr);
    sprintf(buf, "V: %.3f V", voltage);
    u8g2.drawStr(X_OFFSET + 8, Y_OFFSET + 34, buf);
  }

  u8g2.drawFrame(X_OFFSET, Y_OFFSET, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  u8g2.sendBuffer();
  
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
