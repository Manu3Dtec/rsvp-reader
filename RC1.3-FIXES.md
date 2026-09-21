# RC1.3 fixes

- ESP-IDF 6.0 / C++ compatibility: use `GPIO_NUM_NC` instead of integer `-1` for `dc_gpio_num` and `reset_gpio_num` in the 3.49B V2 BSP.
- Retains RC1.2 `esp_timer` CMake dependency fix.
- Retains RC1.1 AXS15231B 2.x driver fix.
