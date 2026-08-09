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
#define LED_PIN2 8 // Mirrors LED_PIN state

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

// LED on/off schedule (time-of-day, stored in flash)
int ledOnHour = 19;
int ledOnMinute = 0;
int ledOffHour = 5;
int ledOffMinute = 0;

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // UTC +7
const int   daylightOffset_sec = 0;

// Voltage history: sampled every 5 minutes, kept for the last 6 hours (circular buffer)
const int VOLTAGE_SAMPLE_INTERVAL_SEC = 5 * 60;
const int VOLTAGE_HISTORY_SIZE = (6 * 60 * 60) / VOLTAGE_SAMPLE_INTERVAL_SEC; // 72 samples
float voltageHistory[VOLTAGE_HISTORY_SIZE];
int32_t voltageHistoryEpoch[VOLTAGE_HISTORY_SIZE];
int voltageHistoryCount = 0;
int voltageHistoryHead = 0;
time_t lastVoltageSampleTime = 0;

// Drive LED_PIN and LED_PIN2 together so they always match
void setLed(bool state) {
  digitalWrite(LED_PIN, state ? HIGH : LOW);
  digitalWrite(LED_PIN2, state ? HIGH : LOW);
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

// Append a voltage sample to the circular history buffer (kept in RAM only)
void recordVoltageSample(float voltage, time_t sampleTime) {
  voltageHistory[voltageHistoryHead] = voltage;
  voltageHistoryEpoch[voltageHistoryHead] = (int32_t)sampleTime;
  voltageHistoryHead = (voltageHistoryHead + 1) % VOLTAGE_HISTORY_SIZE;
  if (voltageHistoryCount < VOLTAGE_HISTORY_SIZE) voltageHistoryCount++;
  lastVoltageSampleTime = sampleTime;
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
    u8g2.setFont(u8g2_font_6x10_tr);
    drawCentered(Y_OFFSET + 9, "Connected!");
    u8g2.setFont(u8g2_font_4x6_tr);
    char ipStr[20];
    sprintf(ipStr, "STA:%s", WiFi.localIP().toString().c_str());
    drawCentered(Y_OFFSET + 20, ipStr);
    sprintf(ipStr, "AP:%s", apIP.toString().c_str());
    drawCentered(Y_OFFSET + 30, ipStr);
    u8g2.sendBuffer();
    
    // Sync time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("NTP Configured");
    
    delay(3000);
  } else {
    Serial.println("WiFi failed, AP mode only");
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
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>Dashboard</h1>";

    html += "<div class='section'>";
    html += "<div class='current'>Voltage: <span id='voltage'>-</span> V</div>";
    html += "</div>";

    html += "<div class='section'>";
    html += "<h3>Voltage (last 6 hours)</h3>";
    html += "<canvas id='voltageChart' width='340' height='160' style='width:100%;height:auto;background:#fff;border-radius:5px;'></canvas>";
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

    html += "</div>";
    html += "<script>";
    html += "var historyData=[],liveVoltage=null,liveEpoch=null;";

    html += "function withLive(hist){";
    html += "if(liveEpoch&&liveEpoch>1000000000&&liveVoltage!==null){return hist.concat([{t:liveEpoch,v:liveVoltage}]);}";
    html += "return hist;";
    html += "}";

    html += "function drawGraph(data){";
    html += "var c=document.getElementById('voltageChart'),ctx=c.getContext('2d'),w=c.width,h=c.height,pad=32;";
    html += "ctx.clearRect(0,0,w,h);";
    html += "var nowT=(liveEpoch&&liveEpoch>1000000000)?liveEpoch:(data.length?data[data.length-1].t:null);";
    html += "if(nowT===null){ctx.fillStyle='#999';ctx.font='12px Arial';ctx.fillText('Collecting data...',10,h/2);return;}";
    html += "var SIX_HOURS=6*60*60,tMax=nowT,tMin=tMax-SIX_HOURS,tRange=SIX_HOURS;";
    html += "ctx.strokeStyle='#ccc';ctx.beginPath();";
    html += "ctx.moveTo(pad,8);ctx.lineTo(pad,h-pad);ctx.lineTo(w-6,h-pad);ctx.stroke();";
    html += "function fmtT(t){var d=new Date(t*1000);var hh=d.getHours().toString().padStart(2,'0');var mm=d.getMinutes().toString().padStart(2,'0');return hh+':'+mm;}";
    html += "ctx.fillStyle='#666';ctx.font='10px Arial';";
    html += "ctx.fillText(fmtT(tMin),pad,h-6);";
    html += "ctx.fillText(fmtT(tMax),w-40,h-6);";
    html += "if(data.length===0){return;}";
    html += "var vals=data.map(function(d){return d.v;});";
    html += "var minV=Math.min.apply(null,vals),maxV=Math.max.apply(null,vals);";
    html += "if(minV===maxV){minV-=0.5;maxV+=0.5;}";
    html += "ctx.fillText(maxV.toFixed(2)+'V',2,14);";
    html += "ctx.fillText(minV.toFixed(2)+'V',2,h-pad+4);";
    html += "ctx.strokeStyle='#2196F3';ctx.lineWidth=2;ctx.beginPath();";
    html += "data.forEach(function(d,i){";
    html += "var x=pad+((d.t-tMin)/tRange)*(w-pad-10);";
    html += "var y=(h-pad)-((d.v-minV)/(maxV-minV))*(h-pad-10);";
    html += "if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);";
    html += "});ctx.stroke();";
    html += "var last=data[data.length-1];";
    html += "var lx=pad+((last.t-tMin)/tRange)*(w-pad-10);";
    html += "var ly=(h-pad)-((last.v-minV)/(maxV-minV))*(h-pad-10);";
    html += "ctx.fillStyle='#2196F3';ctx.beginPath();ctx.arc(lx,ly,3,0,2*Math.PI);ctx.fill();";
    html += "}";

    html += "function loadGraph(){fetch('/graph/data').then(function(r){return r.json();}).then(function(hist){historyData=hist;drawGraph(withLive(historyData));});}";

    html += "setInterval(function(){";
    html += "fetch('/data').then(function(r){return r.json();}).then(function(d){";
    html += "document.getElementById('voltage').innerText=d.voltage.toFixed(3);";
    html += "liveVoltage=d.voltage;liveEpoch=d.t;";
    html += "drawGraph(withLive(historyData));";
    html += "});";
    html += "},1000);";

    html += "loadGraph();setInterval(loadGraph,60000);";

    html += "</script>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Voltage history - login protected
  server.on("/graph/data", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(DASHBOARD_USER, DASHBOARD_PASS)) {
      return request->requestAuthentication();
    }

    String json = "[";
    int startIndex = (voltageHistoryCount < VOLTAGE_HISTORY_SIZE) ? 0 : voltageHistoryHead;
    for (int i = 0; i < voltageHistoryCount; i++) {
      int idx = (startIndex + i) % VOLTAGE_HISTORY_SIZE;
      if (i > 0) json += ",";
      json += "{\"t\":" + String(voltageHistoryEpoch[idx]) + ",\"v\":" + String(voltageHistory[idx], 3) + "}";
    }
    json += "]";

    request->send(200, "application/json", json);
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
  
  // LED Logic: configurable on/off schedule (skipped once manually overridden from /home)
  if (ledOverride) {
    setLed(ledManualState);
  } else if (timeValid) {
    setLed(isLedScheduleOn(timeinfo));
  }

  // Voltage history: sample every 5 minutes once time is synced
  if (timeValid) {
    time_t now = time(nullptr);
    if (lastVoltageSampleTime == 0 || (now - lastVoltageSampleTime) >= VOLTAGE_SAMPLE_INTERVAL_SEC) {
      recordVoltageSample(voltage, now);
    }
  }

  // Update OLED
  u8g2.clearBuffer();

  char buf[20];

  // Cycle every 10s: show IP screen for 3s, then time/voltage for 7s
  bool showIpScreen = (counter % 10) < 5;

  if (showIpScreen) {
    u8g2.setFont(u8g2_font_6x10_tr);
    drawCentered(Y_OFFSET + 14, "IP:");
    u8g2.setFont(u8g2_font_5x8_tr);
    if (staConnected) {
      sprintf(buf, "%s", staIP.toString().c_str());
    } else {
      sprintf(buf, "%s", apIP.toString().c_str());
    }
    drawCentered(Y_OFFSET + 28, buf);
  } else {
    // Display Time (Large & Centered)
    if (timeValid) {
      u8g2.setFont(u8g2_font_helvB12_tr);
      strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
      drawCentered(Y_OFFSET + 18, buf);
    } else {
      u8g2.setFont(u8g2_font_7x13_tr);
      drawCentered(Y_OFFSET + 18, "Syncing...");
    }

    // Display Voltage (Centered below)
    u8g2.setFont(u8g2_font_7x13_tr);
    sprintf(buf, "V: %.3f V", voltage);
    drawCentered(Y_OFFSET + 34, buf);
  }

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
