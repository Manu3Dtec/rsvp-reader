#include "bsp/esp-bsp.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

namespace {
constexpr const char *TAG = "BSP_349V2";

// Official ESP32-S3 Touch-LCD-3.49 V2 pin mapping.
constexpr gpio_num_t LCD_CS = GPIO_NUM_9;
constexpr gpio_num_t LCD_CLK = GPIO_NUM_10;
constexpr gpio_num_t LCD_D0 = GPIO_NUM_11;
constexpr gpio_num_t LCD_D1 = GPIO_NUM_12;
constexpr gpio_num_t LCD_D2 = GPIO_NUM_13;
constexpr gpio_num_t LCD_D3 = GPIO_NUM_14;
constexpr gpio_num_t LCD_BL_PWM = GPIO_NUM_42;
constexpr gpio_num_t EXIO_INT = GPIO_NUM_8;
constexpr gpio_num_t PWR_GPIO = GPIO_NUM_16;
constexpr gpio_num_t SYS_I2C_SDA = GPIO_NUM_47;
constexpr gpio_num_t SYS_I2C_SCL = GPIO_NUM_48;
constexpr gpio_num_t TOUCH_I2C_SDA = GPIO_NUM_17;
constexpr gpio_num_t TOUCH_I2C_SCL = GPIO_NUM_18;
constexpr gpio_num_t SD_CMD = GPIO_NUM_39;
constexpr gpio_num_t SD_D0 = GPIO_NUM_40;
constexpr gpio_num_t SD_CLK = GPIO_NUM_41;

constexpr uint8_t TOUCH_ADDR = 0x3B;
constexpr uint16_t NATIVE_W = 172;
constexpr uint16_t NATIVE_H = 640;
constexpr uint16_t DISP_W = 640;
constexpr uint16_t DISP_H = 172;
constexpr uint32_t LCD_PCLK_HZ = 40 * 1000 * 1000;
constexpr size_t DMA_LINES = 64;
constexpr size_t DMA_PIXELS = NATIVE_W * DMA_LINES;

constexpr uint64_t EXIO_TOUCH_INT = 1ULL << 0;
constexpr uint64_t EXIO_BL_EN = 1ULL << 1;
constexpr uint64_t EXIO_LCD_RST = 1ULL << 5;
constexpr uint64_t EXIO_SYS_EN = 1ULL << 6;

// Waveshare ESP32-S3 Touch-LCD-3.49 V2 official panel wake/display-on sequence.
static const axs15231b_lcd_init_cmd_t LCD_INIT_CMDS[] = {
    {0x11, nullptr, 0, 100}, // Sleep Out
    {0x29, nullptr, 0, 100}, // Display On
};

SemaphoreHandle_t g_lvgl_mutex = nullptr;
SemaphoreHandle_t g_flush_sem = nullptr;
lv_display_t *g_display = nullptr;
lv_indev_t *g_indev = nullptr;
QueueHandle_t g_touch_samples = nullptr;
esp_pm_lock_handle_t g_gui_cpu_lock = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_panel_io_handle_t g_panel_io = nullptr;
esp_io_expander_handle_t g_expander = nullptr;
i2c_master_bus_handle_t g_system_i2c = nullptr;
i2c_master_bus_handle_t g_touch_i2c = nullptr;
i2c_master_dev_handle_t g_touch = nullptr;
uint16_t *g_dma = nullptr;
uint16_t *g_rotated = nullptr;
bool g_sd_mounted = false;

bool flush_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(g_flush_sem, &wake);
    return wake == pdTRUE;
}

void lv_tick_cb(void *) { lv_tick_inc(5); }


void lvgl_task(void *) {
    int64_t report_us = esp_timer_get_time();
    int64_t longest_handler_us = 0;
    while (true) {
        uint32_t delay = 10;
        if (bsp_display_lock(UINT32_MAX)) {
            // Full-frame rotation/rendering must not run at the idle 80 MHz.
            // Release before sleeping so DFS still saves power between frames.
            if (g_gui_cpu_lock) esp_pm_lock_acquire(g_gui_cpu_lock);
            const int64_t start_us = esp_timer_get_time();
            delay = lv_timer_handler();
            const int64_t end_us = esp_timer_get_time();
            longest_handler_us = std::max(longest_handler_us, end_us - start_us);
            if (g_gui_cpu_lock) esp_pm_lock_release(g_gui_cpu_lock);
            bsp_display_unlock();
            if (end_us - report_us >= 5000000) {
                ESP_LOGI(TAG, "GUI max handler %lld us; touch poll 10 ms", longest_handler_us);
                report_us = end_us;
                longest_handler_us = 0;
            }
        }
        // Honor the input timer's deadline instead of adding a 10-ms floor.
        delay = std::clamp<uint32_t>(delay, 2, 100);
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

esp_err_t init_i2c() {
    if (g_system_i2c && g_touch_i2c) return ESP_OK;

    i2c_master_bus_config_t cfg{};
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port = I2C_NUM_0;
    cfg.scl_io_num = SYS_I2C_SCL;
    cfg.sda_io_num = SYS_I2C_SDA;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &g_system_i2c), TAG, "system I2C");

    cfg.i2c_port = I2C_NUM_1;
    cfg.scl_io_num = TOUCH_I2C_SCL;
    cfg.sda_io_num = TOUCH_I2C_SDA;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &g_touch_i2c), TAG, "touch I2C");

    i2c_device_config_t touch_cfg{};
    touch_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    touch_cfg.device_address = TOUCH_ADDR;
    touch_cfg.scl_speed_hz = 300000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_touch_i2c, &touch_cfg, &g_touch), TAG, "touch add");
    return ESP_OK;
}

esp_err_t init_expander() {
    if (g_expander) return ESP_OK;
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C init");
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(
        g_system_i2c, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &g_expander), TAG, "TCA9554");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(g_expander, EXIO_TOUCH_INT, IO_EXPANDER_INPUT), TAG, "touch int dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(g_expander, EXIO_BL_EN | EXIO_LCD_RST, IO_EXPANDER_OUTPUT), TAG, "display expander out dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(g_expander, EXIO_LCD_RST, 1), TAG, "LCD reset high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(g_expander, EXIO_BL_EN, 0), TAG, "BL off");
    return ESP_OK;
}

void reset_lcd() {
    esp_io_expander_set_level(g_expander, EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_io_expander_set_level(g_expander, EXIO_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_io_expander_set_level(g_expander, EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

esp_err_t init_backlight() {
    gpio_set_direction(LCD_BL_PWM, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_PWM, 0);

    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 50 * 1000;
    timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");

    ledc_channel_config_t ch{};
    ch.gpio_num = LCD_BL_PWM;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 255;
    ch.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "backlight channel");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(g_expander, EXIO_BL_EN, 1), TAG, "BL enable");
    return ESP_OK;
}

void touch_sample(lv_indev_data_t *data) {
    static lv_point_t last_point = {0, 0};
    static int64_t last_valid_us = 0;
    static int64_t last_error_log_us = 0;
    static bool pressed = false;
    static bool pending = false;
    static lv_point_t pending_point{};
    static int64_t pending_us = 0;

    const int64_t now_us = esp_timer_get_time();
    auto report_missing_sample = [&]() {
        // Bridge brief gaps (including spurious UP reports) so one drag
        // remains one gesture. Release after at most 25 ms without contact.
        if (pressed && now_us - last_valid_us <= 25000) {
            data->point = last_point;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->point = last_point;
            data->state = LV_INDEV_STATE_RELEASED;
            pressed = false;
        }
        data->continue_reading = false;
    };

    if (!g_touch) {
        report_missing_sample();
        return;
    }

    // Request both controller contacts (2 + 2*6 bytes), as in Waveshare's
    // factory command. When count is two, the contact nearest to the previous
    // point is retained so a phantom record cannot teleport the pointer.
    // Keep the official driver's separate write/read transactions.
    uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x0e, 0, 0, 0};
    uint8_t buf[14]{};
    const esp_err_t write_err = i2c_master_transmit(g_touch, cmd, sizeof(cmd), 8);
    const esp_err_t read_err = write_err == ESP_OK ?
        i2c_master_receive(g_touch, buf, sizeof(buf), 8) : write_err;
    const bool read_ok = read_err == ESP_OK;
    if (!read_ok) {
        if (now_us - last_error_log_us > 5000000) {
            last_error_log_us = now_us;
            ESP_LOGW(TAG, "touch I2C read failed: %s", esp_err_to_name(read_err));
        }
        report_missing_sample();
        return;
    }
    // Byte 0 is a gesture field, not a fixed C0 signature (official driver).
    if (buf[1] == 0) {
        pending = false;
        report_missing_sample();
        return;
    }
    if (buf[1] > 2) {
        report_missing_sample();
        return;
    }
    // LVGL 9.5 rotates pointer coordinates after this callback. Decode both
    // AXS15231B records and, during an existing contact, keep the record nearest
    // to the previous point. The controller occasionally reports a phantom
    // second contact; blindly taking record zero made the pointer jump across
    // the chapter screen.
    struct ContactRecord { lv_point_t point; uint8_t event; bool valid; };
    auto decode = [&](size_t offset) -> ContactRecord {
        const uint16_t raw_x = static_cast<uint16_t>(((buf[offset] & 0x0F) << 8) | buf[offset + 1]);
        const uint16_t raw_y = static_cast<uint16_t>(((buf[offset + 2] & 0x0F) << 8) | buf[offset + 3]);
        return {{static_cast<lv_coord_t>(raw_y),
                 static_cast<lv_coord_t>(DISP_W - 1 - raw_x)},
                static_cast<uint8_t>((buf[offset] >> 6) & 0x03),
                raw_x < DISP_W && raw_y < DISP_H};
    };
    ContactRecord chosen = decode(2);
    if (buf[1] == 2) {
        const ContactRecord second = decode(8);
        if (!chosen.valid || (second.valid && pressed)) {
            if (!chosen.valid) chosen = second;
            else {
                const int first_dx = std::abs(static_cast<int>(chosen.point.x - last_point.x));
                const int first_dy = std::abs(static_cast<int>(chosen.point.y - last_point.y));
                const int second_dx = std::abs(static_cast<int>(second.point.x - last_point.x));
                const int second_dy = std::abs(static_cast<int>(second.point.y - last_point.y));
                if (second_dx + second_dy < first_dx + first_dy) chosen = second;
            }
        }
    }
    if (!chosen.valid || chosen.event == 3) {
        report_missing_sample();
        return;
    }
    data->continue_reading = false;
    if (chosen.event == 1) {
        pending = false;
        report_missing_sample();
        return;
    }
    if (chosen.event != 0 && chosen.event != 2) {
        report_missing_sample();
        return;
    }

    const int dx = std::abs(static_cast<int>(chosen.point.x - last_point.x));
    const int dy = std::abs(static_cast<int>(chosen.point.y - last_point.y));
    // Full-frame refreshes can delay the next poll well beyond 10 ms.
    // Allow proportionally more movement instead of freezing a fast drag.
    const int elapsed_ms = static_cast<int>(std::min<int64_t>(
        std::max<int64_t>(0, now_us - last_valid_us) / 1000, 200));
    const int motion_allowance = std::max(0, elapsed_ms - 10) * 2;
    if (!pressed || dx > 48 + motion_allowance || dy > 96 + motion_allowance) {
        // Confirm initial contacts and coordinate jumps with a second nearby
        // sample. Never keep extending a stale contact on rejected packets.
        const bool confirmed = pending && now_us - pending_us <= 40000 &&
            std::abs(static_cast<int>(chosen.point.x - pending_point.x)) <= 24 &&
            std::abs(static_cast<int>(chosen.point.y - pending_point.y)) <= 48;
        if (!confirmed) {
            pending_point = chosen.point;
            pending_us = now_us;
            pending = true;
            report_missing_sample();
            return;
        }
    }
    pending = false;
    last_point = chosen.point;
    last_valid_us = now_us;
    pressed = true;
    data->point = last_point;
    data->state = LV_INDEV_STATE_PRESSED;
}

// Poll independently of LVGL rendering. A full display update can take over
// 100 ms; retaining press/release edges prevents short taps disappearing in it.
// This task owns the I2C reader/filter and never calls an LVGL function.
void touch_poll_task(void *) {
    lv_indev_data_t previous{};
    previous.state = LV_INDEV_STATE_RELEASED;
    while (true) {
        lv_indev_data_t sample{};
        touch_sample(&sample);
        const bool changed = sample.state != previous.state ||
            (sample.state == LV_INDEV_STATE_PRESSED &&
             (sample.point.x != previous.point.x || sample.point.y != previous.point.y));
        if (changed) {
            if (xQueueSend(g_touch_samples, &sample, 0) != pdTRUE) {
                // Bound latency after an unusually long UI stall. Keep the
                // newest state, especially RELEASED, rather than sticking down.
                lv_indev_data_t discarded{};
                xQueueReceive(g_touch_samples, &discarded, 0);
                xQueueSend(g_touch_samples, &sample, 0);
            }
            previous = sample;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void touch_read(lv_indev_t *, lv_indev_data_t *data) {
    static lv_indev_data_t latest{};
    if (g_touch_samples) xQueueReceive(g_touch_samples, &latest, 0);
    *data = latest; // Native coordinates; LVGL rotates its copy exactly once.
    data->continue_reading = g_touch_samples && uxQueueMessagesWaiting(g_touch_samples) > 0;
}

void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    static bool first_flush = true;
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    if (!g_rotated || !g_dma || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= DISP_W || area->y2 >= DISP_H) {
        ESP_LOGE(TAG, "invalid LVGL flush area: (%ld,%ld)-(%ld,%ld)",
                 static_cast<long>(area->x1), static_cast<long>(area->y1),
                 static_cast<long>(area->x2), static_cast<long>(area->y2));
        lv_display_flush_ready(display);
        return;
    }

    // Rotate the complete logical framebuffer. The AXS15231B panel driver is
    // reliable with full native frames; partial rotated regions can use a
    // different stride and corrupt untouched display lines.
    lv_draw_sw_rgb565_swap(px_map, static_cast<uint32_t>(w * h));
    lv_area_t rotated_area = *area;
    lv_display_rotate_area(display, &rotated_area);
    const lv_color_format_t cf = lv_display_get_color_format(display);
    const uint32_t src_stride = lv_draw_buf_width_to_stride(w, cf);
    const uint32_t dst_stride = lv_draw_buf_width_to_stride(
        lv_area_get_width(&rotated_area), cf);
    lv_draw_sw_rotate(px_map, reinterpret_cast<uint8_t *>(g_rotated), w, h,
                      src_stride, dst_stride, lv_display_get_rotation(display), cf);

    xSemaphoreGive(g_flush_sem);
    for (int32_t y0 = 0; y0 < NATIVE_H; y0 += DMA_LINES) {
        const int32_t lines = std::min<int32_t>(DMA_LINES, NATIVE_H - y0);
        const size_t count = static_cast<size_t>(NATIVE_W) * lines;
        xSemaphoreTake(g_flush_sem, portMAX_DELAY);
        memcpy(g_dma, g_rotated + static_cast<size_t>(y0) * NATIVE_W,
               count * sizeof(uint16_t));
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            g_panel, 0, y0, NATIVE_W, y0 + lines, g_dma);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw failed: %s", esp_err_to_name(err));
            xSemaphoreGive(g_flush_sem);
            break;
        }
    }
    xSemaphoreTake(g_flush_sem, portMAX_DELAY);
    if (first_flush) {
        first_flush = false;
        ESP_LOGI(TAG, "first full LVGL frame flushed to LCD");
    }
    lv_display_flush_ready(display);
}

} // namespace

extern "C" lv_display_t *bsp_display_start(void) {
    if (g_display) return g_display;
    ESP_ERROR_CHECK(init_expander());
    ESP_ERROR_CHECK(init_backlight());

    gpio_config_t pwr{};
    pwr.pin_bit_mask = 1ULL << PWR_GPIO;
    pwr.mode = GPIO_MODE_INPUT;
    pwr.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&pwr));

    g_flush_sem = xSemaphoreCreateBinary();
    g_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    g_dma = static_cast<uint16_t *>(heap_caps_malloc(DMA_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    g_rotated = static_cast<uint16_t *>(heap_caps_calloc(static_cast<size_t>(NATIVE_W) * NATIVE_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    ESP_ERROR_CHECK(g_dma && g_rotated ? ESP_OK : ESP_ERR_NO_MEM);

    spi_bus_config_t bus{};
    bus.sclk_io_num = LCD_CLK;
    bus.data0_io_num = LCD_D0;
    bus.data1_io_num = LCD_D1;
    bus.data2_io_num = LCD_D2;
    bus.data3_io_num = LCD_D3;
    bus.max_transfer_sz = DMA_PIXELS * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.cs_gpio_num = LCD_CS;
    io_cfg.dc_gpio_num = GPIO_NUM_NC;
    io_cfg.spi_mode = 3;
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = flush_done;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &g_panel_io));

    // Match Waveshare's official ESP32-S3 Touch-LCD-3.49 V2 panel wake sequence.
    axs15231b_vendor_config_t vendor{};
    vendor.flags.use_qspi_interface = 1;
    vendor.init_cmds = LCD_INIT_CMDS;
    vendor.init_cmds_size = sizeof(LCD_INIT_CMDS) / sizeof(LCD_INIT_CMDS[0]);

    esp_lcd_panel_dev_config_t panel_cfg{};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor;
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(g_panel_io, &panel_cfg, &g_panel));
    reset_lcd();
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
    // The vendor init sequence already sends DISPON (0x29). Do not repeat it:
    // the AXS15231B shares its display and touch control silicon.
    ESP_LOGI(TAG, "AXS15231B initialized with Waveshare V2 wake sequence");

    ESP_ERROR_CHECK(bsp_display_brightness_set(0));
    ESP_LOGI(TAG, "backlight off until application frame is ready");

    lv_init();
    g_display = lv_display_create(NATIVE_W, NATIVE_H);
    lv_display_set_flush_cb(g_display, flush_cb);
    constexpr size_t DRAW_BYTES = static_cast<size_t>(DISP_W) * DISP_H * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(DRAW_BYTES, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(DRAW_BYTES, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(g_display, buf1, buf2, DRAW_BYTES,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_rotation(g_display, LV_DISPLAY_ROTATION_270);

    g_touch_samples = xQueueCreate(64, sizeof(lv_indev_data_t));
    ESP_ERROR_CHECK(g_touch_samples ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(touch_poll_task, "touch_poll", 4096,
        nullptr, 5, nullptr, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Independent touch polling active; press/release queue ready");
    g_indev = lv_indev_create();
    lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_indev, touch_read);
    lv_indev_set_display(g_indev, g_display);
    lv_timer_set_period(lv_indev_get_read_timer(g_indev), 10);
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "gui", &g_gui_cpu_lock));

    esp_timer_create_args_t tick_args{};
    tick_args.callback = lv_tick_cb;
    tick_args.name = "lv_tick";
    esp_timer_handle_t tick = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 5000));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 6144, nullptr, 4, nullptr, 0);
    // Match Waveshare's factory order: latch board power only after the LCD,
    // touch bus and LVGL task have all been initialized.
    ESP_ERROR_CHECK(esp_io_expander_set_dir(g_expander, EXIO_SYS_EN, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_expander, EXIO_SYS_EN, 1));
    // The application enables illumination after flushing its ON screen.
    ESP_LOGI(TAG, "Display ready: 640x172 landscape, AXS15231B QSPI");
    return g_display;
}

extern "C" lv_indev_t *bsp_display_get_input_dev(void) { return g_indev; }

extern "C" bool bsp_display_lock(uint32_t timeout_ms) {
    if (!g_lvgl_mutex) return false;
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(g_lvgl_mutex, ticks) == pdTRUE;
}

extern "C" void bsp_display_unlock(void) {
    if (g_lvgl_mutex) xSemaphoreGiveRecursive(g_lvgl_mutex);
}

extern "C" esp_err_t bsp_display_brightness_set(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    // Factory LCD_PWM_MODE_255 expands to (0xff - 255): GPIO42 is active-low.
    const uint32_t duty = 255 - static_cast<uint32_t>(percent) * 255 / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "BL duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

extern "C" esp_err_t bsp_display_sleep(bool sleep) {
    if (!g_panel) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_disp_on_off(g_panel, !sleep);
}

extern "C" i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
    if (!g_system_i2c) init_i2c();
    return g_system_i2c;
}

extern "C" esp_err_t bsp_sdcard_mount(void) {
    if (g_sd_mounted) return ESP_OK;
    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = false;
    mount.max_files = 8;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    sdmmc_card_t *card = nullptr;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (err == ESP_OK) g_sd_mounted = true;
    return err;
}

extern "C" bool bsp_pwr_button_pressed(void) {
    return gpio_get_level(PWR_GPIO) == 0;
}

extern "C" esp_err_t bsp_power_off(void) {
    ESP_RETURN_ON_ERROR(init_expander(), TAG, "expander init");
    // Waveshare V2 BATT/PWR example powers the board off by driving TCA9554 SYS_EN low.
    bsp_display_brightness_set(0);
    bsp_display_sleep(true);
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(g_expander, EXIO_SYS_EN, 0), TAG, "power latch off");
    // USB can keep the supply rail alive; deep sleep also switches the MCU off.
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(PWR_GPIO, 0), TAG, "PWR wake");
    esp_deep_sleep_start();
    return ESP_OK;
}
