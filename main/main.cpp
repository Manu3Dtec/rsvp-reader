#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "bsp/esp-bsp.h"
#include "battery_monitor.h"
#include "epub_reader.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "reader_engine.h"
#include "transfer_common.h"
#include "transfer_wifi.h"
#include "unicode_fonts.h"
#include "rtc_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint32_t kBg = 0x050506;
constexpr uint32_t kCard = 0x141418;
constexpr uint32_t kCardPressed = 0x202027;
constexpr uint32_t kText = 0xF5F5F7;
constexpr uint32_t kMuted = 0x8E8E93;
constexpr uint32_t kAccent = 0x5E5CE6;
constexpr uint32_t kOrp = 0xFF453A;
constexpr uint16_t kMinWpm = 100;
constexpr uint16_t kMaxWpm = 1000;
constexpr uint16_t kWpmStep = 25;

enum class Language : uint8_t { German = 0, English = 1 };
enum class TransferMode : uint8_t { None = 0, Wifi };

const char *TAG = "RSVP_V2";

void configure_power_saving() {
    // Dynamic frequency scaling saves power while automatic light sleep stays
    // disabled for stable QSPI display and touch operation.
    esp_pm_config_t pm{};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = false;
    const esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Power saving: dynamic 80-240 MHz CPU, automatic light-sleep off");
    } else {
        ESP_LOGW(TAG, "Power saving setup unavailable: %s", esp_err_to_name(err));
    }
}

ReaderEngine g_reader;
std::vector<std::string> g_books;
std::string g_active_book_key;
std::string g_active_book_title;
std::string g_last_book;
std::vector<std::string> g_recent_books;
std::string g_pending_delete;

lv_obj_t *g_screen = nullptr;
lv_obj_t *g_before = nullptr;
lv_obj_t *g_orp = nullptr;
lv_obj_t *g_after = nullptr;
lv_obj_t *g_progress_label = nullptr;
lv_obj_t *g_jump_slider = nullptr;
lv_obj_t *g_jump_label = nullptr;
std::vector<lv_obj_t *> g_chapter_buttons;
size_t g_jump_chapter = 0;
lv_obj_t *g_speed_label = nullptr;
lv_obj_t *g_play_hint = nullptr;
lv_obj_t *g_play_icon = nullptr;
lv_obj_t *g_wpm_panel = nullptr;
lv_obj_t *g_wpm_slider = nullptr;
lv_obj_t *g_wpm_popup_value = nullptr;
lv_obj_t *g_progress_bar = nullptr;
lv_obj_t *g_overlay = nullptr;
lv_timer_t *g_reader_timer = nullptr;
lv_timer_t *g_overlay_timer = nullptr;
lv_timer_t *g_transfer_timer = nullptr;
lv_timer_t *g_battery_timer = nullptr;
lv_timer_t *g_clock_timer = nullptr;
lv_indev_t *g_touch_indev = nullptr;
lv_obj_t *g_transfer_status = nullptr;
lv_obj_t *g_transfer_bar = nullptr;
lv_obj_t *g_battery_text = nullptr;
lv_obj_t *g_battery_fill = nullptr;
lv_obj_t *g_clock_text = nullptr;
lv_obj_t *g_sentence_pause_label = nullptr;
lv_obj_t *g_clause_pause_label = nullptr;
lv_obj_t *g_brightness_label = nullptr;
lv_obj_t *g_sleep_label = nullptr;

bool g_playing = false;
bool g_sd_mounted = false;
bool g_book_loading = false;
uint16_t g_wpm = 350;
uint32_t g_words_since_save = 0;
Language g_language = Language::German;
TransferMode g_transfer_mode = TransferMode::None;
bool g_home_after_transfer_stop = false;
bool g_display_sleeping = false;
uint8_t g_brightness = 80;
constexpr uint8_t g_auto_sleep_minutes = 2; // Fixed automatic power-off after 2 idle minutes
uint16_t g_sentence_pause_pct = 100;
uint16_t g_clause_pause_pct = 40;
int64_t g_last_activity_us = 0;

bool g_reader_mode = false;
enum class ReaderMenuAction : uint8_t { None, Chapter, Wpm };

// Forward declarations for UI/navigation helpers used before their definitions.
void delete_ui_timers();
void show_home();
void show_library();
void show_settings();
void show_language();
void show_wifi_transfer();
void show_delete_confirm(const std::string &path);
void show_loading(const char *title, const char *subtitle = nullptr);
void stop_reader(bool save = true);
void build_reader_ui();
void show_chapter_overview();
void start_book_open(const std::string &path);
void create_battery_indicator();
void create_clock_indicator();

const char *tr(const char *de, const char *en) {
    return g_language == Language::English ? en : de;
}

uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (s && *s) {
        h ^= static_cast<uint8_t>(*s++);
        h *= 16777619u;
    }
    return h;
}

std::string position_key(const std::string &path) {
    char key[16];
    std::snprintf(key, sizeof(key), "p%08lx", static_cast<unsigned long>(fnv1a(path.c_str())));
    return key;
}

bool file_exists(const std::string &path) {
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

std::string basename_no_ext(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

bool has_extension(const std::string &name, const char *wanted) {
    std::string ext = wanted ? wanted : "";
    if (name.size() < ext.size()) return false;
    std::string tail = name.substr(name.size() - ext.size());
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ext;
}

bool is_supported_book(const std::string &name) {
    return has_extension(name, ".epub") || has_extension(name, ".txt");
}

bool remove_file_if_exists(const std::string &path) {
    if (path.empty()) return false;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return false;
    return std::remove(path.c_str()) == 0;
}

void touch_activity() {
    g_last_activity_us = esp_timer_get_time();
    if (g_display_sleeping) {
        g_display_sleeping = false;
        bsp_display_sleep(false);
        bsp_display_brightness_set(g_brightness);
    }
}

std::string format_storage() {
    if (!g_sd_mounted) return tr("microSD nicht erkannt", "microSD not detected");
    uint64_t total = 0;
    uint64_t freeb = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb) != ESP_OK) return tr("Speicher unbekannt", "Storage unknown");
    char out[80];
    const double total_gb = static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0);
    const double free_gb = static_cast<double>(freeb) / (1024.0 * 1024.0 * 1024.0);
    std::snprintf(out, sizeof(out), tr("%.1f GB frei / %.1f GB", "%.1f GB free / %.1f GB"),
                  free_gb, total_gb);
    return out;
}

void remember_recent(const std::string &path) {
    if (path.empty()) return;
    g_recent_books.erase(std::remove(g_recent_books.begin(), g_recent_books.end(), path), g_recent_books.end());
    g_recent_books.insert(g_recent_books.begin(), path);
    if (g_recent_books.size() > 3) g_recent_books.resize(3);
}

void forget_recent(const std::string &path) {
    g_recent_books.erase(std::remove(g_recent_books.begin(), g_recent_books.end(), path), g_recent_books.end());
    if (g_last_book == path) g_last_book.clear();
}


std::string display_safe(std::string text) {
    auto replace_all = [&](const char *from, const char *to) {
        const size_t from_len = std::strlen(from);
        const size_t to_len = std::strlen(to);
        size_t pos = 0;
        while (from_len && (pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from_len, to);
            pos += to_len;
        }
    };
    replace_all("\xC2\xA0", " ");
    replace_all("\xE2\x80\x87", " ");
    replace_all("\xE2\x80\x89", " ");
    replace_all("\xE2\x80\xAF", " ");
    replace_all("\xE2\x80\x8B", "");
    replace_all("\xE2\x81\xA0", "");
    replace_all("\xEF\xBF\xBC", "");
    replace_all("\xEF\xBF\xBD", "?");
    return text;
}

void scan_books() {
    g_books.clear();
    if (!g_sd_mounted) return;
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (!dir) return;
    while (dirent *entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.empty() || name[0] == '.' || !is_supported_book(name)) continue;
        g_books.emplace_back(std::string(BSP_SD_MOUNT_POINT) + "/" + name);
    }
    closedir(dir);
    std::sort(g_books.begin(), g_books.end(), [](const std::string &a, const std::string &b) {
        std::string aa = a;
        std::string bb = b;
        std::transform(aa.begin(), aa.end(), aa.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bb.begin(), bb.end(), bb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return aa < bb;
    });
}

size_t load_position(const std::string &path) {
    nvs_handle_t handle;
    if (nvs_open("positions", NVS_READONLY, &handle) != ESP_OK) return 0;
    uint32_t value = 0;
    nvs_get_u32(handle, position_key(path).c_str(), &value);
    nvs_close(handle);
    return value;
}

void save_position() {
    if (!g_reader.valid() || g_active_book_key.empty()) return;
    nvs_handle_t handle;
    if (nvs_open("positions", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, position_key(g_active_book_key).c_str(), static_cast<uint32_t>(g_reader.position()));
    nvs_commit(handle);
    nvs_close(handle);
    g_words_since_save = 0;
}

void load_settings() {
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READONLY, &handle) != ESP_OK) return;
    uint16_t value = g_wpm;
    if (nvs_get_u16(handle, "wpm", &value) == ESP_OK) g_wpm = std::clamp<uint16_t>(value, kMinWpm, kMaxWpm);
    uint8_t lang = 0;
    if (nvs_get_u8(handle, "lang", &lang) == ESP_OK) g_language = lang == 1 ? Language::English : Language::German;

    uint8_t brightness = g_brightness;
    if (nvs_get_u8(handle, "bright", &brightness) == ESP_OK) g_brightness = std::clamp<uint8_t>(brightness, 20, 100);
    uint16_t sentence_pct = g_sentence_pause_pct;
    if (nvs_get_u16(handle, "sent_pct", &sentence_pct) == ESP_OK) g_sentence_pause_pct = std::min<uint16_t>(sentence_pct, 200);
    uint16_t clause_pct = g_clause_pause_pct;
    if (nvs_get_u16(handle, "clause_pct", &clause_pct) == ESP_OK) g_clause_pause_pct = std::min<uint16_t>(clause_pct, 150);

    size_t required = 0;
    if (nvs_get_str(handle, "last_book", nullptr, &required) == ESP_OK && required > 1 && required < 256) {
        std::vector<char> buffer(required, 0);
        if (nvs_get_str(handle, "last_book", buffer.data(), &required) == ESP_OK) g_last_book = buffer.data();
    }
    g_recent_books.clear();
    for (int i = 0; i < 3; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "rec%d", i);
        size_t rec_required = 0;
        if (nvs_get_str(handle, key, nullptr, &rec_required) == ESP_OK && rec_required > 1 && rec_required < 256) {
            std::vector<char> buf(rec_required, 0);
            if (nvs_get_str(handle, key, buf.data(), &rec_required) == ESP_OK && file_exists(buf.data())) {
                g_recent_books.emplace_back(buf.data());
            }
        }
    }
    nvs_close(handle);
}

void save_settings() {
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u16(handle, "wpm", g_wpm);
    nvs_set_u8(handle, "lang", static_cast<uint8_t>(g_language));
    nvs_set_u8(handle, "bright", g_brightness);
    nvs_set_u16(handle, "sent_pct", g_sentence_pause_pct);
    nvs_set_u16(handle, "clause_pct", g_clause_pause_pct);
    if (!g_last_book.empty()) nvs_set_str(handle, "last_book", g_last_book.c_str());
    else nvs_erase_key(handle, "last_book");
    for (int i = 0; i < 3; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "rec%d", i);
        if (i < static_cast<int>(g_recent_books.size())) nvs_set_str(handle, key, g_recent_books[i].c_str());
        else nvs_erase_key(handle, key);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void transfer_stop_task(void *arg) {
    const auto mode = static_cast<TransferMode>(reinterpret_cast<intptr_t>(arg));
    if (mode == TransferMode::Wifi) WifiTransfer::stop();

    ESP_LOGI(TAG, "Transfer backend stopped; DMA/internal free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));

    if (g_home_after_transfer_stop) {
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (bsp_display_lock(100)) {

                    g_home_after_transfer_stop = false;
                    show_home();
                bsp_display_unlock();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    vTaskDelete(nullptr);
}

void stop_active_transfer() {
    const TransferMode mode = g_transfer_mode;
    g_transfer_mode = TransferMode::None;
    if (mode == TransferMode::None) return;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        transfer_stop_task, "transfer_stop", 4096,
        reinterpret_cast<void *>(static_cast<intptr_t>(mode)),
        3, nullptr, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create transfer stop task");
    }
}

void delete_ui_timers() {
    auto del = [](lv_timer_t *&timer) {
        if (timer) {
            lv_timer_delete(timer);
            timer = nullptr;
        }
    };
    del(g_reader_timer);
    del(g_overlay_timer);
    del(g_transfer_timer);
    del(g_battery_timer);
    del(g_clock_timer);
}

void clear_screen(bool status_indicators = true) {
    g_reader_mode = false;
    stop_active_transfer();
    delete_ui_timers();
    g_playing = false;

    lv_obj_t *old = g_screen;
    g_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_screen, lv_color_hex(kText), 0);
    lv_screen_load(g_screen);
    if (old) lv_obj_delete(old);

    g_before = nullptr;
    g_orp = nullptr;
    g_after = nullptr;
    g_progress_label = nullptr;
    g_jump_slider = nullptr;
    g_jump_label = nullptr;
    g_chapter_buttons.clear();
    g_speed_label = nullptr;
    g_play_hint = nullptr;
    g_play_icon = nullptr;
    g_wpm_panel = nullptr;
    g_wpm_slider = nullptr;
    g_wpm_popup_value = nullptr;
    g_progress_bar = nullptr;
    g_overlay = nullptr;
    g_transfer_status = nullptr;
    g_transfer_bar = nullptr;
    g_battery_text = nullptr;
    g_battery_fill = nullptr;
    g_clock_text = nullptr;
    g_sentence_pause_label = nullptr;
    g_clause_pause_label = nullptr;
    g_brightness_label = nullptr;
    g_sleep_label = nullptr;

    if (status_indicators) {
        create_battery_indicator();
        create_clock_indicator();
    }
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (font) lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}


void update_battery_indicator() {
    if (!g_battery_text || !g_battery_fill) return;

    const BatteryMonitor::Status st = BatteryMonitor::status();
    char text[16];
    int percent = 0;
    if (!st.available || !st.battery_present) {
        std::snprintf(text, sizeof(text), st.vbus_present ? "USB" : "--%%");
    } else {
        percent = std::clamp<int>(st.percent, 0, 100);
        std::snprintf(text, sizeof(text), st.charging ? "%d%%+" : "%d%%", percent);
    }
    lv_label_set_text(g_battery_text, text);

    // Interior of the battery outline is 24 px wide. Keep a tiny visible
    // sliver at 0 % and scale linearly up to the full inner width.
    const int fill_width = st.available && st.battery_present
        ? std::max(2, (24 * percent) / 100)
        : 2;
    lv_obj_set_width(g_battery_fill, fill_width);

    uint32_t color = kText;
    if (st.available && st.battery_present && percent <= 15) color = kOrp;
    else if (st.charging) color = 0x30D158;
    lv_obj_set_style_bg_color(g_battery_fill, lv_color_hex(color), 0);
}

void battery_ui_timer_cb(lv_timer_t *) {
    update_battery_indicator();
}

void create_battery_indicator() {
    if (!g_screen) return;

    lv_obj_t *wrap = lv_obj_create(g_screen);
    lv_obj_set_size(wrap, 86, 28);
    lv_obj_align(wrap, LV_ALIGN_TOP_RIGHT, -22, 5);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    g_battery_text = make_label(wrap, "--%", &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(g_battery_text, 48);
    lv_obj_set_style_text_align(g_battery_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_battery_text, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(g_battery_text, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *body = lv_obj_create(wrap);
    lv_obj_set_size(body, 30, 16);
    lv_obj_align(body, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(kMuted), 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_pad_all(body, 2, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

    g_battery_fill = lv_obj_create(body);
    lv_obj_set_height(g_battery_fill, 10);
    lv_obj_set_width(g_battery_fill, 2);
    lv_obj_align(g_battery_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(g_battery_fill, 0, 0);
    lv_obj_set_style_radius(g_battery_fill, 1, 0);
    lv_obj_set_style_bg_color(g_battery_fill, lv_color_hex(kText), 0);
    lv_obj_remove_flag(g_battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_battery_fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap = lv_obj_create(wrap);
    lv_obj_set_size(cap, 3, 8);
    lv_obj_align(cap, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(kMuted), 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);

    update_battery_indicator();
    g_battery_timer = lv_timer_create(battery_ui_timer_cb, 15000, nullptr);
}




void update_clock_indicator() {
    if (!g_clock_text) return;
    const RtcClock::Time t = RtcClock::now();
    char text[8] = "--:--";
    if (t.valid) std::snprintf(text, sizeof(text), "%02d:%02d", t.hour, t.minute);
    lv_label_set_text(g_clock_text, text);
}

void clock_ui_timer_cb(lv_timer_t *) {
    update_clock_indicator();
}

void create_clock_indicator() {
    if (!g_screen) return;
    g_clock_text = make_label(g_screen, "--:--", &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(g_clock_text, 52);
    lv_obj_set_style_text_align(g_clock_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_clock_text, LV_ALIGN_TOP_RIGHT, -28, 34);
    lv_obj_remove_flag(g_clock_text, LV_OBJ_FLAG_CLICKABLE);
    update_clock_indicator();
    g_clock_timer = lv_timer_create(clock_ui_timer_cb, 60000, nullptr);
}



// Once a contact moves, it cannot become a menu tap again, even if it
// returns to its starting point or the list reaches its scroll boundary.
void menu_tap_guard_cb(lv_event_t *event) {
    static lv_point_t origin{};
    static bool moved = false;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST &&
        code != LV_EVENT_CLICKED) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        origin = point;
        moved = false;
    } else if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED) {
        if (std::abs(point.x - origin.x) > 10 || std::abs(point.y - origin.y) > 10)
            moved = true;
    } else if (code == LV_EVENT_PRESS_LOST) {
        moved = true;
    } else if (code == LV_EVENT_CLICKED && moved) {
        lv_event_stop_processing(event);
    }
}

lv_obj_t *make_card(lv_obj_t *parent, const char *title, const char *subtitle,
                        lv_event_cb_t cb, int32_t height, void *user_data = nullptr) {
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_width(card, 336);
    lv_obj_set_height(card, height);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_left(card, 18, 0);
    lv_obj_set_style_pad_right(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(card, menu_tap_guard_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *t = make_label(card, title, &rsvp_unicode_18, kText);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, subtitle ? -12 : 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);

    if (subtitle) {
        const lv_font_t *subtitle_font = &rsvp_unicode_14;
        lv_point_t measured{};
        lv_text_get_size(&measured, subtitle, subtitle_font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (measured.x > 270) subtitle_font = &rsvp_unicode_12;
        lv_text_get_size(&measured, subtitle, subtitle_font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (measured.x > 270) subtitle_font = &rsvp_unicode_10;
        lv_obj_t *sub = make_label(card, subtitle, subtitle_font, kMuted);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, 270);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 0, 15);
        lv_obj_remove_flag(sub, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *arrow = make_label(card, ">", &lv_font_montserrat_18, kMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

lv_obj_t *make_small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int32_t width) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 48);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCardPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = make_label(button, text, &lv_font_montserrat_14, kText);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

lv_obj_t *make_direct_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                             int32_t width, int32_t height = 52) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x24242A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kAccent), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = make_label(button, text, &rsvp_unicode_18, kText);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

void stop_reader(bool save) {
    g_playing = false;
    if (g_reader_timer) lv_timer_pause(g_reader_timer);
    if (g_play_hint) lv_label_set_text(g_play_hint, tr("Tippen zum Starten", "Tap to play"));
    if (g_play_icon) lv_label_set_text(g_play_icon, LV_SYMBOL_PLAY);
    if (save) save_position();
}

void overlay_hide_cb(lv_timer_t *timer) {
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

void show_overlay(const std::string &text) {
    // No pop-ups/overlays while reading; keep the RSVP text unobstructed.
    if (g_reader_mode) return;
    if (!g_overlay) return;
    lv_label_set_text(g_overlay, text.c_str());
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_overlay);
    if (g_overlay_timer) {
        lv_timer_set_period(g_overlay_timer, 650);
        lv_timer_reset(g_overlay_timer);
        lv_timer_resume(g_overlay_timer);
    }
}

void update_reader_meta() {
    // The speed label also exists on the Settings screen where no book is
    // necessarily open. Always refresh it before checking reader validity.
    if (g_speed_label) {
        char speed[24];
        std::snprintf(speed, sizeof(speed), "%u WPM", g_wpm);
        lv_label_set_text(g_speed_label, speed);
    }
    if (!g_reader.valid()) return;
    if (g_progress_label) {
        char progress[48];
        const int pct = static_cast<int>(g_reader.progress() * 100.0f);
        const size_t chapters = g_reader.chapter_count();
        const size_t chapter = g_reader.chapter_index();
        if (chapters > 1 && chapter > 0) {
            std::snprintf(progress, sizeof(progress), tr("%d%%  Kap. %u/%u", "%d%%  Ch. %u/%u"),
                          pct, static_cast<unsigned>(chapter), static_cast<unsigned>(chapters));
        } else {
            std::snprintf(progress, sizeof(progress), "%d%%", pct);
        }
        lv_label_set_text(g_progress_label, progress);
    }
    if (g_progress_bar) lv_bar_set_value(g_progress_bar, static_cast<int32_t>(g_reader.progress() * 100.0f), LV_ANIM_OFF);
}

void render_word(const std::string &word) {
    if (!g_before || !g_orp || !g_after) return;

    const std::string shown = display_safe(word);
    std::string before, orp, after;
    ReaderEngine::split_orp(shown, before, orp, after);

    // Use the labels themselves for measuring. This avoids LVGL's text-width
    // helper API, whose v8 compatibility mapping is unavailable in this build.
    const lv_font_t *fonts[] = {
        &rsvp_unicode_48,
        &rsvp_unicode_28,
        &rsvp_unicode_24,
        &rsvp_unicode_18,
        &rsvp_unicode_14
    };

    constexpr int kHalfWordArea = 300; // 640 px display, 20 px margin on each side
    constexpr int kGap = 2;

    lv_label_set_text(g_before, before.c_str());
    lv_label_set_text(g_orp, orp.c_str());
    lv_label_set_text(g_after, after.c_str());

    const lv_font_t *word_font = fonts[4];

    for (const lv_font_t *candidate : fonts) {
        lv_obj_set_style_text_font(g_before, candidate, 0);
        lv_obj_set_style_text_font(g_orp, candidate, 0);
        lv_obj_set_style_text_font(g_after, candidate, 0);

        // Measure actual glyph advances, including the ORP glyph, rather than
        // relying on LVGL label object widths (which may be stale or padded).
        lv_point_t before_size{}, orp_size{}, after_size{};
        lv_text_get_size(&before_size, before.c_str(), candidate, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&orp_size, orp.c_str(), candidate, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&after_size, after.c_str(), candidate, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int before_w = before.empty() ? 0 : before_size.x;
        const int orp_w = orp.empty() ? 0 : orp_size.x;
        const int after_w = after.empty() ? 0 : after_size.x;

        const int left_need = before_w + kGap + (orp_w / 2);
        const int right_need = after_w + kGap + ((orp_w + 1) / 2);

        if (left_need <= kHalfWordArea && right_need <= kHalfWordArea) {
            word_font = candidate;
            break;
        }
    }

    // Apply the selected font once more because the loop may have tested
    // smaller candidates after the current default.
    lv_obj_set_style_text_font(g_before, word_font, 0);
    lv_obj_set_style_text_font(g_orp, word_font, 0);
    lv_obj_set_style_text_font(g_after, word_font, 0);

    lv_obj_update_layout(g_before);
    lv_obj_update_layout(g_orp);
    lv_obj_update_layout(g_after);
    lv_obj_align(g_orp, LV_ALIGN_CENTER, 0, -6);
    lv_obj_align_to(g_before, g_orp, LV_ALIGN_OUT_LEFT_MID, -kGap, 0);
    lv_obj_align_to(g_after, g_orp, LV_ALIGN_OUT_RIGHT_MID, kGap, 0);

    update_reader_meta();
}

void reader_tick_cb(lv_timer_t *timer) {
    std::string word;
    if (!g_reader.next(word)) {
        stop_reader();
        if (g_play_hint) lv_label_set_text(g_play_hint, tr("Ende des Buches", "End of book"));
        show_overlay(tr("Ende", "End"));
        return;
    }
    render_word(word);
    lv_timer_set_period(timer, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
    if (++g_words_since_save >= 25) save_position();
}

void toggle_play() {
    if (!g_reader_timer || !g_reader.valid()) return;
    if (g_playing) {
        stop_reader();
        show_overlay(tr("Pause", "Pause"));
        return;
    }
    std::string word;
    if (!g_reader.current(word)) return;
    g_playing = true;
    if (g_play_hint) lv_label_set_text(g_play_hint, tr("Tippen zum Pausieren", "Tap to pause"));
    if (g_play_icon) lv_label_set_text(g_play_icon, LV_SYMBOL_PAUSE);
    lv_timer_set_period(g_reader_timer, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
    lv_timer_reset(g_reader_timer);
    lv_timer_resume(g_reader_timer);
    show_overlay(tr("Start", "Play"));
}

void change_wpm(int delta) {
    const int next = std::clamp<int>(static_cast<int>(g_wpm) + delta, kMinWpm, kMaxWpm);
    g_wpm = static_cast<uint16_t>(next);
    save_settings();
    update_reader_meta();
    char text[32];
    std::snprintf(text, sizeof(text), "%u WPM", g_wpm);
    show_overlay(text);
    if (g_playing && g_reader_timer) {
        std::string word;
        if (g_reader.current(word)) {
            lv_timer_set_period(g_reader_timer, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
            lv_timer_reset(g_reader_timer);
        }
    }
}

void step_word(bool forward) {
    stop_reader();
    std::string word;
    const bool ok = forward ? g_reader.next(word) : g_reader.previous(word);
    if (ok) render_word(word);
    save_position();
    show_overlay(forward ? tr("Naechstes Wort  >", "Next word  >") : tr("<  Vorheriges Wort", "<  Previous word"));
}

void reader_previous_cb(lv_event_t *) { step_word(false); }
void reader_next_cb(lv_event_t *) { step_word(true); }
void reader_play_cb(lv_event_t *) {
    touch_activity();
    const int64_t started_us = esp_timer_get_time();
    toggle_play();
    ESP_LOGI(TAG, "Reader playback %s; action %lld us", g_playing ? "started" : "paused",
             esp_timer_get_time() - started_us);
}

void wpm_popup_refresh() {
    if (!g_wpm_popup_value) return;
    char text[24];
    std::snprintf(text, sizeof(text), "%u WPM", g_wpm);
    lv_label_set_text(g_wpm_popup_value, text);
}

void wpm_slider_cb(lv_event_t *event) {
    if (!g_wpm_slider) return;
    g_wpm = static_cast<uint16_t>(lv_slider_get_value(g_wpm_slider));
    update_reader_meta();
    wpm_popup_refresh();
    if (g_playing && g_reader_timer) {
        std::string word;
        if (g_reader.current(word)) {
            lv_timer_set_period(g_reader_timer,
                ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
            lv_timer_reset(g_reader_timer);
        }
    }
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) save_settings();
}

void wpm_popup_close_cb(lv_event_t *) {
    save_settings();
    if (g_wpm_panel) lv_obj_delete(g_wpm_panel);
    g_wpm_panel = nullptr;
    g_wpm_slider = nullptr;
    g_wpm_popup_value = nullptr;
}

void wpm_open_cb(lv_event_t *) {
    if (!g_reader_mode || g_wpm_panel) return;
    stop_reader();
    g_wpm_panel = lv_obj_create(g_screen);
    lv_obj_set_size(g_wpm_panel, 438, 88);
    lv_obj_align(g_wpm_panel, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_set_style_radius(g_wpm_panel, 18, 0);
    lv_obj_set_style_bg_color(g_wpm_panel, lv_color_hex(0x303038), 0);
    lv_obj_set_style_bg_opa(g_wpm_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g_wpm_panel, 1, 0);
    lv_obj_set_style_border_color(g_wpm_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(g_wpm_panel, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(g_wpm_panel, 18, 0);
    lv_obj_set_style_shadow_color(g_wpm_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(g_wpm_panel, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(g_wpm_panel, 10, 0);
    lv_obj_remove_flag(g_wpm_panel, LV_OBJ_FLAG_SCROLLABLE);

    g_wpm_popup_value = make_label(g_wpm_panel, "", &rsvp_unicode_14, kText);
    lv_obj_align(g_wpm_popup_value, LV_ALIGN_TOP_LEFT, 2, -2);
    wpm_popup_refresh();
    g_wpm_slider = lv_slider_create(g_wpm_panel);
    lv_obj_set_size(g_wpm_slider, 335, 26);
    lv_obj_align(g_wpm_slider, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_slider_set_range(g_wpm_slider, kMinWpm, kMaxWpm);
    lv_slider_set_value(g_wpm_slider, g_wpm, LV_ANIM_OFF);
    lv_obj_add_event_cb(g_wpm_slider, wpm_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(g_wpm_slider, wpm_slider_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_t *close = make_small_button(g_wpm_panel, "OK", wpm_popup_close_cb, 62);
    lv_obj_set_height(close, 40);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 8);
    lv_obj_move_foreground(g_wpm_panel);
}

// A normal tap opens these controls without a long-press timer.
void reader_menu_event_cb(lv_event_t *event) {
    touch_activity();
    const ReaderMenuAction action = static_cast<ReaderMenuAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (action == ReaderMenuAction::Chapter) {
        if (!g_reader.valid()) return;
        const size_t current = g_reader.chapter_index();
        g_jump_chapter = current > 0 ? current - 1 : 0;
        show_chapter_overview();
    } else if (action == ReaderMenuAction::Wpm) {
        wpm_open_cb(nullptr);
    }
}

// Reader input follows LVGL's actual pointer events. The old sampled state
// machine discarded normal taps unless they lasted 90-400 ms, produced seven
// samples and moved no more than seven pixels.
void reader_touch_event_cb(lv_event_t *event) {
    enum class Contact : uint8_t { Idle, Tracking, Fired, Cancelled };
    static Contact contact = Contact::Idle;
    static lv_point_t origin = {0, 0};
    static int max_x = 0, max_y = 0;
    static int64_t started_us = 0;

    if (!g_reader_mode || !g_reader.valid()) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST) { contact = Contact::Cancelled; return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) indev = g_touch_indev;
    if (!indev) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    const int64_t now_us = esp_timer_get_time();
    if (code == LV_EVENT_PRESSED) {
        touch_activity();
        contact = Contact::Tracking;
        origin = point;
        max_x = max_y = 0;
        started_us = now_us;
        return;
    }
    if (contact == Contact::Idle || contact == Contact::Cancelled) {
        if (code == LV_EVENT_RELEASED) contact = Contact::Idle;
        return;
    }
    touch_activity();
    const int dx = static_cast<int>(point.x) - origin.x;
    const int dy = static_cast<int>(point.y) - origin.y;
    max_x = std::max(max_x, std::abs(dx));
    max_y = std::max(max_y, std::abs(dy));
    // A vertical gesture changes the speed exactly once. Horizontal word
    // navigation has dedicated large buttons and cannot be fired accidentally.
    if (contact == Contact::Tracking && max_y >= 24 && max_y >= max_x + 8) {
        stop_reader();
        change_wpm(dy < 0 ? static_cast<int>(kWpmStep)
                          : -static_cast<int>(kWpmStep));
        contact = Contact::Fired;
    }
    if (code != LV_EVENT_RELEASED) return;
    const int64_t held_us = now_us - started_us;
    const bool tap = contact == Contact::Tracking && max_x <= 18 && max_y <= 18 &&
                     held_us >= 20000 && held_us <= 1200000;
    contact = Contact::Idle;
    if (tap) toggle_play();
}

void home_cb(lv_event_t *) {
    stop_reader();
    g_reader.close();

    // Leaving WLAN is special: do not redraw a full screen while the Wi-Fi
    // driver still owns scarce internal/DMA memory needed by the AMOLED SPI
    // path. Keep the current screen static, tear Wi-Fi down in the worker,
    // then render Home after the memory has actually been returned.
    if (g_transfer_mode == TransferMode::Wifi || g_home_after_transfer_stop) {
        if (g_home_after_transfer_stop) return;
        g_home_after_transfer_stop = true;
        if (g_transfer_timer) {
            lv_timer_delete(g_transfer_timer);
            g_transfer_timer = nullptr;
        }
        if (g_transfer_status) {
            lv_label_set_text(g_transfer_status,
                tr("WLAN wird beendet ...", "Stopping Wi-Fi ..."));
        }
        stop_active_transfer();
        return;
    }

    show_home();
}

void library_cb(lv_event_t *) { show_library(); }
void settings_cb(lv_event_t *) { show_settings(); }
void language_cb(lv_event_t *) { show_language(); }
void wifi_transfer_cb(lv_event_t *) { show_wifi_transfer(); }

void continue_cb(lv_event_t *) {
    if (file_exists(g_last_book)) start_book_open(g_last_book);
    else show_library();
}

void speed_down_cb(lv_event_t *) {
    ESP_LOGI(TAG, "Settings WPM down: %u -> %d", g_wpm, std::max<int>(kMinWpm, static_cast<int>(g_wpm) - kWpmStep));
    change_wpm(-static_cast<int>(kWpmStep));
}
void speed_up_cb(lv_event_t *) {
    ESP_LOGI(TAG, "Settings WPM up: %u -> %d", g_wpm, std::min<int>(kMaxWpm, static_cast<int>(g_wpm) + kWpmStep));
    change_wpm(kWpmStep);
}

void book_cb(lv_event_t *event) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(event));
    if (path) start_book_open(*path);
}

void set_language_cb(lv_event_t *event) {
    const intptr_t value = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    g_language = value == 1 ? Language::English : Language::German;
    save_settings();
    show_home();
}


void recent_book_cb(lv_event_t *event) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(event));
    if (path && file_exists(*path)) start_book_open(*path);
}



void brightness_down_cb(lv_event_t *) {
    g_brightness = static_cast<uint8_t>(std::max<int>(20, g_brightness - 10));
    bsp_display_brightness_set(g_brightness);
    save_settings();
    if (g_brightness_label) {
        char text[24]; std::snprintf(text, sizeof(text), "%u%%", g_brightness);
        lv_label_set_text(g_brightness_label, text);
    }
    touch_activity();
}

void brightness_up_cb(lv_event_t *) {
    g_brightness = static_cast<uint8_t>(std::min<int>(100, g_brightness + 10));
    bsp_display_brightness_set(g_brightness);
    save_settings();
    if (g_brightness_label) {
        char text[24]; std::snprintf(text, sizeof(text), "%u%%", g_brightness);
        lv_label_set_text(g_brightness_label, text);
    }
    touch_activity();
}

void update_pause_labels() {
    if (g_sentence_pause_label) {
        char t[24]; std::snprintf(t, sizeof(t), "+%u%%", g_sentence_pause_pct);
        lv_label_set_text(g_sentence_pause_label, t);
    }
    if (g_clause_pause_label) {
        char t[24]; std::snprintf(t, sizeof(t), "+%u%%", g_clause_pause_pct);
        lv_label_set_text(g_clause_pause_label, t);
    }
}
void sentence_pause_down_cb(lv_event_t *) {
    g_sentence_pause_pct = static_cast<uint16_t>(std::max<int>(0, g_sentence_pause_pct - 25));
    save_settings(); update_pause_labels();
}
void sentence_pause_up_cb(lv_event_t *) {
    g_sentence_pause_pct = static_cast<uint16_t>(std::min<int>(200, g_sentence_pause_pct + 25));
    save_settings(); update_pause_labels();
}
void clause_pause_down_cb(lv_event_t *) {
    g_clause_pause_pct = static_cast<uint16_t>(std::max<int>(0, g_clause_pause_pct - 10));
    save_settings(); update_pause_labels();
}
void clause_pause_up_cb(lv_event_t *) {
    g_clause_pause_pct = static_cast<uint16_t>(std::min<int>(150, g_clause_pause_pct + 10));
    save_settings(); update_pause_labels();
}

void delete_book_request_cb(lv_event_t *event) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(event));
    if (path) show_delete_confirm(*path);
}

void delete_cancel_cb(lv_event_t *) {
    g_pending_delete.clear();
    show_library();
}

void delete_confirm_cb(lv_event_t *) {
    const std::string path = g_pending_delete;
    if (path.empty()) { show_library(); return; }

    if (g_reader.valid() && g_active_book_key == path) {
        stop_reader(false);
        g_reader.close();
    }

    remove_file_if_exists(path);

    if (has_extension(path, ".epub")) {
        char cache[96];
        std::snprintf(cache, sizeof(cache), "%s/.rsvp6_%08lx.txt", BSP_SD_MOUNT_POINT,
                      static_cast<unsigned long>(fnv1a(path.c_str())));
        remove_file_if_exists(cache);
        remove_file_if_exists(std::string(cache) + ".chap");
        // Clean old cache versions as well.
        std::snprintf(cache, sizeof(cache), "%s/.rsvp6_%08lx.txt", BSP_SD_MOUNT_POINT,
                      static_cast<unsigned long>(fnv1a(path.c_str())));
        remove_file_if_exists(cache);
        remove_file_if_exists(std::string(cache) + ".chap");
    }

    nvs_handle_t ph;
    if (nvs_open("positions", NVS_READWRITE, &ph) == ESP_OK) {
        nvs_erase_key(ph, position_key(path).c_str());
        nvs_commit(ph);
        nvs_close(ph);
    }

    forget_recent(path);
    save_settings();
    g_pending_delete.clear();
    scan_books();
    show_library();
}

void power_timer_cb(lv_timer_t *) {
    static bool shutting_down = false;
    if (shutting_down) return;
    if (BatteryMonitor::consume_poweroff_request()) {
        shutting_down = true;
        if (g_touch_indev) lv_indev_enable(g_touch_indev, false);
        stop_active_transfer();
        stop_reader();
        save_settings();
        bsp_display_sleep(false);
        bsp_display_brightness_set(std::max<uint8_t>(g_brightness, 30));
        clear_screen(false);
        lv_obj_t *label = make_label(g_screen, "OFF", &lv_font_montserrat_48, kText);
        lv_obj_center(label);
        lv_timer_create([](lv_timer_t *timer) {
            lv_timer_delete(timer);
            if (bsp_pwr_button_pressed()) {
                lv_timer_create([](lv_timer_t *t) {
                    if (!bsp_pwr_button_pressed()) { lv_timer_delete(t); bsp_power_off(); }
                }, 50, nullptr);
            } else bsp_power_off();
        }, 700, nullptr);
        return;
    }
    if (BatteryMonitor::consume_home_request()) {
        touch_activity();
        home_cb(nullptr);
        return;
    }
    if (!g_touch_indev) return;

    if (lv_indev_get_state(g_touch_indev) == LV_INDEV_STATE_PRESSED) {
        touch_activity();
        return;
    }

    // Do not shut down during playback or an active Wi-Fi transfer.
    if (g_playing || g_transfer_mode != TransferMode::None || g_book_loading) return;
    if (g_last_activity_us == 0) g_last_activity_us = esp_timer_get_time();

    const int64_t idle_us = esp_timer_get_time() - g_last_activity_us;
    const int64_t limit_us = static_cast<int64_t>(g_auto_sleep_minutes) * 60LL * 1000000LL;
    if (idle_us >= limit_us) {
        ESP_LOGI(TAG, "Auto power-off after 2 minutes without input");
        if (g_reader.valid()) stop_reader(); // Persist the exact reading position.
        save_settings();
        bsp_power_off();
    }
}

void refresh_transfer_status_cb(lv_timer_t *) {
    if (!g_transfer_status) return;
    std::string text;
    int progress = -1;
    if (g_transfer_mode == TransferMode::Wifi) {
        text = WifiTransfer::status();
        progress = WifiTransfer::progress();
    }
    if (!text.empty()) lv_label_set_text(g_transfer_status, display_safe(text).c_str());
    if (g_transfer_bar) {
        if (progress >= 0) {
            lv_obj_remove_flag(g_transfer_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(g_transfer_bar, std::clamp(progress, 0, 100), LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(g_transfer_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void show_home() {
    clear_screen(); scan_books();
    lv_obj_t *brand=make_label(g_screen,"RSVP",&lv_font_montserrat_28,kText); lv_obj_align(brand,LV_ALIGN_TOP_LEFT,14,8);
    lv_obj_t *reader=make_label(g_screen,"READER  3.49",&lv_font_montserrat_14,kAccent); lv_obj_align(reader,LV_ALIGN_TOP_LEFT,16,40);
    lv_obj_t *tag=make_label(g_screen,tr("Schneller lesen. Ruhiger fokussieren.","Read faster. Focus calmly."),&rsvp_unicode_14,kMuted); lv_obj_set_width(tag,190); lv_label_set_long_mode(tag,LV_LABEL_LONG_WRAP); lv_obj_align(tag,LV_ALIGN_TOP_LEFT,14,64);
    lv_obj_t *stack=lv_obj_create(g_screen); lv_obj_set_size(stack,410,116); lv_obj_align(stack,LV_ALIGN_RIGHT_MID,-8,12); lv_obj_set_style_bg_opa(stack,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(stack,0,0); lv_obj_set_style_pad_all(stack,4,0); lv_obj_set_style_pad_row(stack,6,0); lv_obj_set_flex_flow(stack,LV_FLEX_FLOW_COLUMN); lv_obj_set_scrollbar_mode(stack,LV_SCROLLBAR_MODE_AUTO); lv_obj_set_scroll_dir(stack, LV_DIR_VER); lv_obj_remove_flag(stack, LV_OBJ_FLAG_SCROLL_ONE); lv_obj_add_flag(stack, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    if(file_exists(g_last_book)){const std::string sub=display_safe(basename_no_ext(g_last_book)); make_card(stack,tr("Weiterlesen","Continue reading"),sub.c_str(),continue_cb,50);}
    char lib[96]; std::snprintf(lib,sizeof(lib),tr("%u Bücher • %s","%u books • %s"),static_cast<unsigned>(g_books.size()),format_storage().c_str()); make_card(stack,tr("Bibliothek","Library"),lib,library_cb,50);
    make_card(stack,tr("WLAN Upload","Wi-Fi upload"),tr("Hotspot + Browser","Hotspot + browser"),wifi_transfer_cb,50);
    make_card(stack,tr("Sprache","Language"),g_language==Language::German?"Deutsch":"English",language_cb,50);
    char st[96]; std::snprintf(st,sizeof(st),tr("%u WPM • Helligkeit %u%%","%u WPM • Brightness %u%%"),g_wpm,g_brightness); make_card(stack,tr("Einstellungen","Settings"),st,settings_cb,50);
}

void show_library() {
    clear_screen(); scan_books();
    lv_obj_t *back=make_small_button(g_screen,"<",home_cb,44); lv_obj_align(back,LV_ALIGN_TOP_LEFT,8,7);
    lv_obj_t *title=make_label(g_screen,tr("Bibliothek","Library"),&rsvp_unicode_24,kText); lv_obj_align(title,LV_ALIGN_TOP_LEFT,62,8);
    lv_obj_t *sub=make_label(g_screen,format_storage().c_str(),&rsvp_unicode_14,kMuted); lv_obj_align(sub,LV_ALIGN_TOP_LEFT,200,15);
    lv_obj_t *list=lv_obj_create(g_screen); lv_obj_set_size(list,624,108); lv_obj_align(list,LV_ALIGN_BOTTOM_MID,0,-4); lv_obj_set_style_bg_opa(list,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_all(list,4,0); lv_obj_set_style_pad_row(list,5,0); lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN); lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_AUTO); lv_obj_set_scroll_dir(list, LV_DIR_VER); lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ONE); lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    if(!g_sd_mounted||g_books.empty()){lv_obj_t *msg=make_label(list,!g_sd_mounted?tr("microSD nicht erkannt","microSD not detected"):tr("Keine Bücher gefunden","No books found"),&rsvp_unicode_18,kMuted);lv_obj_center(msg);return;}
    for(auto &path:g_books){lv_obj_t *row=lv_obj_create(list);lv_obj_set_size(row,590,54);lv_obj_set_style_radius(row,12,0);lv_obj_set_style_bg_color(row,lv_color_hex(kCard),0);lv_obj_set_style_border_width(row,0,0);lv_obj_set_style_pad_all(row,4,0);lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_t *open=lv_button_create(row);lv_obj_set_size(open,520,46);lv_obj_align(open,LV_ALIGN_LEFT_MID,0,0);lv_obj_set_style_bg_opa(open,LV_OPA_TRANSP,0);lv_obj_add_event_cb(open,book_cb,LV_EVENT_CLICKED,&path);
      const std::string name=display_safe(basename_no_ext(path));lv_obj_t *label=make_label(open,name.c_str(),&rsvp_unicode_18,kText);lv_obj_set_width(label,490);lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);lv_obj_align(label,LV_ALIGN_LEFT_MID,6,0);
      lv_obj_t *del=lv_button_create(row);lv_obj_set_size(del,46,46);lv_obj_align(del,LV_ALIGN_RIGHT_MID,0,0);lv_obj_set_style_radius(del,10,0);lv_obj_set_style_bg_color(del,lv_color_hex(0x2C2020),0);lv_obj_add_event_cb(del,delete_book_request_cb,LV_EVENT_CLICKED,&path);lv_obj_t *x=make_label(del,"X",&lv_font_montserrat_18,kText);lv_obj_center(x);}
}

void show_settings() {
    clear_screen();
    lv_obj_t *back = make_small_button(g_screen, "<", home_cb, 44); lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 7);
    lv_obj_t *title = make_label(g_screen, tr("Einstellungen", "Settings"), &rsvp_unicode_24, kText); lv_obj_align(title, LV_ALIGN_TOP_LEFT, 62, 8);
    lv_obj_t *list = lv_obj_create(g_screen); lv_obj_set_size(list, 624, 108); lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_all(list,4,0); lv_obj_set_style_pad_row(list,6,0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW); lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO); lv_obj_set_scroll_dir(list, LV_DIR_HOR); lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ONE); lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    auto make_setting = [&](const char *name, lv_obj_t **value_label, const char *value, lv_event_cb_t down, lv_event_cb_t up) {
        lv_obj_t *card=lv_obj_create(list); lv_obj_set_size(card, 290, 94); lv_obj_set_style_radius(card,14,0); lv_obj_set_style_bg_color(card,lv_color_hex(kCard),0); lv_obj_set_style_border_width(card,0,0); lv_obj_set_style_pad_all(card,8,0); lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *nl=make_label(card,name,&rsvp_unicode_14,kMuted); lv_obj_align(nl,LV_ALIGN_TOP_LEFT,0,0);
        *value_label=make_label(card,value,&rsvp_unicode_18,kText); lv_obj_align(*value_label,LV_ALIGN_BOTTOM_LEFT,2,-8);
        lv_obj_t *minus=make_direct_button(card,"-",down,52,42); lv_obj_align(minus,LV_ALIGN_BOTTOM_RIGHT,-58,0);
        lv_obj_t *plus=make_direct_button(card,"+",up,52,42); lv_obj_align(plus,LV_ALIGN_BOTTOM_RIGHT,0,0);
    };
    char wpm[24]; std::snprintf(wpm,sizeof(wpm),"%u WPM",g_wpm); make_setting(tr("Lesegeschwindigkeit","Reading speed"),&g_speed_label,wpm,speed_down_cb,speed_up_cb);
    char br[24]; std::snprintf(br,sizeof(br),"%u%%",g_brightness); make_setting(tr("Helligkeit","Brightness"),&g_brightness_label,br,brightness_down_cb,brightness_up_cb);
    lv_obj_t *sleep_card = lv_obj_create(list); lv_obj_set_size(sleep_card, 290, 94); lv_obj_set_style_radius(sleep_card,14,0); lv_obj_set_style_bg_color(sleep_card,lv_color_hex(kCard),0); lv_obj_set_style_border_width(sleep_card,0,0); lv_obj_set_style_pad_all(sleep_card,8,0); lv_obj_remove_flag(sleep_card,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sleep_title = make_label(sleep_card,tr("Automatisch ausschalten","Auto power off"),&rsvp_unicode_14,kMuted); lv_obj_align(sleep_title,LV_ALIGN_TOP_LEFT,0,0);
    g_sleep_label = make_label(sleep_card,tr("2 Min. Inaktivitaet","2 min. idle"),&rsvp_unicode_18,kText); lv_obj_align(g_sleep_label,LV_ALIGN_BOTTOM_LEFT,2,-8);
    char se[24]; std::snprintf(se,sizeof(se),"+%u%%",g_sentence_pause_pct); make_setting(tr("Pause am Satzende","Sentence-end pause"),&g_sentence_pause_label,se,sentence_pause_down_cb,sentence_pause_up_cb);
    char cl[24]; std::snprintf(cl,sizeof(cl),"+%u%%",g_clause_pause_pct); make_setting(tr("Pause bei , : ; -","Pause at , : ; -"),&g_clause_pause_label,cl,clause_pause_down_cb,clause_pause_up_cb);
}

void show_language() {
    clear_screen();
    lv_obj_t *back=make_small_button(g_screen,"<",home_cb,44); lv_obj_align(back,LV_ALIGN_TOP_LEFT,8,7);
    lv_obj_t *title=make_label(g_screen,tr("Sprache","Language"),&rsvp_unicode_24,kText); lv_obj_align(title,LV_ALIGN_TOP_LEFT,62,8);
    lv_obj_t *de=lv_button_create(g_screen); lv_obj_set_size(de,240,66); lv_obj_align(de,LV_ALIGN_BOTTOM_LEFT,70,-18); lv_obj_set_style_radius(de,16,0); lv_obj_set_style_bg_color(de,lv_color_hex(0x24242A),0); lv_obj_set_style_border_width(de,0,0); lv_obj_add_event_cb(de,set_language_cb,LV_EVENT_CLICKED,reinterpret_cast<void*>(0)); lv_obj_t *dl=make_label(de,g_language==Language::German?"Deutsch  *":"Deutsch",&rsvp_unicode_18,kText); lv_obj_center(dl);
    lv_obj_t *en=lv_button_create(g_screen); lv_obj_set_size(en,240,66); lv_obj_align(en,LV_ALIGN_BOTTOM_RIGHT,-70,-18); lv_obj_set_style_radius(en,16,0); lv_obj_set_style_bg_color(en,lv_color_hex(0x24242A),0); lv_obj_set_style_border_width(en,0,0); lv_obj_add_event_cb(en,set_language_cb,LV_EVENT_CLICKED,reinterpret_cast<void*>(1)); lv_obj_t *el=make_label(en,g_language==Language::English?"English  *":"English",&rsvp_unicode_18,kText); lv_obj_center(el);
}

void show_delete_confirm(const std::string &path) {
    g_pending_delete=path; clear_screen();
    lv_obj_t *title=make_label(g_screen,tr("Buch löschen?","Delete book?"),&rsvp_unicode_24,kText); lv_obj_align(title,LV_ALIGN_TOP_MID,0,8);
    const std::string name=display_safe(basename_no_ext(path)); lv_obj_t *book=make_label(g_screen,name.c_str(),&rsvp_unicode_18,kMuted); lv_obj_set_width(book,560); lv_label_set_long_mode(book,LV_LABEL_LONG_DOT); lv_obj_set_style_text_align(book,LV_TEXT_ALIGN_CENTER,0); lv_obj_align(book,LV_ALIGN_CENTER,0,-10);
    lv_obj_t *cancel=make_direct_button(g_screen,tr("Abbrechen","Cancel"),delete_cancel_cb,190,48); lv_obj_align(cancel,LV_ALIGN_BOTTOM_LEFT,90,-10);
    lv_obj_t *del=make_direct_button(g_screen,tr("Löschen","Delete"),delete_confirm_cb,190,48); lv_obj_align(del,LV_ALIGN_BOTTOM_RIGHT,-90,-10); lv_obj_set_style_bg_color(del,lv_color_hex(0xB3261E),0);
}

void show_wifi_transfer() {
    clear_screen(); g_transfer_mode=TransferMode::Wifi;
    lv_obj_t *back=make_small_button(g_screen,"<",home_cb,44); lv_obj_align(back,LV_ALIGN_TOP_LEFT,8,7);
    lv_obj_t *title=make_label(g_screen,tr("WLAN Upload","Wi-Fi upload"),&rsvp_unicode_24,kText); lv_obj_align(title,LV_ALIGN_TOP_LEFT,62,8);
    lv_obj_t *card=lv_obj_create(g_screen); lv_obj_set_size(card,500,112); lv_obj_align(card,LV_ALIGN_BOTTOM_LEFT,8,-4); lv_obj_set_style_radius(card,14,0); lv_obj_set_style_bg_color(card,lv_color_hex(kCard),0); lv_obj_set_style_border_width(card,0,0); lv_obj_set_style_pad_all(card,10,0); lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *s1=make_label(card,tr("WLAN:","Wi-Fi:"),&rsvp_unicode_14,kMuted);lv_obj_align(s1,LV_ALIGN_TOP_LEFT,0,0); lv_obj_t *ssid=make_label(card,"RSVP-Reader",&rsvp_unicode_18,kText);lv_obj_align(ssid,LV_ALIGN_TOP_LEFT,54,-2);
    lv_obj_t *s2=make_label(card,tr("Passwort:","Password:"),&rsvp_unicode_14,kMuted);lv_obj_align(s2,LV_ALIGN_TOP_LEFT,0,32); lv_obj_t *pw=make_label(card,"reader1234",&rsvp_unicode_18,kText);lv_obj_align(pw,LV_ALIGN_TOP_LEFT,90,30);
    lv_obj_t *s3=make_label(card,tr("Browser:","Browser:"),&rsvp_unicode_14,kMuted);lv_obj_align(s3,LV_ALIGN_TOP_LEFT,0,64); lv_obj_t *ip=make_label(card,"192.168.4.1",&rsvp_unicode_18,kText);lv_obj_align(ip,LV_ALIGN_TOP_LEFT,76,62);
    g_transfer_status=make_label(g_screen,tr("WLAN wird gestartet...","Starting Wi-Fi..."),&rsvp_unicode_14,kAccent);lv_obj_set_width(g_transfer_status,116);lv_label_set_long_mode(g_transfer_status,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(g_transfer_status,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(g_transfer_status,LV_ALIGN_RIGHT_MID,-8,-6);
    g_transfer_bar=lv_bar_create(g_screen);lv_obj_set_size(g_transfer_bar,110,6);lv_obj_align(g_transfer_bar,LV_ALIGN_BOTTOM_RIGHT,-10,-12);lv_bar_set_range(g_transfer_bar,0,100);lv_obj_add_flag(g_transfer_bar,LV_OBJ_FLAG_HIDDEN);
    if(!g_sd_mounted){lv_label_set_text(g_transfer_status,tr("microSD nicht erkannt","microSD not detected"));return;}
    if(!WifiTransfer::start(g_language==Language::English)){lv_label_set_text(g_transfer_status,tr("WLAN Fehler","Wi-Fi error"));return;}
    g_transfer_timer=lv_timer_create(refresh_transfer_status_cb,400,nullptr);
}

void error_back_cb(lv_event_t *) { show_library(); }

void show_loading(const char *title, const char *subtitle) {
    clear_screen(false);

    lv_obj_t *box = lv_obj_create(g_screen);
    lv_obj_set_size(box, 520, 112);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(kCard), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = make_label(box, title ? title : tr("Wird geladen", "Loading"), &rsvp_unicode_18, kText);
    lv_obj_set_width(label, 480);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, subtitle && *subtitle ? -18 : 0);

    if (subtitle && *subtitle) {
        lv_obj_t *sub = make_label(box, subtitle, &rsvp_unicode_14, kMuted);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, 460);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 18);
    }
}

void show_error(const char *title, const std::string &message) {
    clear_screen();
    lv_obj_t *t=make_label(g_screen,title,&rsvp_unicode_24,kText); lv_obj_align(t,LV_ALIGN_TOP_MID,0,8);
    lv_obj_t *m=make_label(g_screen,message.c_str(),&rsvp_unicode_14,kMuted); lv_obj_set_width(m,520); lv_label_set_long_mode(m,LV_LABEL_LONG_WRAP); lv_obj_set_style_text_align(m,LV_TEXT_ALIGN_CENTER,0); lv_obj_align(m,LV_ALIGN_CENTER,0,-8);
    lv_obj_t *back=make_small_button(g_screen,tr("Zurück","Back"),error_back_cb,120); lv_obj_align(back,LV_ALIGN_BOTTOM_MID,0,-8);
}

// Navigation is deliberately a separate screen: it does not alter reader gestures.
std::string chapter_caption(size_t index, bool selected) {
    const std::string stored = display_safe(g_reader.chapter_title(index));
    if (!stored.empty()) {
        std::string text = stored;
        if (selected) text += "  *";
        return text;
    }
    char fallback[48];
    if (g_reader.chapter_count() == 0)
        std::snprintf(fallback, sizeof(fallback), tr("Gesamtes Buch%s", "Whole book%s"), selected ? " *" : "");
    else
        std::snprintf(fallback, sizeof(fallback), tr("Kapitel %u%s", "Chapter %u%s"),
                      static_cast<unsigned>(index + 1), selected ? " *" : "");
    return fallback;
}
void chapter_jump_update() {
    if (!g_jump_slider || !g_jump_label) return;
    char text[56];
    if (g_reader.chapter_count() == 0) {
        std::snprintf(text, sizeof(text),
                      tr("Gesamtes Buch  |  %ld%%", "Whole book  |  %ld%%"),
                      static_cast<long>(lv_slider_get_value(g_jump_slider)));
    } else {
        std::snprintf(text, sizeof(text),
                      tr("Kapitel %u  |  %ld%%", "Chapter %u  |  %ld%%"),
                      static_cast<unsigned>(g_jump_chapter + 1),
                      static_cast<long>(lv_slider_get_value(g_jump_slider)));
    }
    lv_label_set_text(g_jump_label, text);
}

void chapter_slider_cb(lv_event_t *) { chapter_jump_update(); }

void chapter_cancel_cb(lv_event_t *) { build_reader_ui(); }

void chapter_apply_cb(lv_event_t *) {
    size_t start = 0, end = 0;
    if (!g_reader.chapter_bounds(g_jump_chapter, start, end)) return;
    // Jump to the exact start represented by the selected chapter entry.
    if (!g_reader.seek(start)) return;
    save_position();
    build_reader_ui(); // Playback remains paused.
}

void chapter_choose_cb(lv_event_t *event) {
    const size_t index = reinterpret_cast<size_t>(lv_event_get_user_data(event));
    if (index >= g_chapter_buttons.size()) return;
    g_jump_chapter = index;
    // Keep the list and the active touch target alive during LVGL's click callback.
    // Rebuilding the screen here could delete the pressed button mid-event.
    for (size_t i = 0; i < g_chapter_buttons.size(); ++i) {
        lv_obj_set_style_bg_color(g_chapter_buttons[i],
                                  lv_color_hex(i == index ? kAccent : kCard), 0);
        const std::string title = chapter_caption(i, i == index);
        lv_obj_t *label = lv_obj_get_child(g_chapter_buttons[i], 0);
        if (label) lv_label_set_text(label, title.c_str());
    }
}

void show_chapter_overview() {
    if (!g_reader.valid()) return;
    stop_reader();
    clear_screen(false);
    lv_obj_t *back = make_small_button(g_screen, "<", chapter_cancel_cb, 42);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 5);
    lv_obj_t *heading = make_label(g_screen, tr("Kapitel waehlen", "Choose chapter"),
                                   &rsvp_unicode_18, kText);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 60, 15);
    lv_obj_t *list = lv_obj_create(g_screen);
    lv_obj_set_size(list, 480, 110);
    lv_obj_align(list, LV_ALIGN_BOTTOM_LEFT, 8, -5);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ONE); lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    g_chapter_buttons.clear();
    const size_t count = std::max<size_t>(1, g_reader.chapter_count());
    g_jump_chapter = std::min(g_jump_chapter, count - 1);
    for (size_t i = 0; i < count; ++i) {
        const std::string title = chapter_caption(i, i == g_jump_chapter);
        lv_obj_t *button = lv_button_create(list);
        lv_obj_set_size(button, 450, 40);
        lv_obj_set_style_min_height(button, 40, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(button, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_style_bg_color(button, lv_color_hex(i == g_jump_chapter ? kAccent : kCard), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_add_event_cb(button, chapter_choose_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(i));
        g_chapter_buttons.push_back(button);
        lv_obj_t *label = make_label(button, title.c_str(), &rsvp_unicode_12, kText);
        lv_obj_set_width(label, 420);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *go = make_small_button(g_screen, tr("Oeffnen", "Open"), chapter_apply_cb, 130);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, -10, -7);
}

void chapter_open_cb(lv_event_t *) {
    if (!g_reader.valid()) return;
    const size_t current = g_reader.chapter_index();
    g_jump_chapter = current > 0 ? current - 1 : 0;
    show_chapter_overview();
}

void build_reader_ui() {
    clear_screen();
    lv_obj_t *back=make_small_button(g_screen,"<",home_cb,42); lv_obj_align(back,LV_ALIGN_TOP_LEFT,6,6);
    const std::string safe_title=display_safe(g_active_book_title); lv_obj_t *title=make_label(g_screen,safe_title.c_str(),&rsvp_unicode_14,kText); lv_obj_set_width(title,260); lv_obj_set_height(title,18); lv_label_set_long_mode(title,LV_LABEL_LONG_DOT); lv_obj_align(title,LV_ALIGN_TOP_LEFT,55,11);
    g_progress_label=make_label(g_screen,"0%",&rsvp_unicode_14,kMuted); lv_obj_set_width(g_progress_label,120); lv_obj_align(g_progress_label,LV_ALIGN_TOP_RIGHT,-100,11);
    // Tap the chapter/progress indicator to open chapter navigation.
    lv_obj_set_height(g_progress_label, 30);
    lv_obj_set_style_text_align(g_progress_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(g_progress_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_progress_label, reader_menu_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(ReaderMenuAction::Chapter)));
    g_progress_bar=lv_bar_create(g_screen); lv_obj_set_size(g_progress_bar,520,3); lv_obj_align(g_progress_bar,LV_ALIGN_TOP_MID,0,39); lv_bar_set_range(g_progress_bar,0,100);
    lv_obj_set_style_bg_color(g_progress_bar,lv_color_hex(0x2C2C2E),LV_PART_MAIN); lv_obj_set_style_bg_color(g_progress_bar,lv_color_hex(kAccent),LV_PART_INDICATOR);
    lv_obj_t *reader_area=lv_obj_create(g_screen); lv_obj_set_size(reader_area,620,82); lv_obj_align(reader_area,LV_ALIGN_CENTER,0,8); lv_obj_set_style_bg_opa(reader_area,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(reader_area,0,0); lv_obj_set_style_pad_all(reader_area,0,0); lv_obj_remove_flag(reader_area,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    g_before=make_label(reader_area,"",&lv_font_montserrat_28,kText); g_orp=make_label(reader_area,"",&lv_font_montserrat_28,kOrp); g_after=make_label(reader_area,"",&lv_font_montserrat_28,kText);
    lv_obj_t *focus_top=lv_obj_create(reader_area); lv_obj_set_size(focus_top,2,8); lv_obj_set_style_bg_color(focus_top,lv_color_hex(kMuted),0); lv_obj_set_style_border_width(focus_top,0,0); lv_obj_align(focus_top,LV_ALIGN_CENTER,0,-38); lv_obj_remove_flag(focus_top,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t *focus_bottom=lv_obj_create(reader_area); lv_obj_set_size(focus_bottom,2,10); lv_obj_set_style_bg_color(focus_bottom,lv_color_hex(kMuted),0); lv_obj_set_style_border_width(focus_bottom,0,0); lv_obj_align(focus_bottom,LV_ALIGN_CENTER,0,25); lv_obj_remove_flag(focus_bottom,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    g_speed_label=make_label(g_screen,"",&lv_font_montserrat_14,kMuted); lv_obj_set_width(g_speed_label,80); lv_obj_set_height(g_speed_label,30); lv_obj_set_style_text_align(g_speed_label,LV_TEXT_ALIGN_CENTER,0); lv_obj_align(g_speed_label,LV_ALIGN_TOP_LEFT,330,11); lv_obj_add_flag(g_speed_label,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(g_speed_label,reader_menu_event_cb,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<uintptr_t>(ReaderMenuAction::Wpm)));

    // Dedicated previous/next controls. Word navigation no longer depends on
    // horizontal swipe recognition.
    lv_obj_t *prev=make_small_button(g_screen,LV_SYMBOL_PREV,reader_previous_cb,82); lv_obj_set_height(prev,44); lv_obj_align(prev,LV_ALIGN_BOTTOM_LEFT,6,-4);
    lv_obj_t *next=make_small_button(g_screen,LV_SYMBOL_NEXT,reader_next_cb,82); lv_obj_set_height(next,44); lv_obj_align(next,LV_ALIGN_BOTTOM_RIGHT,-6,-4);
    lv_obj_t *play=make_small_button(g_screen,LV_SYMBOL_PLAY,reader_play_cb,48); lv_obj_set_height(play,36); lv_obj_align(play,LV_ALIGN_BOTTOM_MID,0,-4);
    // Act once on confirmed contact, without waiting for release. The BSP
    // debounces the contact even while the first reader frame is being drawn.
    lv_obj_remove_event_cb(play, reader_play_cb);
    lv_obj_add_event_cb(play, reader_play_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_set_ext_click_area(play, 16);
    lv_obj_set_style_radius(play,18,0);
    lv_obj_set_style_bg_color(play,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_bg_opa(play,LV_OPA_20,0);
    lv_obj_set_style_bg_color(play,lv_color_hex(0xFFFFFF),LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(play,LV_OPA_40,LV_STATE_PRESSED);
    lv_obj_set_style_border_width(play,1,0);
    lv_obj_set_style_border_color(play,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_border_opa(play,LV_OPA_50,0);
    lv_obj_set_style_shadow_width(play,12,0);
    lv_obj_set_style_shadow_color(play,lv_color_hex(kAccent),0);
    lv_obj_set_style_shadow_opa(play,LV_OPA_20,0);
    g_play_icon=lv_obj_get_child(play,0);

    g_overlay=make_label(g_screen,"",&rsvp_unicode_18,kText); lv_obj_set_style_bg_color(g_overlay,lv_color_hex(0x27272D),0); lv_obj_set_style_bg_opa(g_overlay,LV_OPA_90,0); lv_obj_set_style_radius(g_overlay,10,0); lv_obj_set_style_pad_all(g_overlay,8,0); lv_obj_align(g_overlay,LV_ALIGN_BOTTOM_RIGHT,-10,-10); lv_obj_add_flag(g_overlay,LV_OBJ_FLAG_HIDDEN);
    g_reader_mode=true;
    std::string word; if(g_reader.current(word)) render_word(word);
    g_reader_timer=lv_timer_create(reader_tick_cb,ReaderEngine::delay_ms(word,g_wpm,g_sentence_pause_pct,g_clause_pause_pct),nullptr); lv_timer_pause(g_reader_timer);
    g_overlay_timer=lv_timer_create(overlay_hide_cb,650,nullptr); lv_timer_pause(g_overlay_timer);
}

void show_worker_error(const char *title, const std::string &message) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (bsp_display_lock(250)) {
            g_book_loading = false;
            show_error(title, message);
            bsp_display_unlock();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    ESP_LOGE(TAG, "Could not acquire LVGL lock to show error");
    g_book_loading = false;
}

void book_open_task(void *arg) {
    std::string path = *static_cast<std::string *>(arg);
    delete static_cast<std::string *>(arg);

    g_active_book_key = path;
    g_active_book_title = basename_no_ext(path);
    g_last_book = path;
    remember_recent(path);
    save_settings();

    ESP_LOGI(TAG, "Book worker started on core %d: %s", xPortGetCoreID(), path.c_str());

    std::string open_path = path;
    if (has_extension(path, ".epub")) {
        ESP_LOGI(TAG, "Opening EPUB: %s", path.c_str());
        ESP_LOGI(TAG, "Heap before EPUB: internal=%u, psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

        char cache_path[96];
        std::snprintf(cache_path, sizeof(cache_path), "%s/.rsvp6_%08lx.txt", BSP_SD_MOUNT_POINT,
                      static_cast<unsigned long>(fnv1a(path.c_str())));
        std::string error;
        if (!EpubReader::to_text_cache(path.c_str(), cache_path, error)) {
            ESP_LOGE(TAG, "EPUB conversion failed: %s", error.c_str());
            show_worker_error(tr("EPUB konnte nicht geoeffnet werden", "Could not open EPUB"), error);
            vTaskDelete(nullptr);
            return;
        }
        open_path = cache_path;
        ESP_LOGI(TAG, "EPUB cache ready: %s", open_path.c_str());
        ESP_LOGI(TAG, "Heap after EPUB: internal=%u, psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    const size_t position = load_position(path);
    if (!g_reader.open(open_path.c_str(), position)) {
        ESP_LOGE(TAG, "ReaderEngine open failed: %s", open_path.c_str());
        show_worker_error(tr("Buch konnte nicht geoeffnet werden", "Could not open book"),
                          tr("Die Datei ist leer, zu gross oder konnte nicht gelesen werden.",
                             "The file is empty, too large, or could not be read."));
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Book loaded: %u bytes", static_cast<unsigned>(g_reader.length()));

    bool shown = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (bsp_display_lock(250)) {
            build_reader_ui();
            g_book_loading = false;
            bsp_display_unlock();
            shown = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (!shown) {
        ESP_LOGE(TAG, "Could not acquire LVGL lock to show reader");
        g_book_loading = false;
    } else {
        ESP_LOGI(TAG, "Reader UI shown");
    }

    vTaskDelete(nullptr);
}

void start_book_open(const std::string &path) {
    if (g_book_loading || path.empty()) return;
    g_book_loading = true;

    const std::string title = basename_no_ext(path);
    show_loading(has_extension(path, ".epub") ? tr("EPUB wird vorbereitet", "Preparing EPUB") : tr("Buch wird geladen", "Loading book"), title.c_str());

    auto *request = new (std::nothrow) std::string(path);
    if (!request) {
        g_book_loading = false;
        show_error(tr("Nicht genug Speicher", "Not enough memory"), tr("Das Buch konnte nicht zum Laden vorbereitet werden.", "The book could not be prepared for loading."));
        return;
    }

    const BaseType_t result = xTaskCreatePinnedToCore(
        book_open_task,
        "book_open",
        16384,
        request,
        3,
        nullptr,
        1);

    if (result != pdPASS) {
        delete request;
        g_book_loading = false;
        ESP_LOGE(TAG, "Could not create book_open task");
        show_error(tr("Buch konnte nicht geoeffnet werden", "Could not open book"), tr("Hintergrund-Task konnte nicht gestartet werden.", "Background task could not be started."));
    }
}

}  // namespace

extern "C" void app_main(void) {
    const int64_t startup_us = esp_timer_get_time();
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_16, GPIO_PULLUP_ONLY);
    const bool pwr_start = bsp_pwr_button_pressed() || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    configure_power_saving();
    load_settings();
    transfer_common_init();

    ESP_LOGI(TAG, "Simple RSVP Reader 3.49B V2 RC3 starting");
    ESP_LOGI(TAG, "Target: Waveshare ESP32-S3 Touch-LCD-3.49B V2 / AXS15231B");

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }
    if (pwr_start) {
        bsp_display_brightness_set(0);
        if (!bsp_pwr_button_pressed()) { bsp_power_off(); return; }
        while (esp_timer_get_time() - startup_us < 2000000) {
            if (!bsp_pwr_button_pressed()) { bsp_power_off(); return; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    g_touch_indev = bsp_display_get_input_dev();
    if (g_touch_indev) lv_indev_set_long_press_time(g_touch_indev, 1000);
    if (g_touch_indev) {
        // Ignore small coordinate jitter while picking up deliberate drags promptly.
        lv_indev_set_scroll_limit(g_touch_indev, 6);
        lv_indev_set_scroll_throw(g_touch_indev, 12);
        ESP_LOGI(TAG, "Reader touch polling initialized");
    } else {
        ESP_LOGE(TAG, "BSP touch input device unavailable");
    }
    g_last_activity_us = esp_timer_get_time();

    // 3.49B V2 battery ADC and GPIO16 PWR monitoring. Failure is non-fatal;
    // the UI will show --% and PWR control will be unavailable.
    if (!BatteryMonitor::start()) {
        ESP_LOGW(TAG, "Battery/PWR monitor unavailable");
    } else if (!BatteryMonitor::power_key_ready()) {
        ESP_LOGW(TAG, "PWR control unavailable; battery telemetry remains active");
    }
    if (!RtcClock::start()) {
        ESP_LOGW(TAG, "RTC unavailable; clock will show --:--");
    }

    const esp_err_t sd_result = bsp_sdcard_mount();
    g_sd_mounted = sd_result == ESP_OK;
    if (g_sd_mounted) ESP_LOGI(TAG, "microSD mounted: %s", BSP_SD_MOUNT_POINT);
    else ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(sd_result));

    if (!bsp_display_lock(UINT32_MAX)) {
        ESP_LOGE(TAG, "Could not acquire LVGL lock");
        return;
    }
    lv_timer_t *power_timer = lv_timer_create(power_timer_cb, 100, nullptr);
    (void)power_timer;
    clear_screen(false);
    lv_obj_t *on = make_label(g_screen, "ON", &lv_font_montserrat_48, kText);
    lv_obj_center(on);
    // Hide retained panel RAM (including an old diagnostic frame) until the
    // first application screen has completed its synchronous DMA flush.
    lv_refr_now(display);
    ESP_ERROR_CHECK(bsp_display_brightness_set(g_brightness));
    ESP_LOGI(TAG, "ON frame ready; backlight enabled");
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(700));
    if (bsp_display_lock(UINT32_MAX)) {
        show_home();
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "UI ready");
}
