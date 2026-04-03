#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

// OLED Display Configuration for 0.42" display
// This display is 72x40 pixels but uses SSD1306 with 132x64 buffer
// The visible area is offset: adjusted for actual display boundaries
// Reference: https://electronics.stackexchange.com/questions/725871

#define I2C_SDA 5
#define I2C_SCL 6

// U8g2 constructor for SSD1306 128x64 display with Hardware I2C
// The 72x40 screen is mapped in the middle of the 132x64 pixel buffer
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

const int DISPLAY_WIDTH = 72;
const int DISPLAY_HEIGHT = 40;
const int X_OFFSET = 28; // Adjusted for actual visible area
const int Y_OFFSET = 24; // Adjusted for actual visible area

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32-C3 0.42\" OLED ===");
  Serial.print("I2C: SDA=GPIO");
  Serial.print(I2C_SDA);
  Serial.print(", SCL=GPIO");
  Serial.println(I2C_SCL);
  Serial.println("Display: 72x40 pixels");
  
  u8g2.begin();
  u8g2.setContrast(255);      // Max contrast
  u8g2.setBusClock(400000);   // 400kHz I2C
  
  // Display welcome message
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  
  u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 10, "ESP32-C3");
  u8g2.drawStr(X_OFFSET + 8, Y_OFFSET + 22, "0.42\" OLED");
  u8g2.drawStr(X_OFFSET + 8, Y_OFFSET + 34, "72x40 px");
  
  u8g2.sendBuffer();
  
  Serial.println("Display initialized!");
  delay(2000);
}

void loop() {
  static uint32_t counter = 0;
  
  delay(1000);
  counter++;
  
  // Update display
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  
  // Draw title
  u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 10, "ESP32-C3");
  
  // Draw counter
  char buf[20];
  sprintf(buf, "Cnt:%lu", counter);
  u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 22, buf);
  
  // Draw uptime
  sprintf(buf, "Up:%lus", millis() / 1000);
  u8g2.drawStr(X_OFFSET + 2, Y_OFFSET + 34, buf);
  
  // Draw border (optional - shows display boundaries)
  u8g2.drawFrame(X_OFFSET, Y_OFFSET, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  
  u8g2.sendBuffer();
  
  Serial.print("Counter: ");
  Serial.println(counter);
}
