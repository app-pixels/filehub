/*
 * filehub.ino — standalone wireless file hub
 * Waveshare ESP32-S3-Touch-AMOLED-1.8
 */
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include "Arduino_GFX_Library.h"
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "XPowersLib.h"
#include "app_common.h"
#include "app_filehub.h"
#include "TouchDrvFT6X36.hpp"


Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_SH8601 *gfx = new Arduino_SH8601(
  bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *g_canvas = nullptr;
XPowersPMU power;
TouchDrvFT6X36 touch;

void setup() {
  USBSerial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL))
    USBSerial.println("AXP2101 not found");
  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.clearIrqStatus();
  power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  common_init();

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (SD_MMC.begin("/sdcard", true)) {
    File f = SD_MMC.open("/setup/setup.txt");
    if (f) {
      char line[160];
      while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line)-1); line[len]='\0';
        const char *p;
        if ((p = strstr(line,"BRIGHTNESS"))) { p+=10; while(*p==' '||*p=='=')p++; int v=atoi(p); if(v>0&&v<=255) g_config.brightness=(uint16_t)v; }
        if ((p = strstr(line,"TIMEOUT")))    { p+=7;  while(*p==' '||*p=='=')p++; int v=atoi(p); if(v>=0) g_config.timeout_s=(uint32_t)v; }
      }
      f.close();
    }
    SD_MMC.end();
  }

  if (!touch.begin(Wire, FT6X36_SLAVE_ADDRESS, IIC_SDA, IIC_SCL))
    USBSerial.println("FT6X36 init failed");

  g_canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, gfx, 0, 0, 0);
  if (!g_canvas->begin()) USBSerial.println("canvas begin failed");
  gfx->setBrightness(g_config.brightness);
  app_filehub_setup(gfx);
}

void loop() { app_filehub_loop(); }
