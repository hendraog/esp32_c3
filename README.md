# ESP32-C3 OLED Display Project

PlatformIO project for ESP32-C3 with SSD1306 OLED display support.

## Hardware Requirements

- ESP32-C3 Development Board
- SSD1306 OLED Display (128x64, I2C)
- Connecting wires

## Wiring

Default I2C pins (adjust in code if different):
- SDA: GPIO 8
- SCL: GPIO 9
- VCC: 3.3V
- GND: GND

## Getting Started

1. Open this project in VS Code with PlatformIO IDE extension
2. Connect your ESP32-C3 board via USB
3. Click "Build" to compile the project
4. Click "Upload" to flash to your board
5. Open Serial Monitor (115200 baud) to see debug output

## Features

- SSD1306 OLED display initialization
- Simple counter demo updating every second
- Serial debug output
- Graphics and text rendering examples

## Libraries Used

- Adafruit SSD1306
- Adafruit GFX Library
- Adafruit BusIO

## Customization

- Adjust I2C pins in `src/main.cpp` if using different GPIO pins
- Change OLED I2C address if your display uses 0x3D instead of 0x3C
- Modify display content in the `loop()` function
