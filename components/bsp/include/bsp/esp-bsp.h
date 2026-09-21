#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_SD_MOUNT_POINT "/sdcard"

lv_display_t *bsp_display_start(void);
lv_indev_t *bsp_display_get_input_dev(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(uint8_t percent);
esp_err_t bsp_display_sleep(bool sleep);

esp_err_t bsp_sdcard_mount(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

// ESP32-S3 Touch-LCD-3.49B V2 board controls verified against Waveshare V2 examples.
// PWR is GPIO16, active low. SYS_EN is TCA9554 bit 6.
bool bsp_pwr_button_pressed(void);
esp_err_t bsp_power_off(void);

#ifdef __cplusplus
}
#endif
