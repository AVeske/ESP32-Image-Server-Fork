// LovyanGFX display config for the WT32-SC01 (V3.2): a 320x480 ST7796 SPI panel
// with an FT5x06 capacitive touch and a PWM backlight, on a classic ESP32.
//
// Pins/driver are taken from the reference project:
//   https://github.com/luckyluckhcccp/wt32-sc01-v3.2-LVGL8-lovyan-gfx
// (ekranayarlari.h). Trimmed to what this image server needs.
//
// NOTE: this targets the 3.5" 320x480 ST7796 board the reference describes. If
// your unit is a different panel (e.g. an 800x480 5" RGB), swap _panel_instance
// for the matching lgfx::Panel_* and update PANEL_W/PANEL_H below.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static const int PANEL_W = 320;
static const int PANEL_H = 480;

// SPI display pins
#define WT32_MOSI 13
#define WT32_MISO -1
#define WT32_SCLK 14
#define WT32_DC   21
#define WT32_CS   15
#define WT32_RST  22
#define WT32_BL   23

// FT5x06 capacitive touch (I2C)
#define WT32_TOUCH_INT 39
#define WT32_TOUCH_SDA 18
#define WT32_TOUCH_SCL 19

class LGFX_WT32 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Touch_FT5x06  _touch;
  lgfx::Light_PWM     _light;

 public:
  LGFX_WT32() {
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;   // 40 MHz: conservative, stable on long flex
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = true;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = WT32_SCLK;
      cfg.pin_mosi = WT32_MOSI;
      cfg.pin_miso = WT32_MISO;
      cfg.pin_dc   = WT32_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = WT32_CS;
      cfg.pin_rst  = WT32_RST;
      cfg.pin_busy = -1;
      cfg.panel_width  = PANEL_W;
      cfg.panel_height = PANEL_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable  = false;
      cfg.invert    = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = WT32_BL;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0; cfg.x_max = PANEL_W - 1;
      cfg.y_min = 0; cfg.y_max = PANEL_H - 1;
      cfg.pin_int = WT32_TOUCH_INT;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda  = WT32_TOUCH_SDA;
      cfg.pin_scl  = WT32_TOUCH_SCL;
      cfg.freq     = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
