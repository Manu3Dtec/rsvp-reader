#include "rtc_clock.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

namespace RtcClock {
namespace {

constexpr uint8_t kAddress = 0x51;   // PCF85063A
constexpr uint32_t kI2cSpeedHz = 400000;
constexpr int kTimeoutMs = 100;

constexpr uint8_t kRegSeconds = 0x04;

const char *TAG = "RTC";
i2c_master_dev_handle_t g_rtc = nullptr;

uint8_t from_bcd(uint8_t v) {
    return static_cast<uint8_t>(((v >> 4) * 10) + (v & 0x0F));
}

uint8_t to_bcd(int v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

bool sane(const Time &t) {
    return t.year >= 2024 && t.year <= 2099 &&
           t.month >= 1 && t.month <= 12 &&
           t.day >= 1 && t.day <= 31 &&
           t.weekday >= 0 && t.weekday <= 6 &&
           t.hour >= 0 && t.hour <= 23 &&
           t.minute >= 0 && t.minute <= 59 &&
           t.second >= 0 && t.second <= 59;
}

}  // namespace

bool start() {
    if (g_rtc) return true;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "BSP I2C bus unavailable");
        return false;
    }

    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kAddress;
    cfg.scl_speed_hz = kI2cSpeedHz;

    const esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &g_rtc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A add-device failed: %s", esp_err_to_name(err));
        g_rtc = nullptr;
        return false;
    }

    const Time t = now();
    ESP_LOGI(TAG, "PCF85063A ready: valid=%d %02d:%02d",
             t.valid, t.hour, t.minute);
    return true;
}

Time now() {
    Time t;
    if (!g_rtc) return t;

    uint8_t reg = kRegSeconds;
    uint8_t data[7] = {};
    if (i2c_master_transmit_receive(g_rtc, &reg, 1, data, sizeof(data), kTimeoutMs) != ESP_OK) {
        return t;
    }

    // Seconds bit 7 is the oscillator-stop flag. If set, stored time is not reliable.
    if (data[0] & 0x80u) return t;

    t.second = from_bcd(data[0] & 0x7F);
    t.minute = from_bcd(data[1] & 0x7F);
    t.hour = from_bcd(data[2] & 0x3F);
    t.day = from_bcd(data[3] & 0x3F);
    t.weekday = data[4] & 0x07;
    t.month = from_bcd(data[5] & 0x1F);
    t.year = 2000 + from_bcd(data[6]);
    t.valid = sane(t);
    return t;
}

bool set_local(int year, int month, int day, int weekday,
               int hour, int minute, int second) {
    if (!g_rtc) return false;

    Time t;
    t.valid = true;
    t.year = year;
    t.month = month;
    t.day = day;
    t.weekday = weekday;
    t.hour = hour;
    t.minute = minute;
    t.second = second;
    if (!sane(t)) return false;

    uint8_t data[8] = {
        kRegSeconds,
        to_bcd(second),
        to_bcd(minute),
        to_bcd(hour),
        to_bcd(day),
        static_cast<uint8_t>(weekday & 0x07),
        to_bcd(month),
        to_bcd(year % 100),
    };

    const esp_err_t err = i2c_master_transmit(g_rtc, data, sizeof(data), kTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC set failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "RTC synchronized: %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return true;
}

}  // namespace RtcClock
