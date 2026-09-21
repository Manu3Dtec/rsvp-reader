#include "battery_monitor.h"

#include <algorithm>
#include <atomic>

#include "bsp/esp-bsp.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace BatteryMonitor {
namespace {
constexpr const char *TAG = "BAT_PWR_349";
constexpr adc_unit_t kAdcUnit = ADC_UNIT_1;
constexpr adc_channel_t kBatteryChannel = ADC_CHANNEL_3; // GPIO4 on ESP32-S3
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;
constexpr adc_bitwidth_t kWidth = ADC_BITWIDTH_12;

adc_oneshot_unit_handle_t g_adc = nullptr;
adc_cali_handle_t g_cali = nullptr;
std::atomic<bool> g_available{false};
std::atomic<bool> g_present{false};
std::atomic<uint8_t> g_percent{0};
std::atomic<bool> g_poweroff_requested{false};
std::atomic<bool> g_home_requested{false};
std::atomic<bool> g_ready{false};

uint8_t percent_from_voltage(float v) {
    // Conservative Li-ion display estimate. ADC acquisition itself follows Waveshare's
    // documented *3 divider compensation exactly; this curve is UI-only.
    struct P { float v; uint8_t p; };
    static constexpr P curve[] = {
        {3.30f, 0}, {3.50f, 8}, {3.60f, 18}, {3.70f, 35},
        {3.80f, 55}, {3.90f, 72}, {4.00f, 86}, {4.10f, 95}, {4.20f, 100}
    };
    if (v <= curve[0].v) return 0;
    if (v >= curve[8].v) return 100;
    constexpr size_t curve_count = sizeof(curve) / sizeof(curve[0]);
    for (size_t i = 1; i < curve_count; ++i) {
        if (v <= curve[i].v) {
            const float f = (v - curve[i-1].v) / (curve[i].v - curve[i-1].v);
            return static_cast<uint8_t>(curve[i-1].p + f * (curve[i].p - curve[i-1].p));
        }
    }
    return 0;
}

void sample_battery() {
    if (!g_adc || !g_cali) return;
    int raw = 0;
    int mv = 0;
    if (adc_oneshot_read(g_adc, kBatteryChannel, &raw) != ESP_OK) return;
    if (adc_cali_raw_to_voltage(g_cali, raw, &mv) != ESP_OK) return;
    const float volts = static_cast<float>(mv) * 0.001f * 3.0f; // Waveshare V2 reference factor
    const bool present = volts > 2.7f;
    g_present.store(present, std::memory_order_relaxed);
    g_percent.store(present ? percent_from_voltage(volts) : 0, std::memory_order_relaxed);
    g_available.store(true, std::memory_order_relaxed);
}

void monitor_task(void *) {
    bool armed = false, active = false, sent = false;
    bool boot_down = false;
    int64_t pressed_at = 0, boot_at = 0;
    uint32_t battery_ticks = 0;
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    while (true) {
        const int64_t now = esp_timer_get_time();
        const bool down = bsp_pwr_button_pressed();
        // A power-on hold must never become a power-off request.
        if (!down) armed = true;
        if (armed && down && !active) { active = true; sent = false; pressed_at = now; }
        if (active && down && !sent && now - pressed_at >= 2000000) {
            sent = true;
            g_poweroff_requested.store(true, std::memory_order_release);
        }
        if (!down) active = false;
        const bool boot = gpio_get_level(GPIO_NUM_0) == 0;
        static int last_pwr = -1, last_boot = -1;
        static int64_t last_key_log = 0;
        if (last_pwr != down || last_boot != boot || now - last_key_log > 5000000) {
            last_pwr = down; last_boot = boot; last_key_log = now;
            ESP_LOGI(TAG, "keys PWR=%d BOOT=%d armed=%d off_queued=%d", down, boot, armed, sent);
        }
        if (boot && !boot_down) boot_at = now;
        if (!boot && boot_down && now - boot_at >= 40000 && now - boot_at < 1500000)
            g_home_requested.store(true, std::memory_order_release);
        boot_down = boot;
        if (++battery_ticks >= 600) { battery_ticks = 0; sample_battery(); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

bool start() {
    if (g_ready.load()) return true;
    if (xTaskCreatePinnedToCore(monitor_task, "bat_pwr349", 3584, nullptr, 3, nullptr, 1) != pdPASS) {
        return false;
    }
    g_ready.store(true, std::memory_order_release);


    adc_oneshot_unit_init_cfg_t unit_cfg{};
    unit_cfg.unit_id = kAdcUnit;
    if (adc_oneshot_new_unit(&unit_cfg, &g_adc) != ESP_OK) return true;

    adc_oneshot_chan_cfg_t chan_cfg{};
    chan_cfg.atten = kAtten;
    chan_cfg.bitwidth = kWidth;
    if (adc_oneshot_config_channel(g_adc, kBatteryChannel, &chan_cfg) != ESP_OK) return true;

    adc_cali_curve_fitting_config_t cal_cfg{};
    cal_cfg.unit_id = kAdcUnit;
    cal_cfg.atten = kAtten;
    cal_cfg.bitwidth = kWidth;
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &g_cali) != ESP_OK) return true;

    sample_battery();
    ESP_LOGI(TAG, "3.49B V2 battery/PWR monitor ready (ADC GPIO4, PWR GPIO16, SYS_EN via TCA9554)");
    return true;
}

Status status() {
    Status s;
    s.available = g_available.load(std::memory_order_relaxed);
    s.battery_present = g_present.load(std::memory_order_relaxed);
    s.percent = g_percent.load(std::memory_order_relaxed);
    s.vbus_present = false;
    s.charging = false;
    return s;
}

bool consume_poweroff_request() { return g_poweroff_requested.exchange(false, std::memory_order_acq_rel); }

bool consume_home_request() { return g_home_requested.exchange(false, std::memory_order_acq_rel); }

bool power_key_ready() { return g_ready.load(std::memory_order_acquire); }

} // namespace BatteryMonitor
