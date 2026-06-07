// IMPLEMENTATION_NOTES.md - ESP32-S3-Touch-LCD-5 UI Implementation Guide

## Overview
This project implements a complete GUI display system for the Waveshare ESP32-S3-Touch-LCD-5 
(800x480 resolution) using LVGL 8.3.8 and Squareline Studio.

## Hardware
- **Board**: ESP32-S3 (Waveshare Touch LCD 5)
- **Display**: 800x480 RGB LCD with ST7262 controller
- **Touchscreen**: GT911 capacitive touch controller
- **Connectivity**: SPI for display, I2C for touch

## Pin Configuration

### Display (SPI2)
- CS:   GPIO10
- RST:  GPIO8
- DC:   GPIO9
- MOSI: GPIO11
- SCLK: GPIO12
- MISO: GPIO13

### Touch (I2C)
- SDA: GPIO4
- SCL: GPIO5

## Architecture

### Core Files
1. **main.cpp** - Main application entry point
   - Initializes display and touch drivers
   - Sets up LVGL
   - Loads Squareline UI
   - Manages LVGL timer task

2. **lcd_driver.h/c** - LCD display driver
   - SPI communication with ST7262 controller
   - LVGL flush callback
   - Display initialization sequence
   - Window setting (address)

3. **touch_driver.h/c** - Touch input driver
   - I2C communication with GT911
   - LVGL input device callback
   - Touch coordinate reading

4. **lv_conf.h** - LVGL configuration
   - Display dimensions (800x480)
   - Memory settings
   - Widget configuration
   - Input device settings

### UI Files (from Squareline Studio)
Located in include/ folder:
- **ui.h** - Main UI header
- **ui.c** - UI initialization
- **ui_events.h/c** - Event handlers (stubs for implementation)
- **ui_helpers.h/c** - Helper functions for UI manipulation
- **screens/** - Individual screen implementations

## Build and Upload

### Prerequisites
- PlatformIO IDE with ESP32 toolchain
- USB-to-Serial connection to ESP32

### Build Steps
1. Connect your board via USB
2. Hold **Boot** button, press **Reset**, release **Boot** (Flash mode)
3. Click "Build" in PlatformIO
4. Click "Upload" in PlatformIO
5. Release the board

### Monitoring
- Open Serial Monitor at 115200 baud to view debug logs
- Check status messages from LCD, LVGL, and UI initialization

## Configuration Notes

### Display Driver
- SPI Frequency: 80 MHz (configurable via LCD_SPI_FREQ)
- Color Depth: 16-bit RGB565
- Refresh Period: 30ms (configurable via LV_DISP_DEF_REFR_PERIOD)

### Memory Settings
- LVGL Buffer Size: 512KB (LV_MEM_SIZE)
- Display Buffer: 80 lines of 800px each (51.2KB × 2 for double buffering)

### Touch Settings
- I2C Speed: 100 kHz
- Max Touch Points: 5 (GT911 capable)
- Touch Polling: 30ms interval

## Troubleshooting

### Display Not Showing
1. Check PSRAM is enabled: `#define BOARD_HAS_PSRAM`
2. Verify SPI pins are correctly connected
3. Check reset sequence in lcd_driver_init()
4. Monitor serial output for error messages

### Touch Not Working
1. Check I2C pins (GPIO4=SDA, GPIO5=SCL)
2. Verify GT911 I2C address: 0x5D
3. Ensure pull-up resistors on I2C bus (usually built-in on ESP32-S3)
4. Monitor touch data in serial output

### Memory Issues
- Reduce LV_MEM_SIZE if compilation fails
- Adjust display buffer size if needed
- Check PSRAM availability: `Monitor > Device Memory`

## Event Handler Implementation

UI event handlers are stubbed in ui_events.c:
```c
void pimm_select_module1(lv_event_t * e)
{
    // Add your module 1 selection logic here
    lv_obj_t * target = lv_event_get_target(e);
    // ... implementation
}
```

Replace stub implementations with your actual logic.

## Performance Tips

1. **Refresh Rate**: Adjust LV_DISP_DEF_REFR_PERIOD for balance between responsiveness and power consumption
2. **Buffer Size**: Larger buffers = faster refresh, more memory usage
3. **DMA**: SPI DMA is enabled automatically for faster transfers
4. **Task Priority**: LVGL timer task runs at priority 5 on CPU1

## Next Steps

1. ✅ Display driver configured
2. ✅ Touch driver configured
3. ✅ LVGL initialized
4. ✅ UI loaded from Squareline
5. ⏳ Implement event handlers in ui_events.c
6. ⏳ Add your application logic
7. ⏳ Test on hardware and iterate

## Additional Resources

- LVGL Docs: https://docs.lvgl.io/
- ESP32-S3 Docs: https://docs.espressif.com/projects/esp-idf/
- Waveshare Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-5
- Squareline Studio: https://squareline.io/
