idf_component_register(
    SRCS "esp-bsp.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_ledc
        esp_adc
        esp_lcd
        esp_timer
        fatfs
        esp_driver_sdmmc
        lvgl
        esp_lcd_axs15231b
        esp_io_expander_tca9554
)
